#pragma once

// Mandos en Linux por evdev (/dev/input/event*), para WindowX11.cpp. Solo
// los que el kernel da con el esquema de mando (BTN_GAMEPAD: xpad, xone,
// hid-playstation, hid-nintendo, hid-generic con mapa de mando...): ni
// joysticks de vuelo ni el panel tactil o los sensores del DualSense, que van
// en nodos aparte. Todo sin bloquear: O_NONBLOCK, inotify para los nuevos y,
// si falla, una vuelta por la carpeta cada ~1 s. La vibracion va por force
// feedback (FF_RUMBLE). Sin permisos de lectura en /dev/input no hay mandos
// (udev se los da al usuario de la sesion; si no, el grupo "input").

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "GamepadCommon.h"

namespace cramion::dm {

class EvdevGamepads {
public:
    EvdevGamepads() {
        notify_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        // IN_ATTRIB: udev cambia los permisos del nodo despues de crearlo.
        if (notify_ >= 0 && inotify_add_watch(notify_, "/dev/input", IN_CREATE | IN_ATTRIB | IN_DELETE) < 0) {
            ::close(notify_);
            notify_ = -1;
        }
    }
    ~EvdevGamepads() {
        for (const auto& d : devices_) {
            stopEffect(*d);
            ::close(d->fd);
        }
        if (notify_ >= 0) ::close(notify_);
    }
    EvdevGamepads(const EvdevGamepads&) = delete;
    EvdevGamepads& operator=(const EvdevGamepads&) = delete;

