// GENERADO por cramion_sdkgen: los metodos de Lua de las entidades (entity:metodo(...)).
// Se incluye dentro de cramion::Entity.
/// entity:addCharacterVelocity(...)
template <typename... Mas>
Value addCharacterVelocity(Mas&&... mas) const {
    return call("addCharacterVelocity", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:addClothImpulse(Vec3(0, 0, 3))
/// empujon a su tela (m/s a cada particula libre)
template <typename T0 = Value>
Value addClothImpulse(const T0& arg = {}) const {
    return call("addClothImpulse", Values{Value(arg)});
}
/// entity:addComponent("MeshCollider")
/// anade un componente por su nombre
template <typename T0 = Value>
Value addComponent(const T0& texto = {}) const {
    return call("addComponent", Values{Value(texto)});
}
/// entity:addImpulse(Vec3)
/// impulso
template <typename T0 = Value>
Value addImpulse(const T0& vec = {}) const {
    return call("addImpulse", Values{Value(vec)});
}
/// entity:addRagdollForce(Vec3, "Spine")
/// empujon al ragdoll (en un hueso)
template <typename T0 = Value, typename T1 = Value>
Value addRagdollForce(const T0& vec = {}, const T1& texto = {}) const {
    return call("addRagdollForce", Values{Value(vec), Value(texto)});
}
/// entity:addSoftBodyImpulse(Vec3(0, 5, 0))
/// empujon a su cuerpo blando (m/s a cada particula)
template <typename T0 = Value>
Value addSoftBodyImpulse(const T0& arg = {}) const {
    return call("addSoftBodyImpulse", Values{Value(arg)});
}
/// entity:addTorque(Vec3)
/// par de giro
template <typename T0 = Value>
Value addTorque(const T0& vec = {}) const {
    return call("addTorque", Values{Value(vec)});
}
/// entity.alpha: transparencia de su UIImage/UIText
Value alpha() const { return get("alpha"); }
void setAlpha(const Value& v) const { set("alpha", v); }
/// entity:attachToBone(modelo, "RightHand", offset, giro)
/// sigue a un hueso (Bone Socket)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
Value attachToBone(const T0& modelo = {}, const T1& texto = {}, const T2& offset = {}, const T3& giro = {}) const {
    return call("attachToBone", Values{Value(modelo), Value(texto), Value(offset), Value(giro)});
}
/// entity:audioOcclusion()
/// oclusion de su Audio Listener (true/false)
template <typename... Mas>
Value audioOcclusion(Mas&&... mas) const {
    return call("audioOcclusion", Values{Value(std::forward<Mas>(mas))...});
}
/// entity.castShadows: su MeshRenderer proyecta sombra
Value castShadows() const { return get("castShadows"); }
void setCastShadows(const Value& v) const { set("castShadows", v); }
/// entity:characterState(...)
template <typename... Mas>
Value characterState(Mas&&... mas) const {
    return call("characterState", Values{Value(std::forward<Mas>(mas))...});
}
/// entity.color: Vec3 color de su UIImage/UIText
Value color() const { return get("color"); }
void setColor(const Value& v) const { set("color", v); }
/// entity:detachFromBone()
/// deja de seguir al hueso
template <typename... Mas>
Value detachFromBone(Mas&&... mas) const {
    return call("detachFromBone", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:distanceTo(otra)
/// distancia a otro objeto
template <typename T0 = Value>
Value distanceTo(const T0& otra = {}) const {
    return call("distanceTo", Values{Value(otra)});
}
/// entity:find("hijo")
/// un hijo por nombre
template <typename T0 = Value>
Value find(const T0& texto = {}) const {
    return call("find", Values{Value(texto)});
}
/// entity.gear: marcha del Vehicle
Value gear() const { return get("gear"); }
void setGear(const Value& v) const { set("gear", v); }
/// entity:getBonePosition("Head")
/// Vec3 del hueso en el mundo
template <typename T0 = Value>
Value getBonePosition(const T0& texto = {}) const {
    return call("getBonePosition", Values{Value(texto)});
}
/// entity:getBoneRotation("Head")
/// Quat del hueso
template <typename T0 = Value>
Value getBoneRotation(const T0& texto = {}) const {
    return call("getBoneRotation", Values{Value(texto)});
}
/// entity:getBones()
/// nombres de los huesos
template <typename... Mas>
Value getBones(Mas&&... mas) const {
    return call("getBones", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:getField("Light", "intensity")
/// campo de cualquier componente
template <typename T0 = Value, typename T1 = Value>
Value getField(const T0& texto = {}, const T1& texto2 = {}) const {
    return call("getField", Values{Value(texto), Value(texto2)});
}
/// entity:getFields("Ragdoll")
/// todos los campos de un componente
template <typename T0 = Value>
Value getFields(const T0& texto = {}) const {
    return call("getFields", Values{Value(texto)});
}
/// entity:getNetVar("vida")
/// red: lee una variable sincronizada
template <typename T0 = Value>
Value getNetVar(const T0& texto = {}) const {
    return call("getNetVar", Values{Value(texto)});
}
/// entity:getScript()
/// la instancia de su script
template <typename... Mas>
Value getScript(Mas&&... mas) const {
    return call("getScript", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:getStateMachine(...)
template <typename... Mas>
Value getStateMachine(Mas&&... mas) const {
    return call("getStateMachine", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:hasComponent("Rigidbody")
/// tiene ese componente?
template <typename T0 = Value>
Value hasComponent(const T0& texto = {}) const {
    return call("hasComponent", Values{Value(texto)});
}
/// entity.interactable: su boton/slider/campo responde
Value interactable() const { return get("interactable"); }
void setInteractable(const Value& v) const { set("interactable", v); }
/// entity:isCrouching(...)
template <typename... Mas>
Value isCrouching(Mas&&... mas) const {
    return call("isCrouching", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:isMine()
/// red: este objeto lo controlas tu (sin red, siempre)
template <typename... Mas>
Value isMine(Mas&&... mas) const {
    return call("isMine", Values{Value(std::forward<Mas>(mas))...});
}
/// entity.isMoving: su NavAgent va hacia un destino
Value isMoving() const { return get("isMoving"); }
void setIsMoving(const Value& v) const { set("isMoving", v); }
/// entity:isPlayingSound()
/// suena?
template <typename... Mas>
Value isPlayingSound(Mas&&... mas) const {
    return call("isPlayingSound", Values{Value(std::forward<Mas>(mas))...});
}
/// entity.mesh: malla creada por codigo de su MeshRenderer
Value mesh() const { return get("mesh"); }
void setMesh(const Value& v) const { set("mesh", v); }
/// entity:moveTo(Vec3)
/// su NavAgent camina hasta alli por la malla
template <typename T0 = Value>
Value moveTo(const T0& vec = {}) const {
    return call("moveTo", Values{Value(vec)});
}
/// entity.navVelocity: Vec3 de su NavAgent
Value navVelocity() const { return get("navVelocity"); }
void setNavVelocity(const Value& v) const { set("navVelocity", v); }
/// entity:netId()
/// red: id del objeto (0 = no es de red)
template <typename... Mas>
Value netId(Mas&&... mas) const {
    return call("netId", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:netOwner()
/// red: id del jugador dueno
template <typename... Mas>
Value netOwner(Mas&&... mas) const {
    return call("netOwner", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:new(...)
template <typename... Mas>
Value new_(Mas&&... mas) const {
    return call("new", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:playAnimation("Correr", true)
/// clip del Animator
template <typename T0 = Value, typename T1 = Value>
Value playAnimation(const T0& texto = {}, const T1& activar = {}) const {
    return call("playAnimation", Values{Value(texto), Value(activar)});
}
/// entity:playSound()
/// su Audio Source
template <typename... Mas>
Value playSound(Mas&&... mas) const {
    return call("playSound", Values{Value(std::forward<Mas>(mas))...});
}
/// entity.quaternion: Quat de su giro
Value quaternion() const { return get("quaternion"); }
void setQuaternion(const Value& v) const { set("quaternion", v); }
/// entity.ragdoll: ragdoll activado (true/false)
Value ragdoll() const { return get("ragdoll"); }
void setRagdoll(const Value& v) const { set("ragdoll", v); }
/// entity.remainingDistance: metros que le quedan
Value remainingDistance() const { return get("remainingDistance"); }
void setRemainingDistance(const Value& v) const { set("remainingDistance", v); }
/// entity:removeComponent("MeshCollider")
/// quita un componente
template <typename T0 = Value>
Value removeComponent(const T0& texto = {}) const {
    return call("removeComponent", Values{Value(texto)});
}
/// entity:resetBone("Head")
/// quita los cambios del hueso
template <typename T0 = Value>
Value resetBone(const T0& texto = {}) const {
    return call("resetBone", Values{Value(texto)});
}
/// entity:resetBones()
/// quita los de todos
template <typename... Mas>
Value resetBones(Mas&&... mas) const {
    return call("resetBones", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:resetCloth()
/// su tela (Cloth) vuelve a la pose de reposo
template <typename... Mas>
Value resetCloth(Mas&&... mas) const {
    return call("resetCloth", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:resetSoftBody()
/// su cuerpo blando vuelve a su forma, quieto
template <typename... Mas>
Value resetSoftBody(Mas&&... mas) const {
    return call("resetSoftBody", Values{Value(std::forward<Mas>(mas))...});
}
/// entity.rpm: rpm del motor del Vehicle
Value rpm() const { return get("rpm"); }
void setRpm(const Value& v) const { set("rpm", v); }
/// entity:setAnimatorBool("saltando", true)
template <typename T0 = Value, typename T1 = Value>
Value setAnimatorBool(const T0& texto = {}, const T1& activar = {}) const {
    return call("setAnimatorBool", Values{Value(texto), Value(activar)});
}
/// entity:setAnimatorFloat("velocidad", 1.0)
/// parametro del Animator Controller
template <typename T0 = Value, typename T1 = Value>
Value setAnimatorFloat(const T0& texto = {}, const T1& valor = {}) const {
    return call("setAnimatorFloat", Values{Value(texto), Value(valor)});
}
/// entity:setAnimatorTrigger("atacar")
template <typename T0 = Value>
Value setAnimatorTrigger(const T0& texto = {}) const {
    return call("setAnimatorTrigger", Values{Value(texto)});
}
/// entity:setBoneOffset("Hips", Vec3)
/// desplaza el hueso
template <typename T0 = Value, typename T1 = Value>
Value setBoneOffset(const T0& texto = {}, const T1& vec = {}) const {
    return call("setBoneOffset", Values{Value(texto), Value(vec)});
}
/// entity:setBoneRotation("Head", Vec3 grados)
/// gira el hueso encima de la animacion
template <typename T0 = Value, typename T1 = Value>
Value setBoneRotation(const T0& texto = {}, const T1& arg = {}) const {
    return call("setBoneRotation", Values{Value(texto), Value(arg)});
}
/// entity:setBoneScale("Head", 1.2)
/// escala el hueso
template <typename T0 = Value, typename T1 = Value>
Value setBoneScale(const T0& texto = {}, const T1& valor = {}) const {
    return call("setBoneScale", Values{Value(texto), Value(valor)});
}
/// entity:setFootGrounding(true)
/// patas al suelo
template <typename T0 = Value>
Value setFootGrounding(const T0& activar = {}) const {
    return call("setFootGrounding", Values{Value(activar)});
}
/// entity:setIKHint("PieIzq", objetivo)
/// pole (hacia donde dobla)
template <typename T0 = Value, typename T1 = Value>
Value setIKHint(const T0& texto = {}, const T1& objetivo = {}) const {
    return call("setIKHint", Values{Value(texto), Value(objetivo)});
}
/// entity:setIKTarget("PieIzq", objetivo)
/// objetivo de una cadena IK (entidad, Vec3 o nil)
template <typename T0 = Value, typename T1 = Value>
Value setIKTarget(const T0& texto = {}, const T1& objetivo = {}) const {
    return call("setIKTarget", Values{Value(texto), Value(objetivo)});
}
/// entity:setIKWeight("PieIzq", 1)
/// peso 0..1
template <typename T0 = Value, typename T1 = Value>
Value setIKWeight(const T0& texto = {}, const T1& valor = {}) const {
    return call("setIKWeight", Values{Value(texto), Value(valor)});
}
/// entity:setLookAt(objetivo, peso)
/// mirar con cabeza y cuello
template <typename T0 = Value, typename T1 = Value>
Value setLookAt(const T0& objetivo = {}, const T1& peso = {}) const {
    return call("setLookAt", Values{Value(objetivo), Value(peso)});
}
/// entity:setMaterial(0, "Materials/Brillo.crmat")
/// material .crmat de un hueco de su MeshRenderer
template <typename T0 = Value, typename T1 = Value>
Value setMaterial(const T0& valor = {}, const T1& texto = {}) const {
    return call("setMaterial", Values{Value(valor), Value(texto)});
}
/// entity:setNetVar("vida", 100)
/// red: variable sincronizada (dueno o servidor)
template <typename T0 = Value, typename T1 = Value>
Value setNetVar(const T0& texto = {}, const T1& valor = {}) const {
    return call("setNetVar", Values{Value(texto), Value(valor)});
}
/// entity:setSoundEffect("lowpass", true, 800)
/// lowpass/highpass/echo/reverb/occlusion
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
Value setSoundEffect(const T0& texto = {}, const T1& activar = {}, const T2& valor = {}) const {
    return call("setSoundEffect", Values{Value(texto), Value(activar), Value(valor)});
}
/// entity:setVehicleInput(acelerador, direccion, freno, freno de mano)
/// conduce su Vehicle
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
Value setVehicleInput(const T0& acelerador = {}, const T1& direccion = {}, const T2& freno = {}, const T3& frenoDeMano = {}) const {
    return call("setVehicleInput", Values{Value(acelerador), Value(direccion), Value(freno), Value(frenoDeMano)});
}
/// entity:setupCreatureIK()
/// configura el IK solo (patas, cuello, cola)
template <typename... Mas>
Value setupCreatureIK(Mas&&... mas) const {
    return call("setupCreatureIK", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:setupPhysBones()
/// detecta pelo, colas, orejas...
template <typename... Mas>
Value setupPhysBones(Mas&&... mas) const {
    return call("setupPhysBones", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:setupRagdoll()
/// genera los huesos del ragdoll
template <typename... Mas>
Value setupRagdoll(Mas&&... mas) const {
    return call("setupRagdoll", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:showBones(true)
/// dibuja los huesos en la Escena
template <typename T0 = Value>
Value showBones(const T0& activar = {}) const {
    return call("showBones", Values{Value(activar)});
}
/// entity:soundOcclusion()
/// paredes que tapan su sonido ahora (-1 = no suena)
template <typename... Mas>
Value soundOcclusion(Mas&&... mas) const {
    return call("soundOcclusion", Values{Value(std::forward<Mas>(mas))...});
}
/// entity.speed: km/h de su Vehicle
Value speed() const { return get("speed"); }
void setSpeed(const Value& v) const { set("speed", v); }
/// entity:stopMoving()
/// se para
template <typename... Mas>
Value stopMoving(Mas&&... mas) const {
    return call("stopMoving", Values{Value(std::forward<Mas>(mas))...});
}
/// entity:stopSound()
/// para su sonido
template <typename... Mas>
Value stopSound(Mas&&... mas) const {
    return call("stopSound", Values{Value(std::forward<Mas>(mas))...});
}
/// entity.text: texto de su UIText o campo de texto
Value text() const { return get("text"); }
void setText(const Value& v) const { set("text", v); }
/// entity.texture: imagen de su UIImage
Value texture() const { return get("texture"); }
void setTexture(const Value& v) const { set("texture", v); }
/// entity:translateLocal(Vec3)
/// mueve en sus ejes
template <typename T0 = Value>
Value translateLocal(const T0& vec = {}) const {
    return call("translateLocal", Values{Value(vec)});
}
/// entity.uiPosition: Vec3 posicion de su RectTransform
Value uiPosition() const { return get("uiPosition"); }
void setUiPosition(const Value& v) const { set("uiPosition", v); }
/// entity.uiSize: Vec3 tamano de su RectTransform
Value uiSize() const { return get("uiSize"); }
void setUiSize(const Value& v) const { set("uiSize", v); }
/// entity.value: valor de su Slider o Casilla
Value value() const { return get("value"); }
void setValue(const Value& v) const { set("value", v); }
