#include "CramionCore/replay/Replay.h"

#include "CramionCore/anim/MotionMatching.h"
#include "CramionCore/audio/Audio.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"

#include <CramionDM/Input.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>

namespace cramion::replay {

using core::Quat;
using core::Vec3;
using nlohmann::json;

namespace {

ReplaySystem* g_active = nullptr;

constexpr std::uint32_t kNone = 0xFFFFFFFFu;
constexpr float kPositionStep = 0.0005f;  // medio milimetro
constexpr char kMagic[8] = {'C', 'R', 'R', 'E', 'P', 'L', 'A', 'Y'};
constexpr std::uint32_t kVersion = 1;

// Banderas de cada registro (varint).
enum : std::uint32_t {
    kPos = 1u << 0,
    kRot = 1u << 1,
    kScale = 1u << 2,
    kActiveChanged = 1u << 3,
    kActiveOn = 1u << 4,
    kAnim = 1u << 5,
    kPosAbsolute = 1u << 6,
    kRemoved = 1u << 7,
    kParams = 1u << 8,
    kAliveStart = 1u << 9,
};

// Componentes que una copia (objeto que ya no existe) no debe llevar: es
// solo para verse.
constexpr const char* kGhostStrip[] = {"Script",  "CppScript", "StateMachine", "BehaviorTree", "Rigidbody",
                                       "CharacterController", "NavAgent", "NetworkObject", "Vehicle",
                                       "VisualScript"};

std::uint32_t key(entt::entity e) { return static_cast<std::uint32_t>(e); }

// --- Escritura / lectura de bytes ------------------------------------------------

void putU8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }

void putVarint(std::vector<std::uint8_t>& out, std::uint32_t v) {
    while (v >= 0x80u) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80u));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void putSigned(std::vector<std::uint8_t>& out, std::int32_t v) {
    putVarint(out, (static_cast<std::uint32_t>(v) << 1) ^ static_cast<std::uint32_t>(v >> 31));
}

void putI16(std::vector<std::uint8_t>& out, std::int16_t v) {
    const std::uint16_t u = static_cast<std::uint16_t>(v);
    out.push_back(static_cast<std::uint8_t>(u & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(u >> 8));
}

void putF32(std::vector<std::uint8_t>& out, float v) {
    std::uint8_t b[4];
    std::memcpy(b, &v, 4);
    out.insert(out.end(), b, b + 4);
}

struct Reader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t at = 0;
    bool ok = true;

    std::uint8_t u8() {
        if (at >= size) {
            ok = false;
            return 0;
        }
        return data[at++];
    }
    std::uint32_t varint() {
        std::uint32_t v = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            const std::uint8_t b = u8();
            v |= static_cast<std::uint32_t>(b & 0x7Fu) << shift;
            if ((b & 0x80u) == 0) break;
        }
        return v;
    }
    std::int32_t signedVarint() {
        const std::uint32_t u = varint();
        return static_cast<std::int32_t>((u >> 1) ^ (~(u & 1u) + 1u));
    }
    std::int16_t i16() {
        const std::uint16_t lo = u8();
        const std::uint16_t hi = u8();
        return static_cast<std::int16_t>(static_cast<std::uint16_t>(lo | (hi << 8)));
    }
    float f32() {
        float v = 0.0f;
        if (at + 4 > size) {
            ok = false;
            at = size;
            return 0.0f;
        }
        std::memcpy(&v, data + at, 4);
        at += 4;
        return v;
    }
};

// Cuaternion de "los tres pequenos": el componente mayor se reconstruye.
struct PackedQuat {
    std::uint8_t largest = 3;
    std::int16_t a = 0, b = 0, c = 0;
};
constexpr float kQuatScale = 32767.0f * 1.41421356f;

PackedQuat packQuat(Quat q) {
    q = core::normalize(q);
    const float v[4] = {q.x, q.y, q.z, q.w};
    int largest = 0;
    for (int i = 1; i < 4; ++i) {
        if (std::abs(v[i]) > std::abs(v[largest])) largest = i;
    }
    const float sign = v[largest] < 0.0f ? -1.0f : 1.0f;
    PackedQuat p;
    p.largest = static_cast<std::uint8_t>(largest);
    std::int16_t* slots[3] = {&p.a, &p.b, &p.c};
    int k = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == largest) continue;
        const float x = std::clamp(v[i] * sign * kQuatScale, -32767.0f, 32767.0f);
        *slots[k++] = static_cast<std::int16_t>(std::lround(x));
    }
    return p;
}

Quat unpackQuat(const PackedQuat& p) {
    const float small[3] = {p.a / kQuatScale, p.b / kQuatScale, p.c / kQuatScale};
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int k = 0;
    float sum = 0.0f;
    for (int i = 0; i < 4; ++i) {
        if (i == p.largest) continue;
        v[i] = small[k++];
        sum += v[i] * v[i];
    }
    v[std::min<int>(p.largest, 3)] = std::sqrt(std::max(0.0f, 1.0f - sum));
    return core::normalize(Quat{v[0], v[1], v[2], v[3]});
}

void putQuat(std::vector<std::uint8_t>& out, const PackedQuat& p) {
    putU8(out, p.largest);
    putI16(out, p.a);
    putI16(out, p.b);
    putI16(out, p.c);
}

PackedQuat readQuat(Reader& r) {
    PackedQuat p;
    p.largest = static_cast<std::uint8_t>(r.u8() & 3u);
    p.a = r.i16();
    p.b = r.i16();
    p.c = r.i16();
    return p;
}

float quatDot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

Quat nlerp(const Quat& a, Quat b, float t) {
    if (quatDot(a, b) < 0.0f) b = Quat{-b.x, -b.y, -b.z, -b.w};
    return core::normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                                a.w + (b.w - a.w) * t});
}

