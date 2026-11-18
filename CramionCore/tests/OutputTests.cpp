// Pruebas de la salida de imagen (consola, sin GPU): el lector de LUT .cube,
// la eleccion del formato y el espacio de color de la swapchain (SDR, HDR10,
// scRGB y la vuelta a SDR), las curvas PQ, la eleccion de la baja latencia
// (Reflex, Anti-Lag 2, present_wait), la medida de la latencia y la LUT en el
// componente PostProcessing (guardar, leer y mezclar volumenes). Devuelve 0
// si todo va.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/vk/CubeLut.h>
#include <CramionFX/vk/DisplayOutputVk.h>
#include <CramionFX/vk/GpuTypes.h>
#include <CramionFX/vk/GraphicsSettings.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

using namespace cramion;
using gfx::HdrOutput;
using gfx::LatencyBackend;
using gfx::LowLatencyMode;
using gfx::OutputEncoding;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what, const std::string& detail = {}) {
    ++checks;
    std::printf("  %s %s%s%s\n", condition ? "OK   " : "FALLO", what, condition || detail.empty() ? "" : "  -> ",
                condition ? "" : detail.c_str());
    if (!condition) ++failures;
}

bool approx(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool approxV(const core::Vec3& a, const core::Vec3& b, float eps = 1e-3f) {
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) && approx(a.z, b.z, eps);
}

std::string vec(const core::Vec3& v) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "(%.4f, %.4f, %.4f)", v.x, v.y, v.z);
    return buffer;
}

