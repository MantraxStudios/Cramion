// Modelo de material de Disney (Burley 2012, "Physically Based Shading at
// Disney"; y 2015 para la transmision, que va en glass.frag). Lo usan
// lighting.frag (el raster) y path_trace_common.glsl (el path tracing), asi
// la referencia ve lo mismo. Antes de incluirlo: kPi.
//
// Todos los materiales llevan el difuso de Burley (retro-reflexion en lo
// rugoso) y el especular GGX. Cada material elige ademas UN modelo de
// sombreado, con hasta 3 parametros (0..1) guardados en el G-buffer
// (RGBA8: r = modelo, gba = parametros):
//
//   0 Estandar      g = tinte especular
//   1 Barniz        g = barniz, b = rugosidad del barniz, a = tinte especular
//   2 Tela          g = sheen, b = tinte del sheen, a = tinte especular
//   3 Subsurface    g = subsurface (Hanrahan-Krueger), b = translucidez,
//                   a = grosor (para la sombra de la cara de atras)
//   4 Anisotropo    g = anisotropia, b = direccion (0..1 = 0..180 grados
//                   respecto a la base de la normal), a = tinte especular
//   6 Pelo          g = desplazamiento de los brillos, b = direccion de la
//                   hebra (como el anisotropo), a = brillo secundario
//                   (Kajiya-Kay con los dos lobulos desplazados de
//                   Scheuermann, "Hair Rendering and Shading", GDC 2004)
//
// Las funciones devuelven "BRDF x pi x N.L" (la escala del motor: las luces
// ya llevan pi, el difuso de Lambert es albedo x N.L).

const int kShadingStandard = 0;
const int kShadingClearcoat = 1;
const int kShadingCloth = 2;
const int kShadingSubsurface = 3;
const int kShadingAnisotropic = 4;
// 5 = Transmision (vidrio: IOR, grosor, rugosidad): solo en glass.frag.
const int kShadingTransmission = 5;
const int kShadingHair = 6;

struct ShadingModel {
    int model;
    vec3 params;     // los tres parametros del modelo (0..1)
    vec3 tangent;    // anisotropo: direccion en la que se estira el brillo
    vec3 bitangent;
};

// Base ortonormal a partir de la normal sola (Duff et al. 2017): el G-buffer
// la reconstruye igual al leer la normal, asi la direccion del anisotropo se
// guarda como un angulo.
void shadingBasis(vec3 n, out vec3 b1, out vec3 b2) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float b = n.x * n.y * a;
    b1 = vec3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    b2 = vec3(b, s + n.y * n.y * a, -n.y);
}

// Angulo (0..1 = 0..180 grados) de una tangente respecto a la base de la normal.
float shadingEncodeAngle(vec3 n, vec3 t) {
    vec3 b1;
    vec3 b2;
    shadingBasis(n, b1, b2);
    float angle = atan(dot(t, b2), dot(t, b1));
    if (angle < 0.0) angle += kPi;  // el brillo es simetrico: 180 grados bastan
    return clamp(angle / kPi, 0.0, 1.0);
}

ShadingModel decodeShading(vec4 packed, vec3 n) {
    ShadingModel s;
    s.model = int(packed.r * 255.0 + 0.5);
    s.params = packed.gba;
    s.tangent = vec3(0.0);
    s.bitangent = vec3(0.0);
    if (s.model == kShadingAnisotropic || s.model == kShadingHair) {
        vec3 b1;
        vec3 b2;
        shadingBasis(n, b1, b2);
        float angle = s.params.y * kPi;
        s.tangent = normalize(cos(angle) * b1 + sin(angle) * b2);
        s.bitangent = cross(n, s.tangent);
    }
    return s;
}

ShadingModel standardShading() {
    ShadingModel s;
    s.model = kShadingStandard;
    s.params = vec3(0.0);
    s.tangent = vec3(0.0);
    s.bitangent = vec3(0.0);
    return s;
}

float schlickWeight(float cosine) {
    float m = clamp(1.0 - cosine, 0.0, 1.0);
    float m2 = m * m;
    return m2 * m2 * m;
}

vec3 disneyTint(vec3 albedo) {
    float l = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
    return l > 0.0 ? albedo / l : vec3(1.0);
}

// Tinte especular de Disney: la reflectancia de lo no metalico toma el tono
// del color base (latón barnizado, plasticos de color).
float shadingSpecularTint(ShadingModel s) {
    if (s.model == kShadingStandard) return s.params.x;
    if (s.model == kShadingSubsurface || s.model == kShadingHair) return 0.0;
    return s.params.z;
}

vec3 disneyF0(ShadingModel s, vec3 albedo, float reflectance, float metallic) {
    vec3 dielectric = reflectance * mix(vec3(1.0), disneyTint(albedo), shadingSpecularTint(s));
    return mix(dielectric, albedo, metallic);
}