bool passesTag(ecs::Entity e, const std::string& tag) {
    if (tag.empty()) return true;
    int guard = 0;
    for (ecs::Entity a = e; a.valid() && guard++ < 32; a = a.parent()) {
        if (a.compareTag(tag)) return true;
    }
    return false;
}

}  // namespace

ReplaySystem* activeSystem() { return g_active; }
void setActiveSystem(ReplaySystem* system) { g_active = system; }

ReplaySystem::ReplaySystem() = default;
ReplaySystem::~ReplaySystem() {
    if (g_active == this) g_active = nullptr;
}

// -----------------------------------------------------------------------------
// Grabar
// -----------------------------------------------------------------------------

bool ReplaySystem::startRecording(ecs::World& world, const ReplayOptions& options) {
    if (playing_) stop(world);
    clear();
    options_ = options;
    options_.rate = std::clamp(options_.rate, 1.0f, 120.0f);
    options_.keyframe_interval = std::clamp(options_.keyframe_interval, 2, 600);
    options_.max_seconds = std::max(options_.max_seconds, 0.0f);
    scene_name_ = world.sceneName();
    recording_ = true;
    record_time_ = 0.0f;
    sample_accumulator_ = 0.0f;
    samples_since_key_ = 0;
    // Lo que ya existe: se vigila; los animados se graban desde el principio.
    for (const entt::entity h : world.registry().view<ecs::Transform>()) {
        const ecs::Entity e = world.wrap(h);
        Watch w;
        w.version = e.get<ecs::Transform>().version;
        w.active = e.activeSelf();
        watched_[key(h)] = w;
    }
    recordSample(world);
    return true;
}

void ReplaySystem::stopRecording() {
    recording_ = false;
    watched_.clear();
    audio_playing_.clear();
}

void ReplaySystem::notifyOneShot(const std::string& clip, const Vec3& position, float volume, bool spatial) {
    if (!recording_ || !options_.record_audio) return;
    ReplayEvent ev;
    ev.kind = ReplayEvent::Kind::OneShot;
    ev.time = record_time_;
    ev.name = clip;
    ev.position = position;
    ev.volume = volume;
    ev.spatial = spatial;
    events_.push_back(std::move(ev));
}

void ReplaySystem::mark(const std::string& name, const std::string& data) {
    ReplayEvent ev;
    ev.kind = ReplayEvent::Kind::Marker;
    ev.time = recording_ ? record_time_ : (frames_.empty() ? 0.0f : frames_.front().time + time_);
    ev.name = name;
    ev.data = data;
    events_.push_back(std::move(ev));
    std::stable_sort(events_.begin(), events_.end(),
                     [](const ReplayEvent& a, const ReplayEvent& b) { return a.time < b.time; });
}

