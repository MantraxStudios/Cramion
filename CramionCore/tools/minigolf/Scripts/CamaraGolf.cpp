// CamaraGolf.cpp: camara que sigue a la pelota por detras de la direccion de
// tiro. Rueda del raton: acercar / alejar.
#include "Pelota.h"

#include <cmath>

class CamaraGolf : public Script {
public:
    Property<float> distancia{this, "distancia", 3.2f};
    Property<float> altura{this, "altura", 1.7f};
    Property<float> suavidad{this, "suavidad", 5.0f};

    void start() override {
        pelota_ = Scene::find("Pelota");
        zoom_ = 1.0f;
        empezada_ = false;
    }

    void lateUpdate(float dt) override {
        if (!pelota_) return;
        const Pelota* s = pelota_.script<Pelota>();
        const float r = (s != nullptr ? s->yaw() : 0.0f) * Mathf::deg2rad;
        const Vec3 dir{-std::sin(r), 0, -std::cos(r)};

        const float rueda = Input::mouse().wheel;
        if (rueda != 0.0f) zoom_ = Mathf::clamp(zoom_ - rueda * 0.12f, 0.55f, 2.4f);

        const Vec3 objetivo = pelota_.position();
        const float k = 1.0f - std::exp(-dt * suavidad * 1.4f);
        foco_ = empezada_ ? Vec3::lerp(foco_, objetivo, k) : objetivo;
        const Vec3 deseada = foco_ - dir * (distancia * zoom_) + Vec3{0, altura * zoom_, 0};
        const float kp = 1.0f - std::exp(-dt * suavidad);
        pos_ = empezada_ ? Vec3::lerp(pos_, deseada, kp) : deseada;
        empezada_ = true;
        entity().setPosition(pos_);
        entity().lookAt(foco_ + dir * 0.8f + Vec3{0, 0.15f, 0});
    }

private:
    Entity pelota_;
    float zoom_ = 1.0f;
    bool empezada_ = false;
    Vec3 foco_, pos_;
};

CRAMION_SCRIPT(CamaraGolf)
