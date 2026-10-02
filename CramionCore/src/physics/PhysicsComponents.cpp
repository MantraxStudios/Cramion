#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/Destruction.h"

#include "CramionCore/physics/Cloth.h"
#include "CramionCore/physics/SoftBody.h"

#include "CramionCore/asset/AssetTypes.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/modeling/EditableMesh.h"
#include "CramionCore/physics/Particles.h"

#include <array>

namespace cramion::physics {

using ecs::FloatRange;
using ecs::Vec3Kind;

namespace {

void reflectMaterial(ecs::PropertyVisitor& v, ColliderMaterial& m) {
    v.field({"is_trigger", "Es trigger",
             "Detecta a los que entran y salen (eventos Trigger) pero no choca: zonas, "
             "puertas, recogibles..."},
            m.is_trigger);
    v.field({"friction", "Friccion", "0 = hielo, 1 = goma (se promedia con la del otro)"}, m.friction,
            FloatRange{0.0f, 2.0f, 0.01f, "%.2f", true});
    v.field({"bounciness", "Rebote", "0 = no rebota, 1 = rebote perfecto (manda el mayor)"},
            m.bounciness, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    if (v.beginGroup("Anular capas", false)) {
        v.layerMask({"include_layers", "Incluir (permitir)",
                     "Chocar con estas capas aunque la matriz de colisiones diga que no"},
                    m.include_layers);
        v.layerMask({"exclude_layers", "Excluir (denegar)",
                     "No chocar con estas capas aunque la matriz diga que si (gana a Incluir)"},
                    m.exclude_layers);
        v.endGroup();
    }
}

}  // namespace

void Rigidbody::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 3> kTypes = {"Estatico", "Dinamico", "Cinematico"};
    ecs::enumField(v, {"type", "Tipo",
                       "Dinamico: lo mueve la fisica. Cinematico: lo mueve su Transform y empuja "
                       "a los demas. Estatico: no se mueve"},
                   type, kTypes);
    const bool all = v.wantsAllFields();
    if (all || type == BodyType::Dynamic) {
        v.field({"mass", "Masa"}, mass, FloatRange{0.001f, 100000.0f, 0.05f, "%.3f kg"});
        v.field({"linear_damping", "Amortiguamiento lineal"}, linear_damping,
                FloatRange{0.0f, 10.0f, 0.01f, "%.2f"});
        v.field({"angular_damping", "Amortiguamiento angular"}, angular_damping,
                FloatRange{0.0f, 10.0f, 0.01f, "%.2f"});
        v.field({"use_gravity", "Usar gravedad"}, use_gravity);
        if (all || use_gravity) {
            v.field({"gravity_scale", "Escala de gravedad"}, gravity_scale,
                    FloatRange{-10.0f, 10.0f, 0.01f, "%.2f"});
        }
        if (v.beginGroup("Restricciones", false)) {
            v.field({"freeze_pos_x", "Congelar posicion X"}, lock_position_x);
            v.field({"freeze_pos_y", "Congelar posicion Y"}, lock_position_y);
            v.field({"freeze_pos_z", "Congelar posicion Z"}, lock_position_z);
            v.field({"freeze_rot_x", "Congelar rotacion X"}, lock_rotation_x);
            v.field({"freeze_rot_y", "Congelar rotacion Y"}, lock_rotation_y);
            v.field({"freeze_rot_z", "Congelar rotacion Z"}, lock_rotation_z);
            v.endGroup();
        }
        v.field({"initial_velocity", "Velocidad inicial", "m/s al entrar en Play"},
                initial_velocity, Vec3Kind::Position);
        v.field({"initial_angular_velocity", "Velocidad angular inicial", "rad/s"},
                initial_angular_velocity, Vec3Kind::Position);
        v.field({"continuous", "Deteccion continua (CCD)",
                 "Para objetos rapidos: no atraviesan paredes finas"},
                continuous);
        if (v.beginGroup("Anular capas", false)) {
            v.layerMask({"include_layers", "Incluir (permitir)",
                         "Todo el cuerpo choca con estas capas aunque la matriz diga que no"},
                        include_layers);
            v.layerMask({"exclude_layers", "Excluir (denegar)",
                         "Todo el cuerpo ignora estas capas aunque la matriz diga que si (gana a Incluir)"},
                        exclude_layers);
            v.endGroup();
        }
        v.field({"allow_sleep", "Puede dormirse",
                 "Quieto un rato deja de simularse hasta que algo lo toca"},
                allow_sleep);
        v.field({"interpolate", "Interpolar",
                 "Mueve el Transform suave entre pasos de fisica (la fisica va a paso fijo y "
                 "se dibuja a mas FPS). Sin interpolar se ve a saltos"},
                interpolate);
    }
}

void BoxCollider::reflect(ecs::PropertyVisitor& v) {
    reflectMaterial(v, material);
    v.field({"size", "Tamano"}, size, Vec3Kind::Scale);
    v.field({"center", "Centro"}, center, Vec3Kind::Position);
}

void SphereCollider::reflect(ecs::PropertyVisitor& v) {
    reflectMaterial(v, material);
    v.field({"radius", "Radio"}, radius, FloatRange{0.001f, 10000.0f, 0.01f, "%.3f m"});
    v.field({"center", "Centro"}, center, Vec3Kind::Position);
}

void CapsuleCollider::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 3> kAxes = {"X", "Y", "Z"};
    reflectMaterial(v, material);
    v.field({"radius", "Radio"}, radius, FloatRange{0.001f, 10000.0f, 0.01f, "%.3f m"});
    v.field({"height", "Altura", "Total, incluidas las semiesferas"}, height,
            FloatRange{0.001f, 10000.0f, 0.01f, "%.3f m"});
    ecs::enumField(v, {"axis", "Eje"}, axis, kAxes);
    v.field({"center", "Centro"}, center, Vec3Kind::Position);
}