void ReplaySystem::recordSample(ecs::World& world) {
    Frame frame;
    frame.time = record_time_;
    frame.origin[0] = world.origin().x;
    frame.origin[1] = world.origin().y;
    frame.origin[2] = world.origin().z;
    frame.offset = static_cast<std::uint32_t>(stream_.size());
    frame.keyframe = frames_.empty() || samples_since_key_ >= options_.keyframe_interval;
    samples_since_key_ = frame.keyframe ? 1 : samples_since_key_ + 1;

    // Objetos nuevos o que empiezan a moverse: pista nueva.
    std::vector<std::uint32_t> handles(tracks_.size(), kNone);
    std::unordered_map<std::uint32_t, int> track_of;
    for (const auto& [k, w] : watched_) {
        if (w.track >= 0) track_of[k] = w.track;
    }
    for (const entt::entity h : world.registry().view<ecs::Transform>()) {
        const ecs::Entity e = world.wrap(h);
        const ecs::Transform& t = e.get<ecs::Transform>();
        auto it = watched_.find(key(h));
        const bool fresh = it == watched_.end();
        if (fresh) {
            Watch w;
            w.version = t.version;
            w.active = e.activeSelf();
            w.spawned = true;
            it = watched_.emplace(key(h), w).first;
        }
        Watch& w = it->second;
        if (w.track < 0) {
            const bool animated = options_.record_animation &&
                                  (e.has<ecs::Animator>() || e.has<anim::MotionMatching>());
            const bool changed = fresh || w.version != t.version || w.active != e.activeSelf() || animated;
            if (changed && passesTag(e, options_.tag)) {
                Track track;
                track.uuid = e.uuid();
                track.name = e.name();
                track.parent = e.parent().valid() ? e.parent().uuid() : Uuid{};
                track.spawned = w.spawned;
                // Lo que aparece en la partida guarda una copia (si su padre no
                // aparecio tambien: entonces va dentro de la copia del padre).
                if (w.spawned && tracks_.size() < 4096) {
                    const ecs::Entity parent = e.parent();
                    const auto parent_watch = parent.valid() ? watched_.find(key(parent.handle())) : watched_.end();
                    const bool parent_spawned = parent_watch != watched_.end() && parent_watch->second.spawned;
                    if (!parent_spawned) track.snapshot = ecs::serializeEntity(world, e);
                }
                w.track = static_cast<int>(tracks_.size());
                tracks_.push_back(std::move(track));
                encoded_.push_back(TrackState{});
                handles.push_back(kNone);
            }
        }
        w.version = t.version;
        w.active = e.activeSelf();
        if (w.track >= 0) handles[static_cast<std::size_t>(w.track)] = key(h);
    }

    // Registros.
    std::vector<std::uint8_t> body;
    std::uint32_t records = 0;
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        TrackState& enc = encoded_[i];
        const std::uint32_t handle = handles[i];
        if (handle == kNone) {
            // Ya no existe: se quita una vez.
            if (enc.alive && !frame.keyframe) {
                putVarint(body, static_cast<std::uint32_t>(i));
                putVarint(body, kRemoved);
                ++records;
            }
            enc.alive = false;
            continue;
        }
        const ecs::Entity e = world.wrap(static_cast<entt::entity>(handle));
        const ecs::Transform& t = e.get<ecs::Transform>();
        const bool full = frame.keyframe || !enc.alive;
        std::uint32_t flags = full ? (kAliveStart | kPos | kPosAbsolute | kRot | kScale | kActiveChanged) : 0u;

        // Posicion.
        std::int16_t q[3] = {0, 0, 0};
        if (!full) {
            const Vec3 d = t.position - enc.position;
            const float m = std::max({std::abs(d.x), std::abs(d.y), std::abs(d.z)});
            if (m > kPositionStep * 0.5f) {
                flags |= kPos;
                const float lim = 32767.0f * kPositionStep;
                if (m < lim) {
                    q[0] = static_cast<std::int16_t>(std::lround(d.x / kPositionStep));
                    q[1] = static_cast<std::int16_t>(std::lround(d.y / kPositionStep));
                    q[2] = static_cast<std::int16_t>(std::lround(d.z / kPositionStep));
                } else {
                    flags |= kPosAbsolute;
                }
            }
        }
        // Giro.
        const PackedQuat packed = packQuat(t.rotation);
        const Quat unpacked = unpackQuat(packed);
        if (!full && std::abs(quatDot(unpacked, enc.rotation)) < 0.9999995f) flags |= kRot;
        // Escala.
        if (!full) {
            const Vec3 d = t.scale - enc.scale;
            if (std::max({std::abs(d.x), std::abs(d.y), std::abs(d.z)}) > 1e-5f) flags |= kScale;
        }
        // Activo.
        const bool active = e.activeSelf();
        if (!full && active != enc.active) flags |= kActiveChanged;
        if ((flags & kActiveChanged) != 0 && active) flags |= kActiveOn;
        // Animacion.
        int anim_kind = 0, anim_state = -1;
        float anim_time = 0.0f;
        const ecs::Animator* animator = options_.record_animation ? e.tryGet<ecs::Animator>() : nullptr;
        const anim::MotionMatching* mm = options_.record_animation ? e.tryGet<anim::MotionMatching>() : nullptr;
        if (mm != nullptr && mm->enabled && mm->database.valid() && mm->runtime.entry >= 0) {
            anim_kind = 2;
            anim_state = mm->runtime.entry;
            anim_time = mm->runtime.time;
        } else if (animator != nullptr) {
            anim_kind = 1;
            anim_state = animator->controller.valid() ? animator->runtime.state : animator->clip;
            anim_time = animator->time;
        }
        if (anim_kind != 0 && (full || anim_kind != enc.anim_kind || anim_state != enc.anim_state ||
                               std::abs(anim_time - enc.anim_time) > 1e-4f)) {
            flags |= kAnim;
        }
        // Parametros del Animator Controller (los Blend Trees los usan).
        std::vector<std::pair<int, float>> changed_params;
        if (animator != nullptr && anim_kind == 1 && animator->controller.valid()) {
            for (const auto& [name, value] : animator->runtime.values) {
                int index = -1;
                for (std::size_t p = 0; p < param_names_.size(); ++p) {
                    if (param_names_[p] == name) {
                        index = static_cast<int>(p);
                        break;
                    }
                }
                if (index < 0) {
                    index = static_cast<int>(param_names_.size());
                    param_names_.push_back(name);
                }
                const auto old = std::find_if(enc.params.begin(), enc.params.end(),
                                              [&](const std::pair<int, float>& x) { return x.first == index; });
                if (full || old == enc.params.end() || std::abs(old->second - value) > 1e-5f) {
                    changed_params.emplace_back(index, value);
                }
            }
            if (!changed_params.empty()) flags |= kParams;
        }
        raw_bytes_ += 48;
        if (flags == 0) continue;

        putVarint(body, static_cast<std::uint32_t>(i));
        putVarint(body, flags);
        if ((flags & kPos) != 0) {
            if ((flags & kPosAbsolute) != 0) {
                putF32(body, t.position.x);
                putF32(body, t.position.y);
                putF32(body, t.position.z);
                enc.position = t.position;
            } else {
                putI16(body, q[0]);
                putI16(body, q[1]);
                putI16(body, q[2]);
                enc.position = enc.position + Vec3{q[0] * kPositionStep, q[1] * kPositionStep, q[2] * kPositionStep};
            }
        }
        if ((flags & kRot) != 0) {
            putQuat(body, packed);
            enc.rotation = unpacked;
        }
        if ((flags & kScale) != 0) {
            putF32(body, t.scale.x);
            putF32(body, t.scale.y);
            putF32(body, t.scale.z);
            enc.scale = t.scale;
        }
        if ((flags & kActiveChanged) != 0) enc.active = active;
        if ((flags & kAnim) != 0) {
            putU8(body, static_cast<std::uint8_t>(anim_kind));
            putSigned(body, anim_state);
            putF32(body, anim_time);
            enc.anim_kind = anim_kind;
            enc.anim_state = anim_state;
            enc.anim_time = anim_time;
        }
        if ((flags & kParams) != 0) {
            putVarint(body, static_cast<std::uint32_t>(changed_params.size()));
            for (const auto& [index, value] : changed_params) {
                putVarint(body, static_cast<std::uint32_t>(index));
                putF32(body, value);
                auto old = std::find_if(enc.params.begin(), enc.params.end(),
                                        [&](const std::pair<int, float>& x) { return x.first == index; });
                if (old != enc.params.end()) {
                    old->second = value;
                } else {
                    enc.params.emplace_back(index, value);
                }
            }
        }
        enc.alive = true;
        ++records;
    }
    putVarint(stream_, records);
    stream_.insert(stream_.end(), body.begin(), body.end());
    frames_.push_back(frame);

    // Sonidos de los Audio Source (empiezan / paran).
    if (audio_ != nullptr && options_.record_audio) {
        for (const entt::entity h : world.registry().view<audio::AudioSource>()) {
            const ecs::Entity e = world.wrap(h);
            if (!passesTag(e, options_.tag)) continue;
            const bool now = audio_->isPlaying(e);
            bool& before = audio_playing_[key(h)];
            if (now != before) {
                ReplayEvent ev;
                ev.kind = now ? ReplayEvent::Kind::SourcePlay : ReplayEvent::Kind::SourceStop;
                ev.time = record_time_;
                ev.entity = e.uuid();
                ev.name = e.name();
                events_.push_back(std::move(ev));
                before = now;
            }
        }
    }
    // Lo vigilado que ya no existe.
    if (frame.keyframe) {
        for (auto it = watched_.begin(); it != watched_.end();) {
            it = world.registry().valid(static_cast<entt::entity>(it->first)) ? std::next(it) : watched_.erase(it);
        }
    }
    trim();
}

