// Orbita.cpp: camara del menu; da vueltas lentas alrededor del campo.
#include <cramion/Script.h>

#include <cmath>

using namespace cramion;

class Orbita : public Script {
public:
    Property<Vec3> centro{this, "centro", Vec3{0, 0, -4}};
    Property<float> radio{this, "radio", 9.0f};
    Property<float> altura{this, "altura", 5.0f};
    Property<float> velocidad{this, "velocidad", 6.0f, Tooltip("Grados por segundo")};

    void start() override { angulo_ = 30.0f; }

    void update(float dt) override {
        angulo_ += velocidad * dt;
        const float r = angulo_ * Mathf::deg2rad;
        const Vec3 c = centro.get();
        entity().setPosition(c + Vec3{std::sin(r) * radio, altura, std::cos(r) * radio});
        entity().lookAt(c);
    }

private:
    float angulo_ = 30.0f;
};

CRAMION_SCRIPT(Orbita)
