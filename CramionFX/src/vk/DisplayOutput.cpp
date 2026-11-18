#include "CramionFX/vk/DisplayOutput.h"
#include "CramionFX/vk/DisplayOutputVk.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cramion::gfx {

const char* outputEncodingName(OutputEncoding encoding) {
    switch (encoding) {
        case OutputEncoding::Hdr10Pq: return "HDR10 (PQ, BT.2020)";
        case OutputEncoding::ScRgbLinear: return "scRGB (lineal)";
        default: return "SDR";
    }
}

namespace {

vk::SurfaceFormatKHR chooseSdr(const std::vector<vk::SurfaceFormatKHR>& available) {
    // Preferencia: BGRA8 (el habitual en Windows), luego RGBA8 (Android) y
    // luego el de 10 bits.
    const std::array<vk::Format, 4> preferred = {vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Unorm,
                                                 vk::Format::eA2B10G10R10UnormPack32,
                                                 vk::Format::eA2R10G10B10UnormPack32};
    for (const vk::Format wanted : preferred) {
        for (const auto& format : available) {
            if (format.format == wanted && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) return format;
        }
    }
    // Como ultimo recurso cualquiera que no sea _SRGB.
    for (const auto& format : available) {
        const bool srgb = format.format == vk::Format::eB8G8R8A8Srgb || format.format == vk::Format::eR8G8B8A8Srgb ||
                          format.format == vk::Format::eA8B8G8R8SrgbPack32;
        if (!srgb && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) return format;
    }
    return available.front();
}

bool findHdr10(const std::vector<vk::SurfaceFormatKHR>& available, vk::SurfaceFormatKHR& out) {
    for (const vk::Format wanted : {vk::Format::eA2B10G10R10UnormPack32, vk::Format::eA2R10G10B10UnormPack32}) {
        for (const auto& format : available) {
            if (format.format == wanted && format.colorSpace == vk::ColorSpaceKHR::eHdr10St2084EXT) {
                out = format;
                return true;
            }
        }
    }
    return false;
}

bool findScRgb(const std::vector<vk::SurfaceFormatKHR>& available, vk::SurfaceFormatKHR& out) {
    for (const auto& format : available) {
        if (format.format == vk::Format::eR16G16B16A16Sfloat &&
            format.colorSpace == vk::ColorSpaceKHR::eExtendedSrgbLinearEXT) {
            out = format;
            return true;
        }
    }
    return false;
}

}  // namespace

SurfaceFormatChoice chooseOutputFormat(const std::vector<vk::SurfaceFormatKHR>& available, HdrOutput wanted,
                                       bool colorspace_extension) {
    if (available.empty()) {
        throw std::runtime_error("La superficie no expone ningun formato de color.");
    }
    SurfaceFormatChoice choice{};
    choice.surface = chooseSdr(available);
    choice.ui_format = choice.surface.format;
    if (wanted == HdrOutput::Off) return choice;
    // La UI y el FXAA se dibujan en una imagen SDR aparte con el formato SDR
    // de siempre: los pipelines de las herramientas (ImGui) se crearon con el
    // y no cambian al encender o apagar el HDR.
    vk::SurfaceFormatKHR hdr{};
    bool found = false;
    OutputEncoding encoding = OutputEncoding::Sdr;
    if (colorspace_extension) {
        if ((wanted == HdrOutput::Auto || wanted == HdrOutput::Hdr10) && findHdr10(available, hdr)) {
            found = true;
            encoding = OutputEncoding::Hdr10Pq;
        } else if ((wanted == HdrOutput::Auto || wanted == HdrOutput::ScRgb) && findScRgb(available, hdr)) {
            found = true;
            encoding = OutputEncoding::ScRgbLinear;
        }
    }
    if (!found) {
        // Vuelta a SDR: todo como si no se hubiera pedido.
        choice.hdr_fell_back = true;
        return choice;
    }
    choice.surface = hdr;
    choice.encoding = encoding;
    return choice;
}