void ReplaySystem::trim() {
    if (options_.max_seconds <= 0.0f || frames_.size() < 2) return;
    const float keep_from = record_time_ - options_.max_seconds;
    // El primer fotograma completo a partir del que se conserva todo.
    int cut = -1;
    for (std::size_t i = 1; i < frames_.size(); ++i) {
        if (!frames_[i].keyframe) continue;
        if (frames_[i].time > keep_from) break;
        cut = static_cast<int>(i);
    }
    if (cut <= 0) return;
    const std::uint32_t offset = frames_[static_cast<std::size_t>(cut)].offset;
    stream_.erase(stream_.begin(), stream_.begin() + offset);
    frames_.erase(frames_.begin(), frames_.begin() + cut);
    for (Frame& f : frames_) f.offset -= offset;
    const float first = frames_.front().time;
    events_.erase(std::remove_if(events_.begin(), events_.end(), [&](const ReplayEvent& e) { return e.time < first; }),
                  events_.end());
    decoded_index_ = -1;
}

// -----------------------------------------------------------------------------
// Decodificar
// -----------------------------------------------------------------------------

void ReplaySystem::decodeFrame(int index, std::vector<TrackState>& states) const {
    if (index < 0 || index >= static_cast<int>(frames_.size())) return;
    if (states.size() < tracks_.size()) states.resize(tracks_.size());
    const Frame& frame = frames_[static_cast<std::size_t>(index)];
    const std::size_t end = static_cast<std::size_t>(index) + 1 < frames_.size()
                                ? frames_[static_cast<std::size_t>(index) + 1].offset
                                : stream_.size();
    Reader r{stream_.data(), end, frame.offset, true};
    if (frame.keyframe) {
        for (TrackState& s : states) s.alive = false;
    }
    const std::uint32_t records = r.varint();
    for (std::uint32_t n = 0; n < records && r.ok; ++n) {
        const std::uint32_t track = r.varint();
        const std::uint32_t flags = r.varint();
        TrackState dummy;
        TrackState& s = track < states.size() ? states[track] : dummy;
        if ((flags & kRemoved) != 0) {
            s.alive = false;
            continue;
        }
        if ((flags & kPos) != 0) {
            if ((flags & kPosAbsolute) != 0) {
                const float x = r.f32();
                const float y = r.f32();
                const float z = r.f32();
                s.position = Vec3{x, y, z};
            } else {
                const std::int16_t x = r.i16();
                const std::int16_t y = r.i16();
                const std::int16_t z = r.i16();
                s.position = s.position + Vec3{x * kPositionStep, y * kPositionStep, z * kPositionStep};
            }
        }
        if ((flags & kRot) != 0) s.rotation = unpackQuat(readQuat(r));
        if ((flags & kScale) != 0) {
            const float x = r.f32();
            const float y = r.f32();
            const float z = r.f32();
            s.scale = Vec3{x, y, z};
        }
        if ((flags & kActiveChanged) != 0) s.active = (flags & kActiveOn) != 0;
        if ((flags & kAnim) != 0) {
            s.anim_kind = r.u8();
            s.anim_state = r.signedVarint();
            s.anim_time = r.f32();
        }
        if ((flags & kParams) != 0) {
            const std::uint32_t count = r.varint();
            for (std::uint32_t p = 0; p < count && r.ok; ++p) {
                const int index = static_cast<int>(r.varint());
                const float value = r.f32();
                auto old = std::find_if(s.params.begin(), s.params.end(),
                                        [&](const std::pair<int, float>& x) { return x.first == index; });
                if (old != s.params.end()) {
                    old->second = value;
                } else {
                    s.params.emplace_back(index, value);
                }
            }
        }
        if ((flags & kAliveStart) != 0 || (flags & ~kRemoved) != 0) s.alive = true;
    }
}

bool ReplaySystem::decodeTo(int index) {
    if (frames_.empty()) return false;
    index = std::clamp(index, 0, static_cast<int>(frames_.size()) - 1);
    if (index == decoded_index_) return true;
    int start = 0;
    if (decoded_index_ >= 0 && index > decoded_index_ && index - decoded_index_ <= options_.keyframe_interval * 2) {
        start = decoded_index_ + 1;
    } else {
        // Desde el fotograma completo anterior.
        start = index;
        while (start > 0 && !frames_[static_cast<std::size_t>(start)].keyframe) --start;
        decoded_.assign(tracks_.size(), TrackState{});
    }
    for (int i = start; i <= index; ++i) decodeFrame(i, decoded_);
    decoded_index_ = index;
    return true;
}