void testCubeParser() {
    std::printf("LUT .cube\n");
    // Cubo 2x2x2 que invierte los colores (rojo el que cambia mas deprisa).
    const char* invert = R"(# Resolve
TITLE "Invertir"
LUT_3D_SIZE 2
DOMAIN_MIN 0 0 0
DOMAIN_MAX 1 1 1
1 1 1
0 1 1
1 0 1
0 0 1
1 1 0
0 1 0
1 0 0
0 0 0
)";
    gfx::CubeLut lut;
    std::string error;
    const bool ok = gfx::parseCubeLut(invert, lut, error);
    check(ok && lut.valid() && lut.size == 2 && lut.title == "Invertir", "lee un cubo 2^3 con titulo y dominio", error);
    check(approxV(lut.at(1, 0, 0), {0, 1, 1}) && approxV(lut.at(0, 1, 0), {1, 0, 1}) && approxV(lut.at(0, 0, 1), {1, 1, 0}),
          "el rojo es el indice que cambia mas deprisa (orden de Adobe)");
    const core::Vec3 mid = gfx::sampleCubeLut(lut, {0.25f, 0.5f, 0.75f});
    check(approxV(mid, {0.75f, 0.5f, 0.25f}), "interpolacion trilineal (invertir 0.25/0.5/0.75)", vec(mid));

    // Identidad de 33 (la que exporta Resolve) por texto, con CRLF y BOM.
    std::string text = "\xEF\xBB\xBF" "LUT_3D_SIZE 5\r\n";
    for (int b = 0; b < 5; ++b)
        for (int g = 0; g < 5; ++g)
            for (int r = 0; r < 5; ++r) {
                char line[64];
                std::snprintf(line, sizeof(line), "%.6f %.6f %.6f\r\n", r / 4.0, g / 4.0, b / 4.0);
                text += line;
            }
    gfx::CubeLut identity;
    check(gfx::parseCubeLut(text, identity, error) && identity.size == 5, "BOM, CRLF y LUT_3D_SIZE 5", error);
    const core::Vec3 c{0.13f, 0.62f, 0.91f};
    check(approxV(gfx::sampleCubeLut(identity, c), c), "la identidad deja el color igual", vec(gfx::sampleCubeLut(identity, c)));
    check(approxV(gfx::sampleCubeLut(identity, {-1.0f, 2.0f, 0.5f}), {0.0f, 1.0f, 0.5f}), "fuera del dominio se recorta");
    const gfx::CubeLut made = gfx::makeIdentityCubeLut(17);
    check(made.valid() && approxV(gfx::sampleCubeLut(made, c), c), "makeIdentityCubeLut(17)");

    // Dominio distinto de 0..1 (LUT para log): DOMAIN_MAX 2.
    const char* domain = "LUT_3D_SIZE 2\nDOMAIN_MAX 2 2 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    gfx::CubeLut wide;
    check(gfx::parseCubeLut(domain, wide, error) && approxV(gfx::sampleCubeLut(wide, {1.0f, 1.0f, 1.0f}), {0.5f, 0.5f, 0.5f}),
          "DOMAIN_MAX 2: el 1 cae en la mitad del cubo", error);
    const char* range = "LUT_3D_INPUT_RANGE 0 4\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    gfx::CubeLut ranged;
    check(gfx::parseCubeLut(range, ranged, error) && approx(ranged.domain_max.x, 4.0f), "LUT_3D_INPUT_RANGE (Resolve)", error);

    // 1D -> 3D: cada canal por su curva (aqui, cuadrado).
    const char* one_d = "LUT_1D_SIZE 3\n0 0 0\n0.25 0.25 0.25\n1 1 1\n";
    gfx::CubeLut curve;
    const bool curve_ok = gfx::parseCubeLut(one_d, curve, error);
    const core::Vec3 half = curve_ok ? gfx::sampleCubeLut(curve, {0.5f, 0.0f, 1.0f}) : core::Vec3{};
    check(curve_ok && curve.size == gfx::kCubeLutFrom1DSize && approxV(half, {0.25f, 0.0f, 1.0f}, 2e-3f),
          "LUT_1D_SIZE se convierte a un cubo", error + " " + vec(half));

    // Errores (con su linea).
    gfx::CubeLut untouched = lut;
    check(!gfx::parseCubeLut("LUT_3D_SIZE 2\n0 0 0\n", untouched, error) && error.find("8") != std::string::npos &&
              untouched.size == 2 && untouched.title == "Invertir",
          "faltan valores: error y la LUT de antes sin tocar", error);
    check(!gfx::parseCubeLut("0 0 0\n", untouched, error) && error.find("linea 1") != std::string::npos,
          "datos antes del tamano", error);
    check(!gfx::parseCubeLut("LUT_3D_SIZE 2\n0 0 x\n", untouched, error) && error.find("linea 2") != std::string::npos,
          "numero no valido con su linea", error);
    check(!gfx::parseCubeLut("LUT_3D_SIZE 1\n", untouched, error), "LUT_3D_SIZE 1 no vale", error);
    check(!gfx::parseCubeLut("LUT_3D_SIZE 300\n", untouched, error), "LUT_3D_SIZE 300 no vale (maximo 256)", error);
    check(!gfx::parseCubeLut("TITLE \"vacia\"\n", untouched, error), "sin tamano", error);
    check(!gfx::parseCubeLut("LUT_3D_SIZE 2\nDOMAIN_MIN 1 1 1\nDOMAIN_MAX 1 1 1\n", untouched, error),
          "dominio vacio", error);
    check(gfx::parseCubeLut("LUT_3D_SIZE 2\nLUT_IN_VIDEO_RANGE\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n",
                            untouched, error),
          "palabras clave desconocidas se ignoran", error);

    // A half float para la textura 3D.
    const std::vector<std::uint16_t> half_rgba = gfx::cubeLutToHalfRgba(lut);
    check(half_rgba.size() == 8 * 4 && half_rgba[0] == 0x3C00 && half_rgba[3] == 0x3C00 && half_rgba[7 * 4] == 0,
          "texels RGBA en half float (1.0 = 0x3C00)");

    // Desde un archivo.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "cramion_output_tests";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "invertir.cube", std::ios::binary);
        out << invert;
    }
    gfx::CubeLut loaded;
    check(gfx::loadCubeLut(dir / "invertir.cube", loaded, error) && loaded.size == 2, "loadCubeLut desde un archivo", error);
    check(!gfx::loadCubeLut(dir / "no_existe.cube", loaded, error), "archivo que no existe", error);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testFormatChoice() {
    std::printf("Formato y espacio de color de la swapchain\n");
    using F = vk::Format;
    using C = vk::ColorSpaceKHR;
    const std::vector<vk::SurfaceFormatKHR> sdr_only = {{F::eB8G8R8A8Srgb, C::eSrgbNonlinear},
                                                        {F::eB8G8R8A8Unorm, C::eSrgbNonlinear}};
    const std::vector<vk::SurfaceFormatKHR> hdr_screen = {
        {F::eB8G8R8A8Unorm, C::eSrgbNonlinear},
        {F::eA2B10G10R10UnormPack32, C::eSrgbNonlinear},
        {F::eA2B10G10R10UnormPack32, C::eHdr10St2084EXT},
        {F::eR16G16B16A16Sfloat, C::eExtendedSrgbLinearEXT}};
    const std::vector<vk::SurfaceFormatKHR> scrgb_only = {{F::eR8G8B8A8Unorm, C::eSrgbNonlinear},
                                                          {F::eR16G16B16A16Sfloat, C::eExtendedSrgbLinearEXT}};

    gfx::SurfaceFormatChoice c = gfx::chooseOutputFormat(sdr_only, HdrOutput::Off, true);
    check(c.surface.format == F::eB8G8R8A8Unorm && c.encoding == OutputEncoding::Sdr && c.ui_format == F::eB8G8R8A8Unorm &&
              !c.hdr_fell_back,
          "SDR: el UNORM de siempre (no el _SRGB)");
    c = gfx::chooseOutputFormat(hdr_screen, HdrOutput::Off, true);
    check(c.encoding == OutputEncoding::Sdr && c.surface.format == F::eB8G8R8A8Unorm &&
              c.surface.colorSpace == C::eSrgbNonlinear,
          "apagado: SDR aunque la pantalla de HDR (como antes)");
    c = gfx::chooseOutputFormat(hdr_screen, HdrOutput::Auto, true);
    check(c.encoding == OutputEncoding::Hdr10Pq && c.surface.format == F::eA2B10G10R10UnormPack32 &&
              c.surface.colorSpace == C::eHdr10St2084EXT && c.ui_format == F::eB8G8R8A8Unorm,
          "automatico: HDR10 (PQ) con la UI en el UNORM de 8 bits");
    c = gfx::chooseOutputFormat(hdr_screen, HdrOutput::ScRgb, true);
    check(c.encoding == OutputEncoding::ScRgbLinear && c.surface.format == F::eR16G16B16A16Sfloat &&
              c.surface.colorSpace == C::eExtendedSrgbLinearEXT,
          "scRGB pedido: RGBA16F lineal");
    c = gfx::chooseOutputFormat(scrgb_only, HdrOutput::Auto, true);
    check(c.encoding == OutputEncoding::ScRgbLinear && c.ui_format == F::eR8G8B8A8Unorm,
          "automatico sin HDR10: scRGB (UI en el RGBA8 de la pantalla)");
    c = gfx::chooseOutputFormat(scrgb_only, HdrOutput::Hdr10, true);
    check(c.encoding == OutputEncoding::Sdr && c.hdr_fell_back && c.surface.format == F::eR8G8B8A8Unorm,
          "HDR10 pedido sin HDR10: vuelve a SDR y lo dice");
    c = gfx::chooseOutputFormat(hdr_screen, HdrOutput::Auto, false);
    check(c.encoding == OutputEncoding::Sdr && c.hdr_fell_back, "sin VK_EXT_swapchain_colorspace: SDR");
    c = gfx::chooseOutputFormat(sdr_only, HdrOutput::Auto, true);
    check(c.encoding == OutputEncoding::Sdr && c.hdr_fell_back && c.surface.format == F::eB8G8R8A8Unorm,
          "pantalla SDR: SDR");
    bool threw = false;
    try {
        (void)gfx::chooseOutputFormat({}, HdrOutput::Off, true);
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "sin formatos: error (como antes)");
    // El mismo formato de UI con y sin HDR: los pipelines de ImGui no cambian.
    const gfx::SurfaceFormatChoice off = gfx::chooseOutputFormat(hdr_screen, HdrOutput::Off, true);
    const gfx::SurfaceFormatChoice on = gfx::chooseOutputFormat(hdr_screen, HdrOutput::Auto, true);
    check(off.ui_format == on.ui_format, "el formato de la UI no cambia al encender el HDR");

    std::printf("Curvas HDR\n");
    check(approx(gfx::pqEncode(0.0f), 0.0f, 1e-4f) && approx(gfx::pqEncode(10000.0f), 1.0f, 1e-4f), "PQ: 0 y 10000 nits");
    check(approx(gfx::pqEncode(100.0f), 0.5081f, 2e-3f), "PQ: 100 nits = 0.508 (ST 2084)",
          std::to_string(gfx::pqEncode(100.0f)));
    check(approx(gfx::pqEncode(1000.0f), 0.7518f, 2e-3f), "PQ: 1000 nits = 0.752", std::to_string(gfx::pqEncode(1000.0f)));
    bool round_trip = true;
    for (float nits : {0.5f, 80.0f, 203.0f, 600.0f, 4000.0f}) {
        round_trip = round_trip && std::fabs(gfx::pqDecode(gfx::pqEncode(nits)) - nits) <= nits * 2e-3f;
    }
    check(round_trip, "PQ ida y vuelta");
    check(approx(gfx::clampPaperWhite(20.0f), 80.0f) && approx(gfx::clampPaperWhite(900.0f), 500.0f) &&
              approx(gfx::clampPaperWhite(NAN), 200.0f),
          "brillo de papel entre 80 y 500 nits");
    check(approx(gfx::clampMaxNits(100.0f, 300.0f), 300.0f) && approx(gfx::clampMaxNits(20000.0f, 200.0f), 10000.0f),
          "nits maximos entre el papel y 10000");
    const gfx::GraphicsSettings defaults{};
    check(defaults.hdr_output == HdrOutput::Off && defaults.low_latency == LowLatencyMode::Off,
          "por defecto: SDR y sin baja latencia (como siempre)");
}

