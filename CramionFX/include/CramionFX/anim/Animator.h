#ifndef CRAMION_ANIM_ANIMATOR_H
#define CRAMION_ANIM_ANIMATOR_H

#include "CramionFX/asset/Model.h"
#include "CramionFX/core/Math.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cramion::anim {

// Un clip en una mezcla (Blend Trees y fundidos entre estados del Animator).
struct ClipSample {
    std::int32_t clip = -1;  // -1 = pose de reposo
    float time = 0.0f;       // segundos dentro del clip
    float weight = 0.0f;
    // Segundos de clip avanzados este frame hasta `time` (con signo; puede
    // pasar de la duracion en bucle). Solo lo usa el root motion; 0 = quieto.
    float delta = 0.0f;
    bool loop = true;  // root motion: el clip da la vuelta al acabar
};

// Pose local de cada nodo descompuesta (posicion, giro y escala). Las capas,
// las aditivas y los montages se mezclan asi antes de componer las matrices.
struct LocalPose {
    std::vector<core::Vec3> translation;
    std::vector<core::Quat> rotation;
    std::vector<core::Vec3> scale;

    std::size_t size() const { return translation.size(); }
    void resize(std::size_t n) {
        translation.resize(n);
        rotation.resize(n);
        scale.resize(n);
    }
};

// Movimiento de la raiz (root motion) en un intervalo, en el espacio del
// modelo: desplazamiento en XZ y giro en Y (radianes, positivo = el de un
// cuaternion sobre +Y). El desplazamiento va en los ejes que tenia el
// personaje al empezar el intervalo.
struct RootMotion {
    core::Vec3 position{};
    float yaw = 0.0f;
};

// Como se suma una capa a lo de debajo.
enum class LayerBlendMode : int {
    Override = 0,  // la sustituye (con su peso y su mascara)
    Additive = 1,  // le suma la diferencia con su pose de referencia
};

// Una capa de una pose por capas (Animator con capas o montage en un slot).
struct PoseLayerJob {
    std::vector<ClipSample> samples;
    // Aditiva: la pose que se resta. Vacia = el primer fotograma de cada clip
    // de `samples`.
    std::vector<ClipSample> reference;
    float weight = 1.0f;
    LayerBlendMode mode = LayerBlendMode::Override;
    // Peso por nodo (0..1, ver ecs::buildMaskWeights); nulo = todo el cuerpo.
    std::shared_ptr<const std::vector<float>> mask;
    // Override: su root motion se mezcla con el de debajo (montage de cuerpo
    // entero: la esquiva avanza aunque el grafo estuviera parado).
    bool root_motion = false;
};

// Pose por capas: la base (la maquina de la capa 0) y las capas de encima en
// orden. Se rellena en serie y se evalua despues, en paralelo con la de los
// demas personajes (evaluateLayered solo toca su animador).
struct LayeredPosePlan {
    std::vector<ClipSample> base;
    std::vector<PoseLayerJob> layers;
    std::size_t layer_count = 0;  // las que cuentan (layers se reutiliza)

    void clear() {
        base.clear();
        layer_count = 0;
    }
    PoseLayerJob& addLayer() {
        if (layer_count >= layers.size()) layers.emplace_back();
        PoseLayerJob& job = layers[layer_count++];
        job.samples.clear();
        job.reference.clear();
        job.weight = 1.0f;
        job.mode = LayerBlendMode::Override;
        job.mask.reset();
        job.root_motion = false;
        return job;
    }
};

// Caja alineada con los ejes.
struct Aabb {
    core::Vec3 min{};
    core::Vec3 max{};
};