float ReplaySystem::duration() const {
    return frames_.size() < 2 ? 0.0f : frames_.back().time - frames_.front().time;
}

ReplayInfo ReplaySystem::info() const {
    ReplayInfo i;
    i.duration = duration();
    i.frames = static_cast<int>(frames_.size());
    i.tracks = static_cast<int>(tracks_.size());
    i.events = static_cast<int>(events_.size());
    i.bytes = stream_.size();
    i.raw_bytes = raw_bytes_;
    i.scene = scene_name_;
    return i;
}

void ReplaySystem::clear() {
    recording_ = false;
    tracks_.clear();
    frames_.clear();
    stream_.clear();
    events_.clear();
    encoded_.clear();
    watched_.clear();
    audio_playing_.clear();
    param_names_.clear();
    raw_bytes_ = 0;
    decoded_index_ = -1;
    decoded_.clear();
    time_ = 0.0f;
}

// -----------------------------------------------------------------------------
// Reproducir
// -----------------------------------------------------------------------------

void ReplaySystem::bindEntities(ecs::World& world) {
    bound_.assign(tracks_.size(), kNone);
    ghosts_.clear();
    std::unordered_map<Uuid, std::size_t> track_of;
    for (std::size_t i = 0; i < tracks_.size(); ++i) track_of[tracks_[i].uuid] = i;
    // 1. Los que siguen en el mundo.
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        const ecs::Entity e = world.find(tracks_[i].uuid);
        if (e.valid()) bound_[i] = key(e.handle());
    }
    // 2. Los que ya no existen y tienen copia: se crean (bajo su padre si sigue).
    const ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        if (bound_[i] != kNone || tracks_[i].snapshot.empty()) continue;
        const ecs::Entity parent = world.find(tracks_[i].parent);
        ecs::Entity ghost = ecs::pasteEntities(world, tracks_[i].snapshot, parent);
        if (!ghost.valid()) continue;
        // Solo para verse: sin scripts, fisica ni IA.
        std::vector<ecs::Entity> stack{ghost};
        while (!stack.empty()) {
            const ecs::Entity g = stack.back();
            stack.pop_back();
            for (const char* name : kGhostStrip) {
                const ecs::ComponentType* type = registry.find(name);
                if (type != nullptr && type->has(world, g.handle())) type->remove(world, g.handle());
            }
            for (const entt::entity c : g.children()) stack.push_back(world.wrap(c));
        }
        bound_[i] = key(ghost.handle());
        ghosts_.push_back(key(ghost.handle()));
    }
    // 3. Hijos de una copia: por nombre dentro de ella.
    for (int pass = 0; pass < 4; ++pass) {
        bool any = false;
        for (std::size_t i = 0; i < tracks_.size(); ++i) {
            if (bound_[i] != kNone) continue;
            const auto parent_track = track_of.find(tracks_[i].parent);
            if (parent_track == track_of.end() || bound_[parent_track->second] == kNone) continue;
            const ecs::Entity parent = world.wrap(static_cast<entt::entity>(bound_[parent_track->second]));
            for (const entt::entity c : parent.children()) {
                const ecs::Entity child = world.wrap(c);
                if (child.name() == tracks_[i].name) {
                    bound_[i] = key(c);
                    any = true;
                    break;
                }
            }
        }
        if (!any) break;
    }
}

bool ReplaySystem::play(ecs::World& world, float from, float speed) {
    if (frames_.size() < 2) return false;
    if (recording_) stopRecording();
    if (playing_) stop(world);
    bindEntities(world);
    // Como esta todo ahora (se deja igual al parar).
    restore_.clear();
    for (std::size_t i = 0; i < bound_.size(); ++i) {
        if (bound_[i] == kNone) continue;
        if (std::find(ghosts_.begin(), ghosts_.end(), bound_[i]) != ghosts_.end()) continue;
        const ecs::Entity e = world.wrap(static_cast<entt::entity>(bound_[i]));
        if (!e.valid()) continue;
        Restore r;
        r.uuid = e.uuid();
        const ecs::Transform& t = e.get<ecs::Transform>();
        r.position = t.position;
        r.rotation = t.rotation;
        r.scale = t.scale;
        r.active = e.activeSelf();
        if (const ecs::Animator* a = e.tryGet<ecs::Animator>()) {
            r.has_animator = true;
            r.animator_playing = a->playing;
            r.animator_time = a->time;
            r.animator_state = a->runtime.state;
        }
        r.has_motion = e.has<anim::MotionMatching>();
        restore_.push_back(r);
    }
    playing_ = true;
    paused_ = false;
    speed_ = speed;
    decoded_index_ = -1;
    camera_angles_valid_ = false;
    const float length = duration();
    time_ = from < 0.0f ? std::max(0.0f, length + from) : std::min(from, length);
    applyAt(world, time_);
    return true;
}