void MeshCollider::reflect(ecs::PropertyVisitor& v) {
    reflectMaterial(v, material);
    v.field({"convex", "Convexo",
             "Obligatorio para cuerpos dinamicos y triggers: usa la envolvente convexa de la malla"},
            convex);
}

void PlaneCollider::reflect(ecs::PropertyVisitor& v) {
    reflectMaterial(v, material);
}

void Vehicle::reflect(ecs::PropertyVisitor& v) {
    const bool all = v.wantsAllFields();
    if (v.beginGroup("Motor", true)) {
        v.field({"engine_torque", "Par del motor"}, engine_torque, ecs::FloatRange{10.0f, 10000.0f, 5.0f, "%.0f Nm"});
        v.field({"min_rpm", "RPM minimas"}, min_rpm, ecs::FloatRange{100.0f, 5000.0f, 10.0f, "%.0f"});
        v.field({"max_rpm", "RPM maximas"}, max_rpm, ecs::FloatRange{1000.0f, 20000.0f, 10.0f, "%.0f"});
        ecs::listField(v, {"torque_curve", "Curva de par",
                           "Puntos (rpm 0..1 entre min y max, par 0..1 del maximo), como la curva de Unreal"},
                       torque_curve, [](VehicleCurvePoint& p, ecs::PropertyVisitor& pv) {
                           pv.field({"x", "RPM (0..1)"}, p.x, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
                           pv.field({"y", "Par (0..1)"}, p.y, ecs::FloatRange{0.0f, 1.5f, 0.01f, "%.2f", true});
                       });
        v.field({"engine_inertia", "Inercia del motor", "kg m2: mas = sube de vueltas mas despacio"}, engine_inertia,
                ecs::FloatRange{0.01f, 10.0f, 0.01f, "%.2f"});
        v.field({"engine_damping", "Freno motor", "Cuanto baja de vueltas al soltar el acelerador"}, engine_damping,
                ecs::FloatRange{0.0f, 5.0f, 0.01f, "%.2f"});
        v.endGroup();
    }
    if (v.beginGroup("Caja de cambios", true)) {
        v.field({"automatic", "Cambio automatico", "Sin: manual (subir/bajar marcha por accion o script)"}, automatic);
        ecs::listField(v, {"gears", "Marchas", "Relacion de cada marcha (1a, 2a...)"}, gears,
                       [](VehicleGear& g, ecs::PropertyVisitor& pv) {
                           pv.field({"ratio", "Relacion"}, g.ratio, ecs::FloatRange{0.1f, 10.0f, 0.01f, "%.2f"});
                       });
        v.field({"reverse_ratio", "Marcha atras"}, reverse_ratio, ecs::FloatRange{0.1f, 10.0f, 0.01f, "%.2f"});
        if (all || automatic) {
            v.field({"shift_up_rpm", "Subir marcha a"}, shift_up_rpm, ecs::FloatRange{500.0f, 20000.0f, 10.0f, "%.0f rpm"});
            v.field({"shift_down_rpm", "Bajar marcha a"}, shift_down_rpm, ecs::FloatRange{100.0f, 20000.0f, 10.0f, "%.0f rpm"});
            v.field({"shift_time", "Tiempo de cambio"}, shift_time, ecs::FloatRange{0.0f, 3.0f, 0.01f, "%.2f s"});
        }
        v.field({"clutch_strength", "Embrague"}, clutch_strength, ecs::FloatRange{0.1f, 100.0f, 0.1f, "%.1f"});
        v.endGroup();
    }
    if (v.beginGroup("Diferenciales", false)) {
        v.field({"final_drive", "Grupo final", "Relacion del diferencial (mas = mas aceleracion, menos punta)"},
                final_drive, ecs::FloatRange{0.5f, 10.0f, 0.01f, "%.2f"});
        v.field({"limited_slip", "Autoblocante",
                 "Relacion maxima entre la rueda rapida y la lenta antes de pasar el par a la lenta (10 = abierto)"},
                limited_slip, ecs::FloatRange{1.0f, 10.0f, 0.01f, "%.2f"});
        v.field({"front_torque_split", "Reparto delante (4x4)", "Parte del par al eje delantero si los dos traccionan"},
                front_torque_split, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.endGroup();
    }
    if (v.beginGroup("Estabilidad", false)) {
        v.field({"max_pitch_roll", "Inclinacion maxima", "Grados antes de volcar (180 = libre)"}, max_pitch_roll,
                ecs::FloatRange{5.0f, 180.0f, 1.0f, "%.0f°", true});
        v.field({"anti_roll_bars", "Barras estabilizadoras", "Reducen el balanceo en las curvas (por eje)"}, anti_roll_bars);
        if (all || anti_roll_bars) {
            v.field({"anti_roll_stiffness", "Rigidez de las barras"}, anti_roll_stiffness,
                    ecs::FloatRange{0.0f, 100000.0f, 10.0f, "%.0f N/m"});
        }
        v.field({"center_of_mass_offset", "Centro de masas", "Desplazamiento del centro de masas (mas bajo = vuelca menos)"},
                center_of_mass_offset, ecs::Vec3Kind::Position);
        v.endGroup();
    }
    if (v.beginGroup("Entrada", true)) {
        v.field({"keyboard", "Lo conduce el jugador", "Teclado/mando (sin: entity:setVehicleInput desde un script)"},
                keyboard);
        if (all || keyboard) {
            v.field({"use_input_actions", "Usar Input Actions",
                     "Lee las acciones del proyecto (teclado, mando y tactil). Sin: W/S, A/D y Espacio"},
                    use_input_actions);
            if (all || use_input_actions) {
                v.field({"drive_action", "Accion conducir", "Vec2: Y acelerar/atras, X girar (Move por defecto)"},
                        drive_action);
                v.field({"handbrake_action", "Accion freno de mano"}, handbrake_action);
                v.field({"brake_action", "Accion freno", "Opcional (si no: atras frena mientras avanza)"}, brake_action);
                v.field({"shift_up_action", "Accion subir marcha", "Cambio manual"}, shift_up_action);
                v.field({"shift_down_action", "Accion bajar marcha", "Cambio manual"}, shift_down_action);
            }
        }
        v.field({"steer_speed", "Velocidad del volante", "Giro por segundo (0 = al instante)"}, steer_speed,
                ecs::FloatRange{0.0f, 50.0f, 0.1f, "%.1f"});
        v.endGroup();
    }
    if (v.beginGroup("Sonido y efectos", false)) {
        v.entity({"engine_sound", "Sonido del motor", "Objeto con AudioSource en bucle: su tono sigue a las rpm"},
                 engine_sound);
        v.field({"engine_pitch_min", "Tono al ralenti"}, engine_pitch_min, ecs::FloatRange{0.1f, 3.0f, 0.01f, "%.2f"});
        v.field({"engine_pitch_max", "Tono al maximo"}, engine_pitch_max, ecs::FloatRange{0.1f, 4.0f, 0.01f, "%.2f"});
        v.field({"engine_volume_min", "Volumen sin acelerar"}, engine_volume_min, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"engine_volume_max", "Volumen acelerando"}, engine_volume_max, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.entity({"skid_sound", "Sonido de derrape", "Objeto con AudioSource en bucle: su volumen sigue al derrape"},
                 skid_sound);
        v.entity({"skid_particles", "Humo de derrape", "Objeto con ParticleSystem: emite segun el derrape"},
                 skid_particles);
        v.field({"skid_threshold", "Umbral de derrape"}, skid_threshold, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.endGroup();
    }
}

void WheelCollider::reflect(ecs::PropertyVisitor& v) {
    v.field({"radius", "Radio"}, radius, ecs::FloatRange{0.05f, 5.0f, 0.01f, "%.2f m"});
    v.field({"width", "Ancho"}, width, ecs::FloatRange{0.02f, 3.0f, 0.01f, "%.2f m"});
    v.field({"mass", "Masa", "Inercia de la rueda (cilindro)"}, mass, ecs::FloatRange{0.5f, 500.0f, 0.5f, "%.1f kg"});
    if (v.beginGroup("Suspension", true)) {
        v.field({"suspension_min", "Suspension min"}, suspension_min, ecs::FloatRange{0.0f, 3.0f, 0.01f, "%.2f m"});
        v.field({"suspension_max", "Suspension max"}, suspension_max, ecs::FloatRange{0.01f, 3.0f, 0.01f, "%.2f m"});
        v.field({"suspension_preload", "Precarga"}, suspension_preload, ecs::FloatRange{0.0f, 1.0f, 0.005f, "%.3f m"});
        v.field({"spring_frequency", "Dureza (Hz)"}, spring_frequency, ecs::FloatRange{0.1f, 10.0f, 0.05f, "%.2f"});
        v.field({"damping", "Amortiguacion"}, damping, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.endGroup();
    }
    v.field({"max_steer_angle", "Giro maximo", "Grados; 0 = no gira (ruedas traseras)"}, max_steer_angle,
            ecs::FloatRange{0.0f, 80.0f, 0.5f, "%.1f°", true});
    v.field({"drive", "Traccion", "Recibe el par del motor"}, drive);
    v.field({"max_brake_torque", "Freno"}, max_brake_torque, ecs::FloatRange{0.0f, 20000.0f, 10.0f, "%.0f Nm"});
    v.field({"max_handbrake_torque", "Freno de mano"}, max_handbrake_torque, ecs::FloatRange{0.0f, 20000.0f, 10.0f, "%.0f Nm"});
    v.field({"grip", "Agarre"}, grip, ecs::FloatRange{0.05f, 3.0f, 0.01f, "%.2f"});
    if (v.beginGroup("Friccion adelante", false)) {
        v.field({"forward_extremum_slip", "Deslizamiento del pico"}, forward_extremum_slip,
                ecs::FloatRange{0.001f, 1.0f, 0.001f, "%.3f"});
        v.field({"forward_extremum_value", "Agarre del pico"}, forward_extremum_value, ecs::FloatRange{0.0f, 3.0f, 0.01f, "%.2f"});
        v.field({"forward_asymptote_slip", "Deslizamiento al derrapar"}, forward_asymptote_slip,
                ecs::FloatRange{0.002f, 2.0f, 0.001f, "%.3f"});
        v.field({"forward_asymptote_value", "Agarre al derrapar"}, forward_asymptote_value, ecs::FloatRange{0.0f, 3.0f, 0.01f, "%.2f"});
        v.endGroup();
    }
    if (v.beginGroup("Friccion lateral", false)) {
        v.field({"sideways_extremum_slip", "Angulo del pico"}, sideways_extremum_slip, ecs::FloatRange{0.1f, 45.0f, 0.1f, "%.1f°"});
        v.field({"sideways_extremum_value", "Agarre del pico"}, sideways_extremum_value, ecs::FloatRange{0.0f, 3.0f, 0.01f, "%.2f"});
        v.field({"sideways_asymptote_slip", "Angulo al derrapar"}, sideways_asymptote_slip, ecs::FloatRange{0.2f, 90.0f, 0.1f, "%.1f°"});
        v.field({"sideways_asymptote_value", "Agarre al derrapar"}, sideways_asymptote_value, ecs::FloatRange{0.0f, 3.0f, 0.01f, "%.2f"});
        v.endGroup();
    }
    v.entity({"visual", "Rueda visible", "Objeto que gira con la rueda (eje X local)"}, visual);
}

void CharacterController::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 2> kMovements = {"Manual (script)", "Integrado"};
    const bool all = v.wantsAllFields();
    v.field({"height", "Altura", "Total, incluidas las semiesferas"}, height, FloatRange{0.1f, 100.0f, 0.01f, "%.2f m"});
    v.field({"radius", "Radio"}, radius, FloatRange{0.05f, 50.0f, 0.01f, "%.2f m"});
    v.field({"center", "Centro", "Centro de la capsula respecto a la entidad: (0, altura/2, 0) = la entidad en los pies"},
            center, Vec3Kind::Position);
    v.field({"slope_limit", "Pendiente maxima", "Rampas mas empinadas son paredes: resbala y no sube"}, slope_limit,
            FloatRange{0.0f, 89.0f, 0.5f, "%.1f°", true});
    v.field({"step_offset", "Altura de escalon", "Sube sola escalones hasta esta altura"}, step_offset,
            FloatRange{0.0f, 2.0f, 0.01f, "%.2f m"});
    v.field({"skin_width", "Grosor de piel", "Distancia que guarda a lo que toca (evita quedarse enganchado)"},
            skin_width, FloatRange{0.001f, 0.2f, 0.001f, "%.3f m"});
    v.field({"stick_to_floor", "Pegarse al suelo", "Al bajar rampas y escalones sigue en el suelo en vez de saltar"},
            stick_to_floor);
    if (v.beginGroup("Empujar", false)) {
        v.field({"push_rigidbodies", "Empujar Rigidbody"}, push_rigidbodies);
        v.field({"mass", "Masa"}, mass, FloatRange{1.0f, 10000.0f, 0.5f, "%.1f kg"});
        v.field({"push_strength", "Fuerza maxima"}, push_strength, FloatRange{0.0f, 100000.0f, 5.0f, "%.0f N"});
        v.endGroup();
    }
    ecs::enumField(v, {"movement", "Movimiento",
                       "Manual: como Unity, solo move() desde un script. Integrado: como Unreal, con "
                       "gravedad, salto, correr y agacharse"},
                   movement, kMovements);
    if (all || movement == CharacterMovement::Integrated) {
        v.field({"keyboard", "Mover con teclado",
                 "WASD/flechas relativo a la camara, Shift correr, Espacio saltar, C o Ctrl agacharse. "
                 "Sin: entity:setMoveInput() desde un script"},
                keyboard);
        v.field({"walk_speed", "Velocidad andando"}, walk_speed, FloatRange{0.0f, 100.0f, 0.05f, "%.2f m/s"});
        v.field({"run_speed", "Velocidad corriendo"}, run_speed, FloatRange{0.0f, 100.0f, 0.05f, "%.2f m/s"});
        v.field({"crouch_speed", "Velocidad agachado"}, crouch_speed, FloatRange{0.0f, 100.0f, 0.05f, "%.2f m/s"});
        v.field({"acceleration", "Aceleracion", "0 = cambia de velocidad al instante"}, acceleration,
                FloatRange{0.0f, 500.0f, 0.5f, "%.1f m/s2"});
        v.field({"air_control", "Control en el aire"}, air_control, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"jump_height", "Altura de salto"}, jump_height, FloatRange{0.0f, 50.0f, 0.05f, "%.2f m"});
        v.field({"max_jumps", "Saltos seguidos", "2 = doble salto"}, max_jumps, 0, 10);
        v.field({"coyote_time", "Tiempo coyote", "Segundos tras salir de un borde en que aun puede saltar"},
                coyote_time, FloatRange{0.0f, 1.0f, 0.01f, "%.2f s"});
        v.field({"gravity_scale", "Escala de gravedad"}, gravity_scale, FloatRange{-10.0f, 10.0f, 0.01f, "%.2f"});
        v.field({"crouch_height", "Altura agachado"}, crouch_height, FloatRange{0.1f, 100.0f, 0.01f, "%.2f m"});
        v.field({"rotate_to_movement", "Girar hacia el movimiento"}, rotate_to_movement);
        if (all || rotate_to_movement) {
            v.field({"rotation_speed", "Velocidad de giro"}, rotation_speed, FloatRange{0.0f, 5000.0f, 5.0f, "%.0f°/s"});
        }
    }
}

void registerPhysicsComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    registry.registerComponent<Rigidbody>("Rigidbody", "Rigidbody", "Fisica");
    registry.registerComponent<BoxCollider>("BoxCollider", "Box Collider", "Fisica");
    registry.registerComponent<SphereCollider>("SphereCollider", "Sphere Collider", "Fisica");
    registry.registerComponent<CapsuleCollider>("CapsuleCollider", "Capsule Collider", "Fisica");
    registry.registerComponent<MeshCollider>("MeshCollider", "Mesh Collider", "Fisica");
    registry.registerComponent<PlaneCollider>("PlaneCollider", "Plane Collider", "Fisica");
    registry.registerComponent<CharacterController>("CharacterController", "Character Controller", "Fisica");
    registry.registerComponent<Vehicle>("Vehicle", "Vehiculo", "Fisica");
    registry.registerComponent<WheelCollider>("WheelCollider", "Wheel Collider", "Fisica");
    registry.registerComponent<Cloth>("Cloth", "Tela (Cloth)", "Fisica");
    registry.registerComponent<SoftBody>("SoftBody", "Cuerpo blando (gelatina)", "Fisica");
    registry.registerComponent<ParticleSystem>("ParticleSystem", "Particle System", "Efectos");
    registerDestructionComponents();  // Destructible (Destruction.h)
    // La malla editable (modelado) va con su MeshCollider: se registra aqui
    // (lo llaman el editor y el reproductor antes de abrir escenas).
    modeling::registerModelingComponents();
}

bool hasCollider(const ecs::Entity& entity) {
    return entity.has<BoxCollider>() || entity.has<SphereCollider>() || entity.has<CapsuleCollider>() ||
           entity.has<MeshCollider>() || entity.has<PlaneCollider>() || entity.has<CharacterController>();
}

void addDefaultCollider(ecs::Entity entity) {
    if (!entity.valid() || hasCollider(entity)) {
        return;
    }
    const ecs::MeshRenderer* renderer = entity.tryGet<ecs::MeshRenderer>();
    if (renderer == nullptr) {
        return;
    }
    const Uuid& model = renderer->model.uuid;
    if (model == assets::builtin::kCube) {
        entity.add<BoxCollider>();
    } else if (model == assets::builtin::kSphere) {
        entity.add<SphereCollider>();
    } else if (model == assets::builtin::kCapsule || model == assets::builtin::kCylinder) {
        entity.add<CapsuleCollider>();
    } else {
        // Plano y modelos importados: su propia malla.
        entity.add<MeshCollider>();
    }
}

}  // namespace cramion::physics