void testLatency() {
    std::printf("Baja latencia\n");
    gfx::OutputExtensions none{};
    gfx::OutputExtensions wait{};
    wait.present_id = true;
    wait.present_wait = true;
    gfx::OutputExtensions nvidia = wait;
    nvidia.nv_low_latency2 = true;
    gfx::OutputExtensions amd = wait;
    amd.amd_anti_lag = true;
    gfx::OutputExtensions nv_without_id{};
    nv_without_id.nv_low_latency2 = true;

    check(gfx::chooseLatencyBackend(LowLatencyMode::Off, nvidia) == LatencyBackend::None, "apagada: nada");
    check(gfx::chooseLatencyBackend(LowLatencyMode::On, none) == LatencyBackend::None,
          "sin extensiones: nada (igual que ahora)");
    check(gfx::chooseLatencyBackend(LowLatencyMode::On, wait) == LatencyBackend::PresentWait, "present_wait");
    check(gfx::chooseLatencyBackend(LowLatencyMode::Boost, nvidia) == LatencyBackend::Reflex, "NVIDIA: Reflex");
    check(gfx::chooseLatencyBackend(LowLatencyMode::On, amd) == LatencyBackend::AntiLag, "AMD: Anti-Lag 2");
    check(gfx::chooseLatencyBackend(LowLatencyMode::On, nv_without_id) == LatencyBackend::None,
          "Reflex sin present_id: no (las marcas lo necesitan)");

    check(gfx::presentWaitTarget(LowLatencyMode::Off, 10) == 0, "apagada: no espera");
    check(gfx::presentWaitTarget(LowLatencyMode::On, 10) == 8, "activada: uno en cola (espera al 8 antes del 10)");
    check(gfx::presentWaitTarget(LowLatencyMode::Boost, 10) == 9, "boost: espera al anterior");
    check(gfx::presentWaitTarget(LowLatencyMode::On, 2) == 0 && gfx::presentWaitTarget(LowLatencyMode::Boost, 1) == 0,
          "los primeros frames no esperan");

    gfx::LatencyMeter meter;
    const auto t0 = gfx::LatencyMeter::Clock::now();
    meter.frameStarted(1, t0);
    meter.frameStarted(2, t0 + std::chrono::milliseconds(10));
    check(meter.frameFinished(1, t0 + std::chrono::milliseconds(30)) && approx(meter.lastMs(), 30.0f, 0.5f),
          "latencia de un frame (30 ms)");
    check(meter.frameFinished(2, t0 + std::chrono::milliseconds(30)) && approx(meter.averageMs(), 25.0f, 0.5f),
          "media de dos (30 y 20 ms)");
    check(!meter.frameFinished(2, t0 + std::chrono::milliseconds(40)), "un frame no cuenta dos veces");
    check(!meter.frameFinished(99, t0), "un id sin comienzo no cuenta");
    meter.addSample(5.0f);
    check(meter.samples() == 3 && approx(meter.averageMs(), 55.0f / 3.0f, 0.5f), "muestras de Reflex");
    for (int i = 0; i < 200; ++i) meter.addSample(10.0f);
    check(meter.samples() == gfx::LatencyMeter::kWindow && approx(meter.averageMs(), 10.0f, 1e-3f),
          "ventana de las ultimas 64 muestras");
    meter.reset();
    check(meter.samples() == 0 && meter.averageMs() == 0.0f, "reinicio");
    check(std::string(gfx::latencyBackendName(LatencyBackend::Reflex)) == "Reflex" &&
              std::string(gfx::lowLatencyModeName(LowLatencyMode::Boost)) == "activado+boost",
          "nombres para el log");
}

