// Pruebas de la edicion multiple del Inspector (sin ventana): valores
// distintos, copiar solo el campo editado, grupos, ejes de un Vec3 y listas.
#include "PropertyInspector.h"

#include <cstdio>

using namespace cramion;
using namespace cramion::editor;

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "OK   " : "FALLO", what);
    if (!ok) ++failures;
}

template <typename T>
FieldValues collect(T& component) {
    FieldValues values;
    CollectFieldsVisitor visitor(values);
    component.reflect(visitor);
    return values;
}

template <typename T>
void apply(T& component, const std::string& path, const FieldValue& value) {
    ApplyFieldVisitor visitor(path, value);
    component.reflect(visitor);
}

int main() {
    std::printf("Edicion multiple\n");

    // --- Campos simples y grupos (Rigidbody) ---
    physics::Rigidbody a;
    physics::Rigidbody b;
    b.mass = 5.0f;
    b.lock_position_x = true;
    const MixedFields mixed = mixedFields({collect(a), collect(b)});
    check(mixed.count("mass") == 1, "masa distinta -> \"—\"");
    check(mixed.count("type") == 0 && mixed.count("linear_damping") == 0, "lo igual no sale como distinto");
    check(mixed.count("Restricciones/freeze_pos_x") == 1, "campos dentro de un grupo");

    FieldValue mass;
    mass.kind = FieldValue::Kind::Float;
    mass.f = 10.0f;
    apply(b, "mass", mass);
    check(b.mass == 10.0f && b.linear_damping == a.linear_damping && b.lock_position_x, "copia solo ese campo");

    FieldValue freeze;
    freeze.kind = FieldValue::Kind::Bool;
    freeze.b = true;
    apply(a, "Restricciones/freeze_pos_y", freeze);
    check(a.lock_position_y && !a.lock_position_x, "campo de un grupo");

    FieldValue wrong_kind;
    wrong_kind.kind = FieldValue::Kind::Int;
    wrong_kind.i = 3;
    const float before = a.mass;
    apply(a, "mass", wrong_kind);
    check(a.mass == before, "otro tipo en la misma ruta no se aplica");

    FieldValue bad_enum;
    bad_enum.kind = FieldValue::Kind::Enum;
    bad_enum.i = 99;
    apply(a, "type", bad_enum);
    check(a.type == physics::BodyType::Dynamic, "enum fuera de rango no se aplica");

    // --- Vec3: solo los ejes tocados (BoxCollider) ---
    physics::BoxCollider box_a;
    physics::BoxCollider box_b;
    box_b.size = core::Vec3{2.0f, 1.0f, 3.0f};
    const MixedFields box_mixed = mixedFields({collect(box_a), collect(box_b)});
    check(box_mixed.count("size") == 1 && box_mixed.at("size") == 0x5, "ejes distintos (x y z, no y)");
    FieldValue size;
    size.kind = FieldValue::Kind::Vec3;
    size.v3 = core::Vec3{5.0f, 7.0f, 9.0f};
    size.axes = 0x2;
    apply(box_b, "size", size);
    check(box_b.size.x == 2.0f && box_b.size.y == 7.0f && box_b.size.z == 3.0f, "Vec3: solo el eje Y");

    // --- Listas (puntos de un riel) ---
    cinema::DollyTrack track_a;
    cinema::DollyTrack track_b;
    track_b.waypoints[1].roll = 30.0f;
    track_b.waypoints.push_back({});
    const MixedFields list_mixed = mixedFields({collect(track_a), collect(track_b)});
    check(list_mixed.count("waypoints") == 1, "cantidad de elementos distinta");
    check(list_mixed.count("waypoints/1/roll") == 1 && list_mixed.count("waypoints/0/roll") == 0,
          "campo de un elemento de la lista");

    FieldValue roll;
    roll.kind = FieldValue::Kind::Float;
    roll.f = -12.0f;
    apply(track_a, "waypoints/2/roll", roll);
    check(track_a.waypoints[2].roll == -12.0f && track_a.waypoints[1].roll == 0.0f, "elemento concreto");

    FieldValue count;
    count.kind = FieldValue::Kind::ListCount;
    count.count = 5;
    apply(track_a, "waypoints", count);
    check(track_a.waypoints.size() == 5, "anadir elementos (+)");

    FieldValue remove;
    remove.kind = FieldValue::Kind::ListRemove;
    remove.i = 0;
    const core::Vec3 second = track_a.waypoints[1].position;
    apply(track_a, "waypoints", remove);
    check(track_a.waypoints.size() == 4 && track_a.waypoints[0].position.x == second.x, "quitar un elemento (x)");

    std::printf("\n%d fallos\n", failures);
    return failures == 0 ? 0 : 1;
}