    // Lee todo lo pendiente y emite los cambios. active = la ventana tiene el
    // foco (o se leen en segundo plano); sin el, se suelta todo una vez.
    template <typename Emit>
    void poll(bool active, Emit&& emit) {
        const double now = gamepad::nowSeconds();
        for (int i = 0; i < gamepad::kSlots; ++i) {
            gamepad::Rumble& r = rumble_[static_cast<size_t>(i)];
            if (r.until > 0.0 && now >= r.until) {
                r = {};
                applyRumble(i, active);
            }
        }
        if (!active) {
            if (!released_) {
                slots_.releaseAll(emit);
                for (const auto& d : devices_) d->resync = true;
                released_ = true;
            }
            return;
        }
        released_ = false;
        const bool changed = watchChanged();
        if (changed) rejected_.clear();  // un nodo nuevo puede reusar el numero de otro
        if (changed || now >= next_scan_) {
            scan(emit);
            // Con inotify la vuelta periodica solo cubre lo que se escape.
            next_scan_ = now + (notify_ >= 0 ? 5.0 : gamepad::kProbeSeconds);
        }
        for (size_t i = 0; i < devices_.size();) {
            if (read(*devices_[i], emit)) {
                ++i;
                continue;
            }
            // ENODEV: desenchufado.
            Device& d = *devices_[i];
            rumble_[static_cast<size_t>(d.slot)] = {};
            slots_.disconnect(d.slot, emit);
            ::close(d.fd);
            devices_.erase(devices_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    bool vibrate(int pad, float low, float high, float seconds, bool active) {
        const double until = seconds > 0.0f ? gamepad::nowSeconds() + seconds : 0.0;
        bool any = false;
        for (int i = 0; i < gamepad::kSlots; ++i) {
            if ((pad >= 0 && pad != i) || !slots_.used(i)) continue;
            rumble_[static_cast<size_t>(i)] = {low, high, until};
            any = applyRumble(i, active) || any;
        }
        return any;
    }

    // Al cambiar el foco: sin el, nada vibra; al volver, sigue lo pedido.
    void refreshRumble(bool active) {
        for (int i = 0; i < gamepad::kSlots; ++i) {
            const gamepad::Rumble& r = rumble_[static_cast<size_t>(i)];
            if (r.low > 0.0f || r.high > 0.0f) applyRumble(i, active);
        }
    }

    void stopAll() {
        for (const auto& d : devices_) stopEffect(*d);
        rumble_ = {};
    }

private:
    // Lo que da cada eje del kernel en nuestro estado.
    enum class Target : uint8_t { None, LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, HatX, HatY };

    struct Device {
        int fd = -1;
        dev_t node = 0;  // st_rdev: el mismo mando no se abre dos veces
        int slot = -1;
        bool positional = false;  // Sony/Nintendo: BTN_NORTH es el de arriba
        bool digital_triggers = false;
        bool resync = true;       // releer el estado entero (al abrir, SYN_DROPPED, foco)
        bool dropped = false;     // tras SYN_DROPPED, hasta el siguiente SYN_REPORT
        std::array<Target, ABS_CNT> targets{};
        std::array<input_absinfo, ABS_CNT> info{};
        int hat_x = 0;
        int hat_y = 0;
        gamepad::State state;
        bool can_rumble = false;
        int effect = -1;  // id del efecto subido
        float low = 0.0f, high = 0.0f;
    };

    static bool testBit(const unsigned long* bits, unsigned bit) {
        constexpr unsigned kBits = sizeof(unsigned long) * 8;
        return (bits[bit / kBits] >> (bit % kBits)) & 1ul;
    }

    // ¿Hubo altas o bajas en /dev/input? Vacia la cola de inotify.
    bool watchChanged() {
        if (notify_ < 0) return false;
        alignas(inotify_event) char buffer[4096];
        bool changed = false;
        while (::read(notify_, buffer, sizeof(buffer)) > 0) changed = true;
        return changed;
    }

    template <typename Emit>
    void scan(Emit&& emit) {
        DIR* dir = opendir("/dev/input");
        if (dir == nullptr) return;
        while (dirent* entry = readdir(dir)) {
            if (std::strncmp(entry->d_name, "event", 5) != 0) continue;
            const std::string path = std::string("/dev/input/") + entry->d_name;
            struct stat st {};
            if (::stat(path.c_str(), &st) != 0) continue;
            bool known = false;
            for (const auto& d : devices_) known = known || d->node == st.st_rdev;
            if (known || std::find(rejected_.begin(), rejected_.end(), st.st_rdev) != rejected_.end()) continue;
            open(path, st.st_rdev, emit);
        }
        closedir(dir);
    }

    template <typename Emit>
    void open(const std::string& path, dev_t node, Emit&& emit) {
        if (slots_.used(0) && slots_.used(1) && slots_.used(2) && slots_.used(3)) return;
        // Escritura para la vibracion; si no hay permiso, solo lectura.
        int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        bool writable = fd >= 0;
        if (fd < 0) fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return;  // sin permiso: quiza llegue con IN_ATTRIB
        constexpr size_t kLongs = (KEY_CNT + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8);
        unsigned long keys[kLongs] = {};
        unsigned long abs[(ABS_CNT + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)] = {};
        unsigned long ff[(FF_CNT + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8)] = {};
        const bool ok = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 &&
                        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) >= 0;
        if (!ok || !testBit(keys, BTN_GAMEPAD) || !testBit(abs, ABS_X) || !testBit(abs, ABS_Y)) {
            ::close(fd);
            rejected_.push_back(node);  // no es un mando: no se vuelve a abrir
            return;
        }
        auto d = std::make_unique<Device>();
        d->fd = fd;
        d->node = node;
        input_id id{};
        ioctl(fd, EVIOCGID, &id);
        GamepadKind kind = GamepadKind::Generic;
        if (id.vendor == 0x045E) kind = GamepadKind::Xbox;
        else if (id.vendor == 0x054C) kind = GamepadKind::PlayStation;
        else if (id.vendor == 0x057E) kind = GamepadKind::Nintendo;
        // xpad (y lo que imita a un mando de Xbox) da BTN_X/BTN_Y por la
        // etiqueta; hid-playstation y hid-nintendo, por la posicion.
        d->positional = kind == GamepadKind::PlayStation || kind == GamepadKind::Nintendo;
        // Ejes: sticks en ABS_X/Y y ABS_RX/RY con los gatillos en ABS_Z/RZ;
        // los HID genericos sin RX/RY llevan el stick derecho en Z/RZ y los
        // gatillos en BRAKE/GAS.
        const bool has_rx = testBit(abs, ABS_RX) && testBit(abs, ABS_RY);
        d->targets[ABS_X] = Target::LeftX;
        d->targets[ABS_Y] = Target::LeftY;
        if (has_rx) {
            d->targets[ABS_RX] = Target::RightX;
            d->targets[ABS_RY] = Target::RightY;
            d->targets[ABS_Z] = Target::LeftTrigger;
            d->targets[ABS_RZ] = Target::RightTrigger;
        } else {
            d->targets[ABS_Z] = Target::RightX;
            d->targets[ABS_RZ] = Target::RightY;
        }
        d->targets[ABS_BRAKE] = Target::LeftTrigger;
        d->targets[ABS_GAS] = Target::RightTrigger;
        d->targets[ABS_HAT0X] = Target::HatX;
        d->targets[ABS_HAT0Y] = Target::HatY;
        bool analog_triggers = false;
        for (unsigned a = 0; a < ABS_CNT; ++a) {
            if (!testBit(abs, a)) {
                d->targets[a] = Target::None;
                continue;
            }
            if (d->targets[a] == Target::None) continue;
            if (ioctl(fd, EVIOCGABS(a), &d->info[a]) < 0) d->targets[a] = Target::None;
            analog_triggers = analog_triggers || d->targets[a] == Target::LeftTrigger;
        }
        d->digital_triggers = !analog_triggers;
        if (writable && ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff) >= 0) d->can_rumble = testBit(ff, FF_RUMBLE);
        d->slot = slots_.connect(kind, emit);
        if (d->slot < 0) {
            ::close(fd);
            return;
        }
        devices_.push_back(std::move(d));
    }

    // Un eje del kernel normalizado: sticks a [-1, 1] (Y hacia arriba),
    // gatillos a [0, 1].
    static float normalize(const input_absinfo& info, int value, bool centered) {
        const float range = static_cast<float>(info.maximum - info.minimum);
        if (range <= 0.0f) return 0.0f;
        const float t = std::clamp((static_cast<float>(value) - static_cast<float>(info.minimum)) / range, 0.0f, 1.0f);
        return centered ? t * 2.0f - 1.0f : t;
    }

    void setAbs(Device& d, unsigned code, int value) {
        if (code >= ABS_CNT) return;
        const input_absinfo& info = d.info[code];
        switch (d.targets[code]) {
            case Target::LeftX: d.state.set(GamepadAxis::LeftX, normalize(info, value, true)); break;
            case Target::LeftY: d.state.set(GamepadAxis::LeftY, -normalize(info, value, true)); break;
            case Target::RightX: d.state.set(GamepadAxis::RightX, normalize(info, value, true)); break;
            case Target::RightY: d.state.set(GamepadAxis::RightY, -normalize(info, value, true)); break;
            case Target::LeftTrigger: d.state.set(GamepadAxis::LeftTrigger, normalize(info, value, false)); break;
            case Target::RightTrigger: d.state.set(GamepadAxis::RightTrigger, normalize(info, value, false)); break;
            case Target::HatX:
                d.hat_x = value < 0 ? -1 : (value > 0 ? 1 : 0);
                d.state.setHat(d.hat_x, d.hat_y);
                break;
            case Target::HatY:
                d.hat_y = value < 0 ? -1 : (value > 0 ? 1 : 0);
                d.state.setHat(d.hat_x, d.hat_y);
                break;
            default: break;
        }
    }

    static void setKey(Device& d, unsigned code, bool down) {
        gamepad::State& s = d.state;
        switch (code) {
            case BTN_SOUTH: s.set(GamepadButton::A, down); break;
            case BTN_EAST: s.set(GamepadButton::B, down); break;
            case BTN_NORTH: s.set(d.positional ? GamepadButton::Y : GamepadButton::X, down); break;
            case BTN_WEST: s.set(d.positional ? GamepadButton::X : GamepadButton::Y, down); break;
            case BTN_TL: s.set(GamepadButton::LeftShoulder, down); break;
            case BTN_TR: s.set(GamepadButton::RightShoulder, down); break;
            case BTN_TL2:
                if (d.digital_triggers) s.set(GamepadAxis::LeftTrigger, down ? 1.0f : 0.0f);
                break;
            case BTN_TR2:
                if (d.digital_triggers) s.set(GamepadAxis::RightTrigger, down ? 1.0f : 0.0f);
                break;
            case BTN_SELECT: s.set(GamepadButton::Back, down); break;
            case BTN_START: s.set(GamepadButton::Start, down); break;
            case BTN_THUMBL: s.set(GamepadButton::LeftStick, down); break;
            case BTN_THUMBR: s.set(GamepadButton::RightStick, down); break;
            case BTN_DPAD_UP: s.set(GamepadButton::DpadUp, down); break;
            case BTN_DPAD_DOWN: s.set(GamepadButton::DpadDown, down); break;
            case BTN_DPAD_LEFT: s.set(GamepadButton::DpadLeft, down); break;
            case BTN_DPAD_RIGHT: s.set(GamepadButton::DpadRight, down); break;
            default: break;
        }
    }

    // El estado entero por ioctl (al abrir, tras perder eventos o el foco).
    void resync(Device& d) {
        constexpr size_t kLongs = (KEY_CNT + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8);
        unsigned long keys[kLongs] = {};
        d.state = {};
        d.hat_x = d.hat_y = 0;
        if (ioctl(d.fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
            for (unsigned code = BTN_SOUTH; code <= BTN_THUMBR; ++code) setKey(d, code, testBit(keys, code));
            for (unsigned code = BTN_DPAD_UP; code <= BTN_DPAD_RIGHT; ++code) setKey(d, code, testBit(keys, code));
        }
        for (unsigned a = 0; a < ABS_CNT; ++a) {
            if (d.targets[a] == Target::None) continue;
            input_absinfo info{};
            if (ioctl(d.fd, EVIOCGABS(a), &info) >= 0) setAbs(d, a, info.value);
        }
        d.resync = false;
    }

    // Vacia la cola del mando y emite al cerrar cada paquete (SYN_REPORT).
    // false si el mando ya no esta.
    template <typename Emit>
    bool read(Device& d, Emit&& emit) {
        if (d.resync) {
            resync(d);
            slots_.update(d.slot, d.state, emit);
        }
        input_event events[64];
        for (;;) {
            const ssize_t n = ::read(d.fd, events, sizeof(events));
            if (n < 0) return errno == EAGAIN || errno == EINTR;
            if (n == 0) return true;
            const size_t count = static_cast<size_t>(n) / sizeof(input_event);
            for (size_t i = 0; i < count; ++i) {
                const input_event& ev = events[i];
                if (ev.type == EV_SYN) {
                    if (ev.code == SYN_DROPPED) {
                        d.dropped = true;  // se perdieron eventos: el estado no vale
                    } else if (ev.code == SYN_REPORT) {
                        if (d.dropped) {
                            d.dropped = false;
                            resync(d);
                        }
                        slots_.update(d.slot, d.state, emit);
                    }
                    continue;
                }
                if (d.dropped) continue;
                if (ev.type == EV_KEY) setKey(d, ev.code, ev.value != 0);
                else if (ev.type == EV_ABS) setAbs(d, ev.code, ev.value);
            }
            if (count < sizeof(events) / sizeof(events[0])) return true;
        }
    }

    bool applyRumble(int slot, bool active) {
        const gamepad::Rumble& r = rumble_[static_cast<size_t>(slot)];
        for (const auto& d : devices_) {
            if (d->slot != slot) continue;
            if (!d->can_rumble) return false;
            const float low = active ? r.low : 0.0f;
            const float high = active ? r.high : 0.0f;
            if (low <= 0.0f && high <= 0.0f) {
                stopEffect(*d);
                return true;
            }
            // Se reescribe el mismo efecto (el id lo da el kernel la primera
            // vez); length 0 = hasta que se pare (la duracion la lleva poll).
            ff_effect effect{};
            effect.type = FF_RUMBLE;
            effect.id = static_cast<int16_t>(d->effect);
            effect.u.rumble.strong_magnitude = gamepad::motor16(low);
            effect.u.rumble.weak_magnitude = gamepad::motor16(high);
            effect.replay.length = 0;
            if (ioctl(d->fd, EVIOCSFF, &effect) < 0) {
                d->can_rumble = false;
                return false;
            }
            d->effect = effect.id;
            input_event play{};
            play.type = EV_FF;
            play.code = static_cast<uint16_t>(d->effect);
            play.value = 1;
            const bool ok = ::write(d->fd, &play, sizeof(play)) == static_cast<ssize_t>(sizeof(play));
            d->low = low;
            d->high = high;
            return ok;
        }
        return false;
    }

    static void stopEffect(Device& d) {
        if (d.effect < 0 || (d.low <= 0.0f && d.high <= 0.0f)) return;
        input_event stop{};
        stop.type = EV_FF;
        stop.code = static_cast<uint16_t>(d.effect);
        stop.value = 0;
        [[maybe_unused]] const ssize_t written = ::write(d.fd, &stop, sizeof(stop));
        d.low = d.high = 0.0f;
    }

    int notify_ = -1;
    double next_scan_ = 0.0;
    bool released_ = false;
    std::vector<std::unique_ptr<Device>> devices_;
    std::vector<dev_t> rejected_;  // nodos que no son mandos (teclados, raton...)
    gamepad::Slots slots_;
    std::array<gamepad::Rumble, gamepad::kSlots> rumble_{};
};

}  // namespace cramion::dm