float disneyGgx(float n_dot_h, float alpha) {
    float a2 = alpha * alpha;
    float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

// Smith-GGX con correlacion de altura, ya dividida por 4 N.L N.V.
float disneySmith(float n_dot_v, float n_dot_l, float alpha) {
    float a2 = alpha * alpha;
    float ggx_v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    float ggx_l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    return 0.5 / max(ggx_v + ggx_l, 1e-5);
}

// GGX anisotropo (Burley 2012) y su visibilidad (Heitz 2014).
float disneyGgxAniso(float n_dot_h, float t_dot_h, float b_dot_h, float ax, float ay) {
    float x = t_dot_h / ax;
    float y = b_dot_h / ay;
    float d = x * x + y * y + n_dot_h * n_dot_h;
    return 1.0 / (kPi * ax * ay * d * d);
}

float disneySmithAniso(float n_dot_v, float n_dot_l, float t_dot_v, float b_dot_v, float t_dot_l, float b_dot_l,
                       float ax, float ay) {
    float lambda_v = n_dot_l * length(vec3(ax * t_dot_v, ay * b_dot_v, n_dot_v));
    float lambda_l = n_dot_v * length(vec3(ax * t_dot_l, ay * b_dot_l, n_dot_l));
    return 0.5 / max(lambda_v + lambda_l, 1e-5);
}

// Anisotropia de Disney: cuanto se estira el lobulo en cada eje.
void disneyAnisoAlphas(float alpha, float anisotropy, out float ax, out float ay) {
    float aspect = sqrt(1.0 - 0.9 * clamp(anisotropy, 0.0, 1.0));
    ax = max(alpha / aspect, 0.002);
    ay = max(alpha * aspect, 0.002);
}

// Una luz. `l`: hacia la luz, `v`: hacia la camara. `f0` ya con el tinte
// especular (disneyF0). `energy`: compensacion de dispersion multiple del
// especular. `light_size`: tangente del radio angular de la luz (el sol:
// 0.0047); ensancha el lobulo sin puntos blancos (Karis 2013).
// Pelo: brillo de Kajiya-Kay alrededor de la hebra `t` (la tangente
// desplazada hacia la normal: cada lobulo cae en otro sitio).
float hairStrandSpecular(vec3 t, vec3 h, float exponent) {
    float t_dot_h = dot(t, h);
    float sin_th = sqrt(max(1.0 - t_dot_h * t_dot_h, 0.0));
    float dir_atten = smoothstep(-1.0, 0.0, t_dot_h);
    return dir_atten * pow(sin_th, exponent);
}

vec3 hairBrdf(ShadingModel s, vec3 n, vec3 v, vec3 l, vec3 albedo, float roughness, vec3 energy) {
    float n_dot_l = dot(n, l);
    // Difuso envolvente: el pelo deja pasar la luz (sin terminador duro).
    float wrap = clamp(n_dot_l * 0.75 + 0.25, 0.0, 1.0);
    if (wrap <= 0.0) return vec3(0.0);
    vec3 h = normalize(l + v);
    // La hebra va en la bitangente (como el brillo estirado del anisotropo).
    vec3 strand = s.bitangent;
    float shift = (s.params.x - 0.5) * 0.6;
    vec3 t1 = normalize(strand + n * (shift + 0.1));
    vec3 t2 = normalize(strand + n * (shift - 0.15));
    float alpha = max(roughness * roughness, 0.01);
    float e1 = clamp(2.0 / (alpha * alpha) - 2.0, 4.0, 1024.0);
    float e2 = e1 * 0.25;
    float spec1 = hairStrandSpecular(t1, h, e1) * (e1 + 2.0) / (8.0 * kPi);
    float spec2 = hairStrandSpecular(t2, h, e2) * (e2 + 2.0) / (8.0 * kPi) * s.params.z;
    vec3 specular = (vec3(0.04 * spec1 * 4.0) + albedo * spec2) * kPi * energy;
    vec3 diffuse = albedo * 0.85;
    return (diffuse + specular * smoothstep(0.0, 0.2, n_dot_l + 0.1)) * wrap;
}

vec3 disneyBrdf(ShadingModel s, vec3 n, vec3 v, vec3 l, vec3 albedo, float roughness, float metallic, vec3 f0,
                vec3 energy, float light_size) {
    if (s.model == kShadingHair) return hairBrdf(s, n, v, l, albedo, roughness, energy);
    float n_dot_l = dot(n, l);
    if (n_dot_l <= 0.0) return vec3(0.0);
    float n_dot_v = max(dot(n, v), 1e-4);
    vec3 h = normalize(l + v);
    float n_dot_h = max(dot(n, h), 0.0);
    float l_dot_h = max(dot(l, h), 0.0);

    // --- Difuso de Burley (con el subsurface de Hanrahan-Krueger) ---
    float fl = schlickWeight(n_dot_l);
    float fv = schlickWeight(n_dot_v);
    float fd90 = 0.5 + 2.0 * l_dot_h * l_dot_h * roughness;
    float fd = mix(1.0, fd90, fl) * mix(1.0, fd90, fv);
    if (s.model == kShadingSubsurface) {
        float fss90 = l_dot_h * l_dot_h * roughness;
        float fss = mix(1.0, fss90, fl) * mix(1.0, fss90, fv);
        float ss = 1.25 * (fss * (1.0 / (n_dot_l + n_dot_v) - 0.5) + 0.5);
        fd = mix(fd, ss, s.params.x);
    }
    vec3 diffuse = albedo * fd * (1.0 - metallic);
    // Sheen (tela): brillo suave de los bordes, con el tono del color base.
    // En el modelo de Disney el sheen no lleva 1/pi (el difuso si): en la
    // escala del motor (BRDF x pi) va por pi.
    if (s.model == kShadingCloth) {
        vec3 sheen_color = mix(vec3(1.0), disneyTint(albedo), s.params.y);
        diffuse += s.params.x * sheen_color * schlickWeight(l_dot_h) * (1.0 - metallic) * kPi;
    }

    // --- Especular ---
    float alpha = max(roughness * roughness, 0.002);
    float alpha_light = min(alpha + light_size * 0.5, 1.0);
    float size_normalization = (alpha / alpha_light) * (alpha / alpha_light);
    vec3 fresnel = f0 + (1.0 - f0) * schlickWeight(max(dot(v, h), 0.0));
    float d_v;
    if (s.model == kShadingAnisotropic) {
        float ax;
        float ay;
        disneyAnisoAlphas(alpha_light, s.params.x, ax, ay);
        float vx;
        float vy;
        disneyAnisoAlphas(alpha, s.params.x, vx, vy);
        vec3 t = s.tangent;
        vec3 b = s.bitangent;
        d_v = disneyGgxAniso(n_dot_h, dot(t, h), dot(b, h), ax, ay) * size_normalization *
              disneySmithAniso(n_dot_v, n_dot_l, dot(t, v), dot(b, v), dot(t, l), dot(b, l), vx, vy);
    } else {
        d_v = disneyGgx(n_dot_h, alpha_light) * size_normalization * disneySmith(n_dot_v, n_dot_l, alpha);
    }
    vec3 specular = d_v * fresnel * kPi * energy;
    vec3 result = diffuse + specular;

    // --- Barniz: una segunda capa especular, dielectrica (IOR 1.5) ---
    if (s.model == kShadingClearcoat && s.params.x > 0.0) {
        float coat_alpha = max(s.params.y * s.params.y, 0.002);
        float coat_alpha_light = min(coat_alpha + light_size * 0.5, 1.0);
        float coat_normalization = (coat_alpha / coat_alpha_light) * (coat_alpha / coat_alpha_light);
        float coat_fresnel = (0.04 + 0.96 * schlickWeight(l_dot_h)) * s.params.x;
        // Visibilidad de Kelemen (la del barniz de Filament): barata y buena.
        float coat_v = 0.25 / max(l_dot_h * l_dot_h, 1e-4);
        float coat = disneyGgx(n_dot_h, coat_alpha_light) * coat_normalization * coat_v * coat_fresnel * kPi;
        // Lo que refleja el barniz no llega a la capa de debajo.
        result = result * (1.0 - coat_fresnel) + vec3(coat);
    }
    return result * n_dot_l;
}

// Translucidez (subsurface): la luz que entra por la cara de atras y sale
// hacia la camara (hojas a contraluz, orejas, cera). Se suma aunque N.L sea
// negativo; la sombra que se le aplica es la de la cara de atras (ver
// lighting.frag). Aproximacion de Barre-Brisebois y Bouchard (2011).
// Color de la luz que atraviesa: mas claro y saturado que el que se refleja
// (recorre el interior: una oreja a contraluz sale roja). En lo fino (hojas,
// params.z bajo) tira al verde amarillento de una hoja con el sol detras; con
// el color reflejado (verde oscuro) las copas a contraluz salian negras.
vec3 translucencyColor(ShadingModel s, vec3 albedo) {
    vec3 transmitted = pow(max(albedo, vec3(0.0)), vec3(0.6));
    float thin = 1.0 - smoothstep(0.1, 0.3, s.params.z);
    return transmitted * mix(vec3(1.0), vec3(1.08, 1.12, 0.62), thin);
}

vec3 disneyTranslucency(ShadingModel s, vec3 n, vec3 v, vec3 l, vec3 albedo, float metallic) {
    if (s.model != kShadingSubsurface || s.params.y <= 0.0) return vec3(0.0);
    float thin = 1.0 - smoothstep(0.1, 0.3, s.params.z);
    // Transmision difusa: lo que entra por la cara de atras sale repartido
    // hacia todos lados (en una hoja fina es lo principal; en algo grueso,
    // poco). Antes solo brillaba mirando justo al sol a traves de ella.
    float diffuse = clamp(dot(-n, l), 0.0, 1.0) * mix(0.3, 0.85, thin);
    // Mas el lobulo hacia delante: mirando a la luz a traves de ella.
    vec3 through = normalize(l + n * 0.35);
    float forward = pow(clamp(dot(v, -through), 0.0, 1.0), 6.0) * 2.0;
    // Y un poco de luz que da la vuelta al objeto (envoltura).
    float wrap = clamp((dot(-n, l) + 0.2) / 1.2, 0.0, 1.0) * 0.15;
    return translucencyColor(s, albedo) * s.params.y * (diffuse + forward + wrap) * (1.0 - metallic);
}