// Reproduce un clip de un modelo y calcula la matriz final de cada hueso.
//
// La pose se evalua en tres pasos:
//
//   1. Local:  cada nodo animado interpola sus claves (lerp para posicion y
//              escala, slerp para la rotacion). Los que no tienen pista
//              conservan su transformacion de reposo.
//   2. Global: se recorre la jerarquia de padres a hijos acumulando
//              global = global(padre) * local.
//   3. Hueso:  global(nodo del hueso) * offset. El offset lleva el vertice
//              al espacio del hueso en reposo; la global lo devuelve al
//              modelo ya en la pose animada.
//
// No hay tope de huesos: las matrices van en un std::vector del tamano del
// esqueleto y en la GPU en un storage buffer de longitud variable.
class Animator {
public:
    Animator() = default;
    explicit Animator(const asset::ModelData& model);

    // Clip a reproducir; -1 = pose de reposo.
    void play(std::int32_t clip, bool loop = true);

    // Avanza el tiempo y recalcula las matrices de los huesos.
    void update(float delta_seconds);
    // Solo avanza el tiempo (como update, sin evaluar): la pose se calcula
    // despues con evaluate(), p. ej. en otro hilo junto a la de otros
    // personajes.
    void advance(float delta_seconds);

    // Recalcula la pose en el instante actual sin avanzar el tiempo.
    void evaluate();

    // Pose mezclando varios clips con sus pesos (se normalizan): posicion y
    // escala promediadas, rotacion por nlerp en el mismo hemisferio. Los nodos
    // sin pista en un clip aportan su pose de reposo. Sin muestras (o peso 0),
    // la pose de reposo. No toca el clip ni el tiempo de play().
    void evaluateBlend(const std::vector<ClipSample>& samples) { evaluateBlend(samples.data(), samples.size()); }
    void evaluateBlend(const ClipSample* samples, std::size_t count);

    // --- Capas, aditivas y root motion ------------------------------------------
    // Mezcla de muestras como evaluateBlend (la misma cuenta) en una pose local
    // sin componer. Con root motion activo quita a la raiz lo que avanza y gira
    // y, si `motion` no es nulo, deja ahi su movimiento.
    void samplePose(const ClipSample* samples, std::size_t count, LocalPose& out, RootMotion* motion = nullptr);
    // La pose de reposo descompuesta.
    void restPose(LocalPose& out) const;
    // Compone una pose local: matrices locales, globales y de los huesos.
    void applyPose(const LocalPose& pose);
    // Base y capas (override con mascara, aditivas con su referencia) y root
    // motion de lo que lo aporta.
    void evaluateLayered(const LayeredPosePlan& plan);

    // Root motion: nodo raiz cuyo avance (XZ) y giro (Y) se sacan de la pose y
    // quedan en rootMotion() tras cada evaluateBlend / evaluateLayered.
    // -1 = apagado (lo de siempre: la pose tal cual).
    void setRootMotionNode(std::int32_t node);
    std::int32_t rootMotionNode() const { return root_node_; }
    const RootMotion& rootMotion() const { return root_motion_; }

    // Una matriz por hueso de asset::ModelData::bones, en espacio del modelo.
    const std::vector<core::Mat4>& boneMatrices() const { return bone_matrices_; }
    // Solo las matrices de los huesos de otro animador del mismo modelo (lo que
    // lee el render): sin copiar los clips, la pose local y la global.
    void copyBonesFrom(const Animator& other) { bone_matrices_ = other.bone_matrices_; }

    // Pose de cada nodo (espacio del padre y del modelo) tras evaluate(): la
    // cinematica inversa la retoca y luego rehace las matrices con
    // updateBones().
    std::vector<core::Mat4>& locals() { return local_; }
    std::vector<core::Mat4>& globals() { return global_; }
    const asset::ModelData* model() const { return model_; }
    void updateBones();

    float time() const { return time_; }
    void setTime(float seconds);
    void setSpeed(float speed) { speed_ = speed; }

    std::int32_t clip() const { return clip_; }

private:
    const asset::ModelData* model_ = nullptr;

    std::int32_t clip_ = -1;
    bool loop_ = true;
    float time_ = 0.0f;
    float speed_ = 1.0f;