void ReplaySystem::stop(ecs::World& world) {
    if (!playing_) return;
    playing_ = false;
    paused_ = false;
    for (const std::uint32_t g : ghosts_) {
        const ecs::Entity e = world.wrap(static_cast<entt::entity>(g));
        if (e.valid()) world.destroy(e);
    }
    ghosts_.clear();
    for (const Restore& r : restore_) {
        ecs::Entity e = world.find(r.uuid);
        if (!e.valid()) continue;
        e.setLocalTrs(r.position, r.rotation, r.scale);
        if (e.activeSelf() != r.active) e.setActive(r.active);
        if (r.has_animator) {
            if (ecs::Animator* a = e.tryGet<ecs::Animator>()) {
                a->playing = r.animator_playing;
                a->time = r.animator_time;
                a->runtime.state = r.animator_state;
                a->runtime.replay_control = false;
            }
        }
        if (r.has_motion) {
            if (anim::MotionMatching* mm = e.tryGet<anim::MotionMatching>()) {
                mm->runtime.replay_control = false;
                mm->runtime.sim_valid = false;  // la posicion salta: no es velocidad
            }
        }
    }
    restore_.clear();
    bound_.clear();
}

void ReplaySystem::seek(float seconds) {
    time_ = std::clamp(seconds, 0.0f, duration());
}

void ReplaySystem::fireEvents(float from, float to) {
    if (audio_ == nullptr || frames_.empty() || to <= from || to - from > 0.5f) return;
    const float base = frames_.front().time;
    for (const ReplayEvent& ev : events_) {
        const float t = ev.time - base;
        if (t <= from || t > to) continue;
        switch (ev.kind) {
            case ReplayEvent::Kind::OneShot: audio_->playOneShot(ev.name, ev.position, ev.volume, ev.spatial); break;
            default: break;  // las fuentes van en applyAt (necesitan el mundo)
        }
    }
}

