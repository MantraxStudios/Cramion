// Bandera.cpp: la bandera sube cuando la pelota se acerca (para que pueda entrar).
#include <cramion/Script.h>

#include <cmath>

using namespace cramion;

class Bandera : public Script {
public:
    Property<float> subida{this, "subida", 0.9f};

    void start() override {
        base_ = entity().position();
        pelota_ = Scene::find("Pelota");
        altura_ = 0.0f;
    }

    void update(float dt) override {
        if (!pelota_) return;
        const Vec3 d = pelota_.position() - base_;
        const bool cerca = std::sqrt(d.x * d.x + d.z * d.z) < 1.4f;
        const float objetivo = cerca ? subida.get() : 0.0f;
        altura_ += (objetivo - altura_) * (1.0f - std::exp(-dt * 6.0f));
        entity().setPosition(base_ + Vec3{0, altura_, 0});
    }

private:
    Vec3 base_;
    Entity pelota_;
    float altura_ = 0.0f;
};

CRAMION_SCRIPT(Bandera)