    // Pista del clip actual para cada nodo (-1 = sin animar).
    std::vector<std::int32_t> node_channel_;
    // Para evaluateBlend: pista de cada clip por nodo (se rellena al usarlo;
    // el modelo puede ganar clips .cranim despues) y la pose de reposo
    // descompuesta.
    std::vector<std::vector<std::int32_t>> clip_channels_;
    std::vector<core::Vec3> rest_translation_;
    std::vector<core::Quat> rest_rotation_;
    std::vector<core::Vec3> rest_scale_;
    // Muestras que cuentan en evaluateBlend (se reutiliza entre frames).
    struct BlendActive {
        const asset::AnimationClip* clip;
        const std::vector<std::int32_t>* channels;
        float time;
        float weight;
    };
    std::vector<BlendActive> blend_active_;
    // Rellena blend_active_ con las muestras que cuentan; devuelve la suma de pesos.
    float gatherActive(const ClipSample* samples, std::size_t count);
    // Pose local de un nodo mezclando blend_active_.
    void blendNode(std::size_t node, float total, core::Vec3& t, core::Quat& r, core::Vec3& s) const;

    // Root motion.
    std::int32_t root_node_ = -1;
    core::Mat4 root_parent_ = core::Mat4::identity();  // global en reposo del padre de la raiz
    core::Mat4 root_parent_inv_ = core::Mat4::identity();
    core::Quat root_parent_rot_{};
    RootMotion root_motion_{};
    // Por muestra activa (paralelo a blend_active_): delta y bucle.
    std::vector<float> active_delta_;
    std::vector<std::uint8_t> active_loop_;
    void stripRoot(const asset::AnimationClip& clip, std::int32_t channel, core::Vec3& t, core::Quat& r) const;
    RootMotion activeRootMotion(float total) const;
    // Para evaluateLayered (se reutilizan).
    LocalPose layer_base_, layer_pose_, layer_ref_;

    std::vector<core::Mat4> local_;
    std::vector<core::Mat4> global_;
    std::vector<core::Mat4> bone_matrices_;
};

// Root motion de un clip entre `time - delta` y `time` (segundos; en bucle da
// las vueltas que haga falta) para el nodo `node` del modelo.
RootMotion clipRootMotion(const asset::ModelData& model, const asset::AnimationClip& clip, std::int32_t node,
                          float time, float delta, bool loop);

// Gira un vector `angle` radianes sobre +Y.
core::Vec3 rotateAboutY(const core::Vec3& v, float angle);

// Mezclas de poses locales. `mask`: peso por nodo (nulo = 1 en todos).
void blendPoseOverride(LocalPose& base, const LocalPose& layer, float weight, const std::vector<float>* mask);
// pose = pose - reference (posicion restada, giro inverso(ref) * giro, escala dividida).
void makeAdditivePose(LocalPose& pose, const LocalPose& reference);
// base += delta * peso (giro: base * slerp(identidad, delta, peso)).
void applyAdditivePose(LocalPose& base, const LocalPose& delta, float weight, const std::vector<float>* mask);

// Eventos de un clip que se cruzan al pasar de `time - delta` a `time`
// (segundos). Cada evento sale como mucho UNA vez por llamada aunque el salto
// cubra varias vueltas. `include_start`: tambien los que estan justo en el
// punto de partida (el estado o el montage acaba de empezar). En bucle cuenta
// las vueltas; si no, el intervalo se recorta a [0, duracion]. Anade a `out`
// los indices de clip.events.
void collectClipEvents(const asset::AnimationClip& clip, float time, float delta, bool loop, bool include_start,
                       std::vector<std::uint32_t>& out);

// Deforma todos los vertices en la CPU con la pose dada y devuelve su caja.
// Se usa una vez al cargar, para colocar y escalar el modelo en el mundo.
Aabb skinnedBounds(const asset::ModelData& model, const std::vector<core::Mat4>& bones);

}  // namespace cramion::anim

#endif  // CRAMION_ANIM_ANIMATOR_H
