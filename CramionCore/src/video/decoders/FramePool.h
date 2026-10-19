#ifndef CRAMION_VIDEO_FRAME_POOL_H
#define CRAMION_VIDEO_FRAME_POOL_H

// Interno de src/video/decoders: buferes de los frames reciclados (los de
// MediaCodec y FFmpeg; Media Foundation tiene el suyo en MfDecoder.cpp). Un
// frame NV12 de 1080p son 3 MB: sin pool cada frame era una reserva nueva.
// El frame mantiene vivo su bufer (VideoFrame::owner); al soltarlo vuelve al
// pool si sigue existiendo (si no, se libera).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace cramion::video::detail {

class FramePool : public std::enable_shared_from_this<FramePool> {
public:
    std::shared_ptr<std::vector<std::uint8_t>> get(std::size_t size) {
        std::unique_ptr<std::vector<std::uint8_t>> buffer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = free_.begin(); it != free_.end(); ++it) {
                if ((*it)->capacity() >= size) {
                    buffer = std::move(*it);
                    free_.erase(it);
                    break;
                }
            }
        }
        if (!buffer) buffer = std::make_unique<std::vector<std::uint8_t>>();
        buffer->resize(size);
        std::weak_ptr<FramePool> weak = weak_from_this();
        return std::shared_ptr<std::vector<std::uint8_t>>(buffer.release(), [weak](std::vector<std::uint8_t>* b) {
            if (auto pool = weak.lock()) {
                std::lock_guard<std::mutex> lock(pool->mutex_);
                if (pool->free_.size() < kKeep) {
                    pool->free_.emplace_back(b);
                    return;
                }
            }
            delete b;
        });
    }

private:
    static constexpr std::size_t kKeep = 6;
    std::mutex mutex_;
    std::vector<std::unique_ptr<std::vector<std::uint8_t>>> free_;
};

}  // namespace cramion::video::detail

#endif  // CRAMION_VIDEO_FRAME_POOL_H