void ReplaySystem::applyAt(ecs::World& world, float seconds) {
    if (frames_.empty()) return;
    const float absolute = frames_.front().time + seconds;
    // Ultimo fotograma con tiempo <= absolute.
    const auto upper = std::upper_bound(frames_.begin(), frames_.end(), absolute,
                                        [](float t, const Frame& f) { return t < f.time; });
    const int index = std::max(0, static_cast<int>(upper - frames_.begin()) - 1);
    decodeTo(index);
    const bool has_next = index + 1 < static_cast<int>(frames_.size());
    float alpha = 0.0f;
    if (has_next) {
        next_state_ = decoded_;
        decodeFrame(index + 1, next_state_);
        const float t0 = frames_[static_cast<std::size_t>(index)].time;
        const float t1 = frames_[static_cast<std::size_t>(index) + 1].time;
        alpha = t1 > t0 ? std::clamp((absolute - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
    }
    const Frame& frame = frames_[static_cast<std::size_t>(index)];
    const Vec3 origin_offset{static_cast<float>(frame.origin[0] - world.origin().x),
                             static_cast<float>(frame.origin[1] - world.origin().y),
                             static_cast<float>(frame.origin[2] - world.origin().z)};
    // La camara principal con camara libre no sigue lo grabado.
    std::uint32_t free_camera = kNone;
    if (free_camera_) {
        for (const entt::entity h : world.registry().view<ecs::Camera>()) {
            const ecs::Entity e = world.wrap(h);
            if (e.get<ecs::Camera>().is_main && e.activeInHierarchy()) {
                free_camera = key(h);
                break;
            }
        }
    }
    for (std::size_t i = 0; i < bound_.size() && i < decoded_.size(); ++i) {
        if (bound_[i] == kNone || bound_[i] == free_camera) continue;
        ecs::Entity e = world.wrap(static_cast<entt::entity>(bound_[i]));
        if (!e.valid()) continue;
        const TrackState& a = decoded_[i];
        if (!a.alive) {
            if (e.activeSelf()) e.setActive(false);
            continue;
        }
        const TrackState& b = has_next && i < next_state_.size() && next_state_[i].alive ? next_state_[i] : a;
        Vec3 position = core::lerp(a.position, b.position, alpha);
        if (!e.parent().valid()) position = position + origin_offset;
        e.setLocalTrs(position, nlerp(a.rotation, b.rotation, alpha), core::lerp(a.scale, b.scale, alpha));
        if (e.activeSelf() != a.active) e.setActive(a.active);
        if (a.anim_kind == 0) continue;
        float anim_time = a.anim_time;
        if (b.anim_kind == a.anim_kind && b.anim_state == a.anim_state && b.anim_time >= a.anim_time) {
            anim_time = a.anim_time + (b.anim_time - a.anim_time) * alpha;
        }
        if (a.anim_kind == 1) {
            if (ecs::Animator* animator = e.tryGet<ecs::Animator>()) {
                animator->playing = false;
                if (animator->controller.valid()) {
                    animator->runtime.replay_control = true;
                    animator->runtime.state = a.anim_state;
                    for (const auto& [index_param, value] : a.params) {
                        if (index_param >= 0 && index_param < static_cast<int>(param_names_.size())) {
                            animator->runtime.values[param_names_[static_cast<std::size_t>(index_param)]] = value;
                        }
                    }
                } else {
                    animator->clip = a.anim_state;
                    animator->clip_name.clear();
                }
                animator->time = anim_time;
            }
        } else if (a.anim_kind == 2) {
            if (anim::MotionMatching* mm = e.tryGet<anim::MotionMatching>()) {
                mm->runtime.replay_control = true;
                mm->runtime.entry = a.anim_state;
                mm->runtime.time = anim_time;
            }
        }
    }
}

void ReplaySystem::updateFreeCamera(ecs::World& world, float dt) {
    ecs::Entity camera;
    for (const entt::entity h : world.registry().view<ecs::Camera>()) {
        const ecs::Entity e = world.wrap(h);
        if (e.get<ecs::Camera>().is_main && e.activeInHierarchy()) {
            camera = e;
            break;
        }
    }
    if (!camera.valid()) return;
    if (!camera_angles_valid_) {
        const Vec3 f = camera.forward();
        camera_yaw_ = std::atan2(-f.x, -f.z) * 180.0f / core::kPi;
        camera_pitch_ = std::asin(std::clamp(f.y, -1.0f, 1.0f)) * 180.0f / core::kPi;
        camera_angles_valid_ = true;
    }
    Vec3 position = camera.worldPosition();
    if (input_ != nullptr) {
        if (input_->isMouseButtonDown(dm::MouseButton::Right)) {
            camera_yaw_ -= input_->mouseDeltaX() * 0.15f;
            camera_pitch_ = std::clamp(camera_pitch_ - input_->mouseDeltaY() * 0.15f, -89.0f, 89.0f);
        }
        const float speed = (input_->isKeyDown(dm::Key::LeftShift) ? 20.0f : 6.0f) * dt;
        const Quat rotation = ecs::quatFromEulerDegrees(Vec3{camera_pitch_, camera_yaw_, 0.0f});
        const Vec3 forward = ecs::quatRotate(rotation, Vec3{0.0f, 0.0f, -1.0f});
        const Vec3 right = ecs::quatRotate(rotation, Vec3{1.0f, 0.0f, 0.0f});
        Vec3 move{};
        if (input_->isKeyDown(dm::Key::W)) move = move + forward;
        if (input_->isKeyDown(dm::Key::S)) move = move - forward;
        if (input_->isKeyDown(dm::Key::D)) move = move + right;
        if (input_->isKeyDown(dm::Key::A)) move = move - right;
        if (input_->isKeyDown(dm::Key::E)) move = move + Vec3{0.0f, 1.0f, 0.0f};
        if (input_->isKeyDown(dm::Key::Q)) move = move - Vec3{0.0f, 1.0f, 0.0f};
        position = position + move * speed;
    }
    const Quat rotation = ecs::quatFromEulerDegrees(Vec3{camera_pitch_, camera_yaw_, 0.0f});
    camera.setWorldMatrix(core::composeTrs(position, rotation, Vec3{1.0f, 1.0f, 1.0f}));
}

void ReplaySystem::update(ecs::World& world, float delta_seconds) {
    if (recording_) {
        record_time_ += delta_seconds;
        sample_accumulator_ += delta_seconds;
        const float interval = 1.0f / options_.rate;
        if (sample_accumulator_ >= interval) {
            // Un frame muy largo no graba muchas muestras iguales.
            sample_accumulator_ = std::min(sample_accumulator_ - interval, interval);
            recordSample(world);
        }
        return;
    }
    if (!playing_) return;
    const float length = duration();
    if (!paused_) {
        const float previous = time_;
        time_ += delta_seconds * speed_;
        if (time_ > length) {
            time_ = loop_ && length > 0.0f ? std::fmod(time_, length) : length;
            if (!loop_) paused_ = true;  // se queda en el ultimo fotograma
        }
        if (time_ < 0.0f) time_ = loop_ && length > 0.0f ? length + std::fmod(time_, length) : 0.0f;
        if (time_ > previous) {
            fireEvents(previous, time_);
            // Fuentes de audio que empiezan o paran en este tramo.
            if (audio_ != nullptr && time_ - previous < 0.5f) {
                const float base = frames_.front().time;
                for (const ReplayEvent& ev : events_) {
                    const float t = ev.time - base;
                    if (t <= previous || t > time_) continue;
                    if (ev.kind != ReplayEvent::Kind::SourcePlay && ev.kind != ReplayEvent::Kind::SourceStop) continue;
                    const ecs::Entity e = world.find(ev.entity);
                    if (!e.valid()) continue;
                    if (ev.kind == ReplayEvent::Kind::SourcePlay) {
                        audio_->play(e);
                    } else {
                        audio_->stop(e);
                    }
                }
            }
        }
    }
    applyAt(world, time_);
    if (free_camera_) updateFreeCamera(world, delta_seconds);
}

// -----------------------------------------------------------------------------
// Archivos
// -----------------------------------------------------------------------------

std::filesystem::path ReplaySystem::fileFor(const std::string& name) const {
    std::filesystem::path file(std::u8string(name.begin(), name.end()));
    if (file.extension() != ".crreplay") file += ".crreplay";
    return file.is_absolute() ? file : folder_ / file;
}

std::vector<std::string> ReplaySystem::list() const {
    std::vector<std::string> names;
    std::error_code ec;
    if (folder_.empty() || !std::filesystem::is_directory(folder_, ec)) return names;
    for (const auto& entry : std::filesystem::directory_iterator(folder_, ec)) {
        if (entry.path().extension() != ".crreplay") continue;
        const std::u8string stem = entry.path().stem().u8string();
        names.emplace_back(stem.begin(), stem.end());
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool ReplaySystem::save(const std::filesystem::path& file, std::string* error) const {
    if (frames_.empty()) {
        if (error) *error = "no hay nada grabado";
        return false;
    }
    json header;
    header["scene"] = scene_name_;
    header["rate"] = options_.rate;
    header["keyframe_interval"] = options_.keyframe_interval;
    header["raw_bytes"] = raw_bytes_;
    json& tracks = header["tracks"] = json::array();
    for (const Track& t : tracks_) {
        tracks.push_back({{"uuid", t.uuid.toString()},
                          {"parent", t.parent.valid() ? t.parent.toString() : std::string{}},
                          {"name", t.name},
                          {"spawned", t.spawned},
                          {"snapshot", t.snapshot}});
    }
    header["params"] = param_names_;
    json& events = header["events"] = json::array();
    for (const ReplayEvent& e : events_) {
        events.push_back({{"kind", static_cast<int>(e.kind)},
                          {"time", e.time},
                          {"name", e.name},
                          {"data", e.data},
                          {"entity", e.entity.valid() ? e.entity.toString() : std::string{}},
                          {"position", json::array({e.position.x, e.position.y, e.position.z})},
                          {"volume", e.volume},
                          {"spatial", e.spatial}});
    }
    const std::string text = header.dump();
    std::vector<std::uint8_t> out;
    out.insert(out.end(), kMagic, kMagic + 8);
    const auto putU32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    };
    const auto putF64 = [&](double v) {
        std::uint8_t b[8];
        std::memcpy(b, &v, 8);
        out.insert(out.end(), b, b + 8);
    };
    putU32(kVersion);
    putU32(static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
    putU32(static_cast<std::uint32_t>(frames_.size()));
    for (const Frame& f : frames_) {
        putF32(out, f.time);
        putF64(f.origin[0]);
        putF64(f.origin[1]);
        putF64(f.origin[2]);
        putU32(f.offset);
        out.push_back(f.keyframe ? 1 : 0);
    }
    putU32(static_cast<std::uint32_t>(stream_.size()));
    out.insert(out.end(), stream_.begin(), stream_.end());

    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error) *error = "no se pudo escribir " + file.string();
        return false;
    }
    stream.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!stream) {
        if (error) *error = "error escribiendo " + file.string();
        return false;
    }
    return true;
}

bool ReplaySystem::load(const std::filesystem::path& file, std::string* error) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        if (error) *error = "no se pudo abrir " + file.string();
        return false;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (bytes.size() < 16 || std::memcmp(bytes.data(), kMagic, 8) != 0) {
        if (error) *error = "no es una repeticion (.crreplay)";
        return false;
    }
    std::size_t at = 8;
    bool ok = true;
    const auto u32 = [&]() -> std::uint32_t {
        if (at + 4 > bytes.size()) {
            ok = false;
            return 0;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(bytes[at + static_cast<std::size_t>(i)]) << (8 * i);
        at += 4;
        return v;
    };
    const auto f32 = [&]() -> float {
        float v = 0.0f;
        if (at + 4 > bytes.size()) {
            ok = false;
            return 0.0f;
        }
        std::memcpy(&v, bytes.data() + at, 4);
        at += 4;
        return v;
    };
    const auto f64 = [&]() -> double {
        double v = 0.0;
        if (at + 8 > bytes.size()) {
            ok = false;
            return 0.0;
        }
        std::memcpy(&v, bytes.data() + at, 8);
        at += 8;
        return v;
    };
    const std::uint32_t version = u32();
    if (version != kVersion) {
        if (error) *error = "version de repeticion desconocida";
        return false;
    }
    const std::uint32_t header_size = u32();
    if (!ok || at + header_size > bytes.size()) {
        if (error) *error = "repeticion cortada";
        return false;
    }
    const json header =
        json::parse(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                    bytes.begin() + static_cast<std::ptrdiff_t>(at + header_size), nullptr, false);
    at += header_size;
    if (header.is_discarded() || !header.is_object()) {
        if (error) *error = "cabecera de la repeticion danada";
        return false;
    }
    if (playing_ || recording_) {
        if (error) *error = "para la repeticion o la grabacion antes de cargar otra";
        return false;
    }
    clear();
    scene_name_ = header.value("scene", std::string{});
    options_ = ReplayOptions{};
    options_.rate = header.value("rate", 30.0f);
    options_.keyframe_interval = std::max(2, header.value("keyframe_interval", 30));
    raw_bytes_ = header.value("raw_bytes", static_cast<std::size_t>(0));
    if (const auto it = header.find("tracks"); it != header.end() && it->is_array()) {
        for (const json& j : *it) {
            Track t;
            t.uuid = Uuid::parse(j.value("uuid", std::string{}));
            t.parent = Uuid::parse(j.value("parent", std::string{}));
            t.name = j.value("name", std::string{});
            t.spawned = j.value("spawned", false);
            t.snapshot = j.value("snapshot", std::string{});
            tracks_.push_back(std::move(t));
        }
    }
    if (const auto it = header.find("params"); it != header.end() && it->is_array()) {
        for (const json& j : *it) param_names_.push_back(j.is_string() ? j.get<std::string>() : std::string{});
    }
    if (const auto it = header.find("events"); it != header.end() && it->is_array()) {
        for (const json& j : *it) {
            ReplayEvent e;
            e.kind = static_cast<ReplayEvent::Kind>(std::clamp(j.value("kind", 0), 0, 3));
            e.time = j.value("time", 0.0f);
            e.name = j.value("name", std::string{});
            e.data = j.value("data", std::string{});
            e.entity = Uuid::parse(j.value("entity", std::string{}));
            if (const auto p = j.find("position"); p != j.end() && p->is_array() && p->size() == 3) {
                e.position = Vec3{(*p)[0].get<float>(), (*p)[1].get<float>(), (*p)[2].get<float>()};
            }
            e.volume = j.value("volume", 1.0f);
            e.spatial = j.value("spatial", true);
            events_.push_back(std::move(e));
        }
    }
    const std::uint32_t frame_count = u32();
    for (std::uint32_t i = 0; i < frame_count && ok; ++i) {
        Frame f;
        f.time = f32();
        f.origin[0] = f64();
        f.origin[1] = f64();
        f.origin[2] = f64();
        f.offset = u32();
        f.keyframe = at < bytes.size() && bytes[at] != 0;
        ++at;
        frames_.push_back(f);
    }
    const std::uint32_t stream_size = u32();
    if (!ok || at + stream_size > bytes.size()) {
        clear();
        if (error) *error = "repeticion cortada";
        return false;
    }
    stream_.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                   bytes.begin() + static_cast<std::ptrdiff_t>(at + stream_size));
    if (!frames_.empty()) frames_.front().keyframe = true;
    return true;
}

}  // namespace cramion::replay