void testComponent() {
    std::printf("LUT en el componente PostProcessing\n");
    // Ajustes del composite: los dos vec4 nuevos van al final (std140).
    check(sizeof(gfx::GpuCompositeSettings) == 15 * 16, "GpuCompositeSettings = 15 vec4 (13 de antes + LUT)");
    check(sizeof(gfx::GpuHdrOutputPush) == 32 && sizeof(gfx::GpuHdrOutputPush) <= 128,
          "push de la salida HDR: 32 bytes (dentro de 128)");

    ecs::World world;
    ecs::Entity e = world.create("Volumen");
    auto& post = e.add<ecs::PostProcessing>();
    post.settings.color_lut = "LUTs/Calido.cube";
    post.settings.color_lut_intensity = 0.6f;
    const std::string text = ecs::serializeWorld(world);
    ecs::World loaded;
    const bool ok = ecs::deserializeWorld(loaded, text);
    const ecs::Entity again = loaded.findByName("Volumen");
    const ecs::PostProcessing* read = ok && again.valid() ? again.tryGet<ecs::PostProcessing>() : nullptr;
    check(read != nullptr && read->settings.color_lut == "LUTs/Calido.cube" && approx(read->settings.color_lut_intensity, 0.6f),
          "se guarda y se lee la ruta y la intensidad");

    // Una escena de antes (sin las claves): sin LUT.
    nlohmann::json old = nlohmann::json::parse(text);
    const std::function<void(nlohmann::json&)> strip = [&](nlohmann::json& j) {
        if (j.is_object()) {
            j.erase("color_lut");
            j.erase("color_lut_intensity");
            for (auto& [key, value] : j.items()) strip(value);
        } else if (j.is_array()) {
            for (auto& value : j) strip(value);
        }
    };
    strip(old);
    const std::string old_text = old.dump();
    ecs::World legacy;
    const bool legacy_ok = old_text.find("color_lut") == std::string::npos && ecs::deserializeWorld(legacy, old_text);
    const ecs::Entity legacy_e = legacy.findByName("Volumen");
    const ecs::PostProcessing* legacy_post = legacy_ok && legacy_e.valid() ? legacy_e.tryGet<ecs::PostProcessing>() : nullptr;
    check(legacy_post != nullptr && legacy_post->settings.color_lut.empty() &&
              approx(legacy_post->settings.color_lut_intensity, 1.0f),
          "escena de antes: sin LUT");

    // Mezcla de volumenes: de nada a una LUT la intensidad sube desde 0.
    gfx::PostProcessSettings base{};
    gfx::PostProcessSettings with{};
    with.color_lut = "a.cube";
    with.color_lut_intensity = 0.8f;
    gfx::PostProcessSettings blended = base;
    ecs::blendPostProcess(blended, with, 0.25f, ecs::kPostAll);
    check(blended.color_lut == "a.cube" && approx(blended.color_lut_intensity, 0.2f),
          "sin LUT -> LUT: la ruta ya y la intensidad sube (0.25 -> 0.2)");
    blended = with;
    ecs::blendPostProcess(blended, base, 0.75f, ecs::kPostAll);
    check(blended.color_lut == "a.cube" && approx(blended.color_lut_intensity, 0.2f),
          "LUT -> sin LUT: se queda la ruta y la intensidad baja");
    gfx::PostProcessSettings other_lut{};
    other_lut.color_lut = "b.cube";
    blended = with;
    ecs::blendPostProcess(blended, other_lut, 0.4f, ecs::kPostAll);
    const bool before_half = blended.color_lut == "a.cube";
    blended = with;
    ecs::blendPostProcess(blended, other_lut, 0.6f, ecs::kPostAll);
    check(before_half && blended.color_lut == "b.cube", "entre dos LUT la ruta cambia a mitad");
    blended = base;
    ecs::blendPostProcess(blended, with, 1.0f, ecs::kPostBloom);
    check(blended.color_lut.empty(), "un volumen local que no sobrescribe la gradacion no la cambia");
}

}  // namespace

int main() {
    testCubeParser();
    testFormatChoice();
    testLatency();
    testComponent();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
