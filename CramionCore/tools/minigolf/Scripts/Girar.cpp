// Girar.cpp: gira sin parar (las aspas del molino, la camara del menu...).
#include <cramion/Script.h>

using namespace cramion;

class Girar : public Script {
public:
    Property<Vec3> eje{this, "eje", Vec3{0, 0, 1}, Tooltip("Eje local")};
    Property<float> velocidad{this, "velocidad", 60.0f, Tooltip("Grados por segundo")};

    void update(float dt) override { entity().rotate(eje.get() * (velocidad * dt)); }
};

CRAMION_SCRIPT(Girar)