float pqEncode(float nits) {
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 4096.0f * 128.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 4096.0f * 32.0f;
    constexpr float c3 = 2392.0f / 4096.0f * 32.0f;
    const float y = std::clamp(nits / 10000.0f, 0.0f, 1.0f);
    const float p = std::pow(y, m1);
    return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

float pqDecode(float value) {
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 4096.0f * 128.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 4096.0f * 32.0f;
    constexpr float c3 = 2392.0f / 4096.0f * 32.0f;
    const float e = std::pow(std::clamp(value, 0.0f, 1.0f), 1.0f / m2);
    const float y = std::pow(std::max(e - c1, 0.0f) / (c2 - c3 * e), 1.0f / m1);
    return y * 10000.0f;
}

float clampPaperWhite(float nits) {
    if (!std::isfinite(nits)) return 200.0f;
    return std::clamp(nits, 80.0f, 500.0f);
}

float clampMaxNits(float nits, float paper_white) {
    if (!std::isfinite(nits)) nits = 1000.0f;
    return std::clamp(nits, clampPaperWhite(paper_white), 10000.0f);
}

const char* latencyBackendName(LatencyBackend backend) {
    switch (backend) {
        case LatencyBackend::PresentWait: return "present_wait";
        case LatencyBackend::Reflex: return "Reflex";
        case LatencyBackend::AntiLag: return "Anti-Lag 2";
        default: return "ninguno";
    }
}

const char* lowLatencyModeName(LowLatencyMode mode) {
    switch (mode) {
        case LowLatencyMode::On: return "activado";
        case LowLatencyMode::Boost: return "activado+boost";
        default: return "apagado";
    }
}

LatencyBackend chooseLatencyBackend(LowLatencyMode mode, const OutputExtensions& ext) {
    if (mode == LowLatencyMode::Off) return LatencyBackend::None;
    if (ext.nv_low_latency2 && ext.present_id) return LatencyBackend::Reflex;
    if (ext.amd_anti_lag) return LatencyBackend::AntiLag;
    if (ext.present_id && ext.present_wait) return LatencyBackend::PresentWait;
    return LatencyBackend::None;
}

std::uint64_t presentWaitTarget(LowLatencyMode mode, std::uint64_t next_id) {
    const std::uint64_t behind = mode == LowLatencyMode::Boost ? 1 : mode == LowLatencyMode::On ? 2 : 0;
    if (behind == 0 || next_id <= behind) return 0;
    return next_id - behind;
}

void LatencyMeter::frameStarted(std::uint64_t id, Clock::time_point when) {
    if (id == 0) return;
    starts_[id % starts_.size()] = Start{id, when};
}

bool LatencyMeter::frameFinished(std::uint64_t id, Clock::time_point when) {
    if (id == 0) return false;
    Start& s = starts_[id % starts_.size()];
    if (s.id != id) return false;
    s.id = 0;
    const float ms = std::chrono::duration<float, std::milli>(when - s.when).count();
    if (!(ms >= 0.0f) || ms > 2000.0f) return false;
    addSample(ms);
    return true;
}

void LatencyMeter::addSample(float ms) {
    if (!std::isfinite(ms) || ms < 0.0f) return;
    window_[next_] = ms;
    next_ = (next_ + 1) % kWindow;
    count_ = std::min(count_ + 1, kWindow);
    last_ms_ = ms;
}

float LatencyMeter::averageMs() const {
    if (count_ == 0) return 0.0f;
    float sum = 0.0f;
    for (std::size_t i = 0; i < count_; ++i) sum += window_[i];
    return sum / static_cast<float>(count_);
}

void LatencyMeter::reset() {
    starts_ = {};
    window_ = {};
    next_ = 0;
    count_ = 0;
    last_ms_ = 0.0f;
}

}  // namespace cramion::gfx
