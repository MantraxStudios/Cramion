// Decodificador vacio: las plataformas sin backend de video (servidor de
// Linux; Android hasta la Fase 2 con AMediaCodec). open() falla con un motivo
// claro y el reproductor pasa a PlayerState::Error sin bloquear nada.

#include "DecoderInternal.h"

#include <string>
#include <utility>

namespace cramion::video::detail {

namespace {

class NullDecoder final : public IVideoDecoder {
public:
    explicit NullDecoder(std::string reason) : reason_(std::move(reason)) {}

    bool open(const std::string&, const DecodeOptions&, std::string& error) override {
        error = reason_;
        return false;
    }
    StreamInfo info() const override {
        StreamInfo s;
        s.backend = "None";
        return s;
    }
    DecodeEvent next(VideoFrame&, AudioChunk&, std::string& error) override {
        error = reason_;
        return DecodeEvent::Error;
    }
    bool seek(double) override { return false; }
    void interrupt() override {}

private:
    std::string reason_;
};

}  // namespace

std::unique_ptr<IVideoDecoder> makeNullDecoder(std::string reason) {
    return std::make_unique<NullDecoder>(std::move(reason));
}

std::string platformUnsupportedReason() {
#if defined(_WIN32)
    return "Media Foundation no esta disponible";
#elif defined(__ANDROID__)
    return "La reproduccion de video en Android llega en la Fase 2 (AMediaCodec)";
#else
    return "Esta plataforma no reproduce video (el servidor de Linux ignora los VideoPlayer)";
#endif
}

}  // namespace cramion::video::detail
