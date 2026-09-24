// Codigo comun de las pasadas de IBL (ibl_prefilter.comp, ibl_sh.comp).
//
// El "entorno" que ilumina las superficies es el cielo fisico (sky_lut.frag)
// por encima del horizonte y, por debajo, el suelo: terreno de albedo medio
// iluminado por el sol (o la luna) y por el propio cielo.
//
// Con lluvia, el suelo esta mojado y encharcado: una parte es lamina de agua
// que refleja como un espejo lo que tiene encima (ver wetGround). Asi los
// objetos con reflejos (metal, cristal, lo pulido) ven en el IBL el brillo
// de la calle mojada, no un suelo gris mate.
//
// Requiere declarar antes `uniform sampler2D sky_lut`.

const float kPi = 3.14159265;
const float kGroundAlbedo = 0.18;

// Debe coincidir con skyLutUv() de lighting.frag y con sky_lut.frag.
vec2 skyLutUv(vec3 direction) {
    float azimuth = atan(direction.z, direction.x);
    float elevation = asin(clamp(direction.y, -1.0, 1.0));
    float v = 0.5 + 0.5 * sign(elevation) * sqrt(abs(elevation) / (0.5 * kPi));
    return vec2(azimuth / (2.0 * kPi) + 0.5, v);
}

vec3 skySample(vec3 direction) {
    return textureLod(sky_lut, skyLutUv(direction), 0.0).rgb;
}

// Radiancia media del hemisferio superior (cenit + anillo a 30 grados).
vec3 skyAverage() {
    vec3 sum = skySample(vec3(0.0, 1.0, 0.0)) * 0.3;
    for (int i = 0; i < 6; ++i) {
        float azimuth = float(i) * (kPi / 3.0);
        sum += skySample(vec3(cos(azimuth) * 0.866, 0.5, sin(azimuth) * 0.866)) * (0.7 / 6.0);
    }
    return sum;
}

// Radiancia del suelo: difuso de albedo kGroundAlbedo con la luz directa y la
// del cielo (las intensidades de la escena ya llevan el factor pi).
vec3 groundRadiance(vec3 light_radiance, vec3 to_light) {
    return kGroundAlbedo * (light_radiance * max(to_light.y, 0.0) + skyAverage());
}

// Fresnel de Schlick del agua (F0 = 0.02, IOR 1.33). Igual que en
// rain_common.glsl.
float iblWaterFresnel(float cosine) {
    float m = 1.0 - clamp(cosine, 0.0, 1.0);
    float m2 = m * m;
    return 0.02 + 0.98 * m2 * m2 * m;
}

// Suelo lejano visto en la direccion `direction` (hacia abajo). `wet` es la
// fraccion cubierta de agua: refleja `mirrored` (lo que hay en la direccion
// espejo, hacia arriba) con el Fresnel del agua, que en angulo rasante llega
// casi a 1: por eso una calle mojada brilla hacia el horizonte. Lo que no
// refleja entra y llega al suelo, oscurecido por estar empapado (Lagarde).
vec3 wetGround(vec3 direction, vec3 ground, vec3 mirrored, float wet) {
    if (wet <= 0.0) {
        return ground;
    }
    float reflected = iblWaterFresnel(-direction.y) * wet;
    return mix(ground * mix(1.0, 0.6, wet), mirrored, reflected);
}

vec3 environment(vec3 direction, vec3 ground, float wet) {
    if (direction.y >= 0.0) {
        return skySample(direction);
    }
    vec3 horizon = skySample(normalize(vec3(direction.x, 0.0, direction.z)));
    vec3 mirrored = skySample(vec3(direction.x, -direction.y, direction.z));
    return mix(horizon, wetGround(direction, ground, mirrored, wet),
               smoothstep(0.0, 0.15, -direction.y));
}
