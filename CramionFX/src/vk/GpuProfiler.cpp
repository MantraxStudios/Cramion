#include "CramionFX/vk/GpuProfiler.h"

#include "CramionFX/vk/VulkanDevice.h"

#include <algorithm>

namespace cramion::gfx {

namespace {

// Peso de cada frame nuevo en la media: ~1 s a 60-200 FPS.
constexpr float kSmoothing = 0.03f;

}  // namespace

void GpuProfiler::create(const VulkanDevice& device, std::uint32_t frames_in_flight) {
    destroy();

    const vk::PhysicalDeviceProperties properties = device.physicalDevice().getProperties();
    const auto families = device.physicalDevice().getQueueFamilyProperties();
    const std::uint32_t family = device.queueFamilies().graphics;
    const std::uint32_t valid_bits =
        family < families.size() ? families[family].timestampValidBits : 0u;
    supported_ = valid_bits > 0 && properties.limits.timestampPeriod > 0.0f;
    if (!supported_) {
        return;
    }
    period_ns_ = properties.limits.timestampPeriod;
    valid_mask_ = valid_bits >= 64 ? ~0ull : ((1ull << valid_bits) - 1ull);

    vk::QueryPoolCreateInfo pool_info{};
    pool_info.queryType = vk::QueryType::eTimestamp;
    pool_info.queryCount = kMaxMarks * frames_in_flight;
    pool_ = vk::raii::QueryPool(device.handle(), pool_info);

    frames_.assign(frames_in_flight, FrameMarks{});
    timings_.clear();
    total_ms_ = 0.0f;
}

void GpuProfiler::destroy() {
    pool_ = nullptr;
    frames_.clear();
    timings_.clear();
    supported_ = false;
}

void GpuProfiler::begin(const vk::raii::CommandBuffer& cmd, std::uint32_t frame) {
    if (!supported_) {
        return;
    }
    FrameMarks& marks = frames_[frame];
    marks.names.clear();
    marks.recorded = true;
    cmd.resetQueryPool(*pool_, frame * kMaxMarks, kMaxMarks);
    cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *pool_, frame * kMaxMarks);
    marks.names.push_back(nullptr);
}

void GpuProfiler::mark(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                       const char* name) {
    if (!supported_) {
        return;
    }
    FrameMarks& marks = frames_[frame];
    if (!marks.recorded || marks.names.size() >= kMaxMarks) {
        return;
    }
    const auto index = static_cast<std::uint32_t>(marks.names.size());
    cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *pool_,
                        frame * kMaxMarks + index);
    marks.names.push_back(name);
}

void GpuProfiler::collect(std::uint32_t frame) {
    if (!supported_ || frame >= frames_.size()) {
        return;
    }
    FrameMarks& marks = frames_[frame];
    const auto count = static_cast<std::uint32_t>(marks.names.size());
    if (!marks.recorded || count < 2) {
        return;
    }
    marks.recorded = false;

    // La fence de este hueco ya se espero: los resultados estan escritos.
    const auto [result, stamps] = pool_.getResults<std::uint64_t>(
        frame * kMaxMarks, count, count * sizeof(std::uint64_t), sizeof(std::uint64_t),
        vk::QueryResultFlagBits::e64);
    if (result != vk::Result::eSuccess) {
        return;
    }

    // Las pasadas que este frame no se grabaron (el mapa de lluvia, que se
    // dibuja una vez) bajan hacia 0 en vez de quedarse con su ultimo valor.
    for (GpuTiming& timing : timings_) {
        const bool seen = std::any_of(marks.names.begin() + 1, marks.names.end(),
                                      [&](const char* name) { return timing.name == name; });
        if (!seen) {
            timing.milliseconds -= timing.milliseconds * kSmoothing;
        }
    }

    float total = 0.0f;
    for (std::uint32_t i = 1; i < count; ++i) {
        const std::uint64_t delta = (stamps[i] - stamps[i - 1]) & valid_mask_;
        const float ms = static_cast<float>(static_cast<double>(delta) * period_ns_ * 1e-6);
        total += ms;

        const char* name = marks.names[i];
        auto it = std::find_if(timings_.begin(), timings_.end(),
                               [&](const GpuTiming& t) { return t.name == name; });
        if (it == timings_.end()) {
            timings_.push_back(GpuTiming{name, ms});
        } else {
            it->milliseconds += (ms - it->milliseconds) * kSmoothing;
        }
    }
    total_ms_ = total_ms_ <= 0.0f ? total : total_ms_ + (total - total_ms_) * kSmoothing;
}

}  // namespace cramion::gfx
