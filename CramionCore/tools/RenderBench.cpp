// Banco de pruebas del render sin ventana visible (no molesta al editor):
// dibuja una escena de prueba con la camara quieta y mide
//
//   - el tiempo de GPU de cada pasada (media de los frames medidos) y el de
//     CPU por frame,
//   - el ruido temporal: con nada moviendose, cuanto cambia cada pixel de un
//     frame al siguiente (diferencia media en 8 bits, percentil 99 y % de
//     pixeles que cambian mas de 2/255). Es el "hormigueo": sin ruido, una
//     imagen quieta no cambia.
//
//   cramion_render_bench [--width W] [--height H] [--frames N] [--warmup N]
//                        [--rt 0|1] [--upscaler off|taa|fsr1] [--scale S]
//                        [--adaptive 0|1] [--preset low|medium|high|ultra]
//                        [--noise-frames K] [--model archivo] [--out carpeta]
//                        [--compat] [--no-validation] [--move] [--lite]
//                        [--demo bistro|bistro-interior|san-miguel|sibenik|sportscar]
//                        [--assets carpeta] [--clouds] [--volumetric 0|1]
//                        [--bc-test] [--orbit] [--animated archivo]
//                        [--shadow-res N] [--tier low|medium|high|ultra]
//                        [--freeze-budget]
//
// --freeze-budget deja el presupuesto adaptativo en los niveles con los que
// arranca el perfil (con --adaptive 1 --tier low: lo que ve un PC de gama
// baja al empezar), sin que suba o baje nada durante la medida.
//
// --orbit gira la camara alrededor de su objetivo durante todo el banco (como
// al jugar: las cascadas de sombra cambian de encuadre cada frame).
// --animated pone un personaje animado (p. ej. assets/Reaction.fbx) delante
// de la camara: sombras con actores animados (cache de lo estatico).
//
// Con --bc-test no dibuja nada: compara la descompresion por CPU de las
// texturas BC (la de los moviles sin BC, asset::decodeBlockCompressed) con
// la de la GPU, con bloques al azar de todos los modos.
//
// Con --demo carga un escenario de demostracion (los de cramion.exe) en vez
// de la escena de prueba: para medir el coste de cada pasada en algo real.
//
// Escena: suelo satinado, una esfera de pocos poligonos (borde de la sombra
// facetado), una esfera de metal pulido, una pared, columnas, el casco de
// glTF si esta, el sol y dos luces locales con bombilla grande (penumbra de
// las sombras por rayos). Codigo 0 si termina sin excepciones.

#include <CramionDM/Input.h>
#include <CramionDM/Window.h>
#include <CramionFX/asset/Dds.h>
#include <CramionFX/asset/ImageFile.h>
#include <CramionFX/asset/Model.h>
#include <CramionFX/scene/Scene.h>
#include <CramionFX/vk/VulkanRenderer.h>

#include "../../src/DemoScenes.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace cramion;

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Options {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    int frames = 120;
    int warmup = 90;
    int noise_frames = 16;
    int rt = -1;  // -1 = el de la GPU
    std::string upscaler = "off";
    float scale = 1.0f;
    bool adaptive = false;
    std::string preset;
    std::filesystem::path model = "assets/DamagedHelmet.glb";
    std::filesystem::path out;
    bool validation = true;
    bool compat = false;
    // La luz puntual y la camara se mueven durante las capturas (estelas del
    // filtro temporal: el ruido no se mide, se guarda el ultimo frame).
    bool move = false;
    std::string demo;
    std::filesystem::path assets;
    bool clouds = false;
    int volumetric = -1;  // -1 = lo que traiga
    int lite = -1;        // iluminacion ligera (-1 = la del presupuesto)
    bool bc_test = false;
    bool orbit = false;
    std::filesystem::path animated;
    int shadow_res = 0;  // 0 = la del perfil de hardware
    std::string tier;    // perfil forzado (CRAMION_HARDWARE_TIER)
    bool freeze_budget = false;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--width") o.width = static_cast<std::uint32_t>(std::max(64, std::atoi(next().c_str())));
        else if (a == "--height") o.height = static_cast<std::uint32_t>(std::max(64, std::atoi(next().c_str())));
        else if (a == "--frames") o.frames = std::max(1, std::atoi(next().c_str()));
        else if (a == "--warmup") o.warmup = std::max(0, std::atoi(next().c_str()));
        else if (a == "--noise-frames") o.noise_frames = std::max(0, std::atoi(next().c_str()));
        else if (a == "--rt") o.rt = std::atoi(next().c_str());
        else if (a == "--upscaler") o.upscaler = next();
        else if (a == "--scale") o.scale = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--adaptive") o.adaptive = std::atoi(next().c_str()) != 0;
        else if (a == "--preset") o.preset = next();
        else if (a == "--model") o.model = next();
        else if (a == "--out") o.out = next();
        else if (a == "--no-validation") o.validation = false;
        else if (a == "--compat") o.compat = true;
        else if (a == "--move") o.move = true;
        else if (a == "--demo") o.demo = next();
        else if (a == "--assets") o.assets = next();
        else if (a == "--clouds") o.clouds = true;
        else if (a == "--volumetric") o.volumetric = std::atoi(next().c_str());
        else if (a == "--lite") o.lite = 1;
        else if (a == "--bc-test") o.bc_test = true;
        else if (a == "--orbit") o.orbit = true;
        else if (a == "--animated") o.animated = next();
        else if (a == "--shadow-res") o.shadow_res = std::atoi(next().c_str());
        else if (a == "--tier") o.tier = next();
        else if (a == "--freeze-budget") o.freeze_budget = true;
        else std::cerr << "Opcion desconocida: " << a << "\n";
    }
    return o;
}

// --- Mallas de la escena de prueba ---
struct MeshBuilder {
    asset::ModelData model;

    std::uint32_t vertex(const core::Vec3& position, const core::Vec3& normal, const core::Vec2& uv) {
        asset::SkinnedVertex v{};
        v.position = position;
        v.normal = core::normalize(normal);
        v.uv = uv;
        // Tangente cualquiera perpendicular a la normal (sin normal map).
        const core::Vec3 helper = std::abs(v.normal.y) < 0.99f ? core::Vec3{0.0f, 1.0f, 0.0f} : core::Vec3{1.0f, 0.0f, 0.0f};
        const core::Vec3 t = core::normalize(core::cross(helper, v.normal));
        v.tangent = core::Vec4{t.x, t.y, t.z, 1.0f};
        v.joints[0] = 0;
        v.weights[0] = 1.0f;
        model.vertices.push_back(v);
        return static_cast<std::uint32_t>(model.vertices.size() - 1);
    }
    void quad(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        for (const std::uint32_t i : {a, b, c, a, c, d}) model.indices.push_back(i);
    }
    asset::ModelData finish(const std::string& name, const core::Vec4& color, float roughness, float metallic) {
        model.name = name;
        model.nodes.push_back(asset::Node{name, -1, core::Mat4::identity()});
        model.bones.push_back(asset::Bone{name, 0, core::Mat4::identity()});
        asset::MaterialData material{};
        material.name = name;
        material.base_color = color;
        material.roughness = roughness;
        material.metallic = metallic;
        model.materials.push_back(material);
        asset::SubMesh submesh{};
        submesh.index_count = static_cast<std::uint32_t>(model.indices.size());
        submesh.node = 0;
        model.submeshes.push_back(submesh);
        asset::computeSubmeshBounds(model);
        return std::move(model);
    }
};

asset::ModelData box(const core::Vec3& half, const core::Vec4& color, float roughness, float metallic,
                     const std::string& name) {
    MeshBuilder b;
    const core::Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const core::Vec3& n : normals) {
        const core::Vec3 up = std::abs(n.y) > 0.5f ? core::Vec3{0.0f, 0.0f, n.y > 0.0f ? -1.0f : 1.0f}
                                                   : core::Vec3{0.0f, 1.0f, 0.0f};
        const core::Vec3 u = core::cross(up, n);
        const core::Vec3 v = core::cross(n, u);
        const auto corner = [&](float su, float sv) {
            const core::Vec3 p = n + u * su + v * sv;
            return core::Vec3{p.x * half.x, p.y * half.y, p.z * half.z};
        };
        const std::uint32_t a0 = b.vertex(corner(-1, -1), n, {0.0f, 1.0f});
        const std::uint32_t a1 = b.vertex(corner(1, -1), n, {1.0f, 1.0f});
        const std::uint32_t a2 = b.vertex(corner(1, 1), n, {1.0f, 0.0f});
        const std::uint32_t a3 = b.vertex(corner(-1, 1), n, {0.0f, 0.0f});
        b.quad(a0, a1, a2, a3);
    }
    return b.finish(name, color, roughness, metallic);
}

asset::ModelData sphere(float radius, int segments, int rings, const core::Vec4& color, float roughness, float metallic,
                        const std::string& name) {
    MeshBuilder b;
    for (int r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rings);
        const float theta = v * kPi;
        for (int s = 0; s <= segments; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(segments);
            const float phi = u * 2.0f * kPi;
            const core::Vec3 n{std::sin(theta) * std::cos(phi), std::cos(theta), -std::sin(theta) * std::sin(phi)};
            b.vertex(n * radius, n, {u, v});
        }
    }
    const int stride = segments + 1;
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            const auto i0 = static_cast<std::uint32_t>(r * stride + s);
            const auto i1 = static_cast<std::uint32_t>((r + 1) * stride + s);
            b.quad(i0, i1, i1 + 1, i0 + 1);
        }
    }
    return b.finish(name, color, roughness, metallic);
}

struct NoiseStats {
    double mean = 0.0;     // diferencia media por pixel (0..255)
    double p99 = 0.0;
    double changed = 0.0;  // % de pixeles con diferencia > 2
};

NoiseStats noiseBetween(const std::vector<asset::ImageRgba8>& frames, asset::ImageRgba8* heatmap) {
    NoiseStats stats;
    if (frames.size() < 2) return stats;
    const std::size_t pixels = static_cast<std::size_t>(frames[0].width) * frames[0].height;
    std::vector<float> worst(pixels, 0.0f);
    std::vector<float> all;
    all.reserve(pixels);
    double sum = 0.0;
    std::size_t count = 0;
    std::size_t changed = 0;
    for (std::size_t f = 1; f < frames.size(); ++f) {
        const auto& a = frames[f - 1].pixels;
        const auto& b = frames[f].pixels;
        if (a.size() != b.size() || a.size() < pixels * 4) continue;
        for (std::size_t i = 0; i < pixels; ++i) {
            const float d = (std::abs(int(a[i * 4]) - int(b[i * 4])) + std::abs(int(a[i * 4 + 1]) - int(b[i * 4 + 1])) +
                             std::abs(int(a[i * 4 + 2]) - int(b[i * 4 + 2]))) /
                            3.0f;
            sum += d;
            ++count;
            if (d > 2.0f) ++changed;
            worst[i] = std::max(worst[i], d);
            if (f == frames.size() - 1) all.push_back(d);
        }
    }
    if (count == 0) return stats;
    stats.mean = sum / static_cast<double>(count);
    stats.changed = 100.0 * static_cast<double>(changed) / static_cast<double>(count);
    std::sort(all.begin(), all.end());
    stats.p99 = all.empty() ? 0.0 : all[static_cast<std::size_t>(0.99 * static_cast<double>(all.size() - 1))];
    if (heatmap != nullptr) {
        heatmap->width = frames[0].width;
        heatmap->height = frames[0].height;
        heatmap->pixels.assign(pixels * 4, 255);
        for (std::size_t i = 0; i < pixels; ++i) {
            const auto v = static_cast<std::uint8_t>(std::clamp(worst[i] * 12.0f, 0.0f, 255.0f));
            heatmap->pixels[i * 4] = v;
            heatmap->pixels[i * 4 + 1] = static_cast<std::uint8_t>(v / 3);
            heatmap->pixels[i * 4 + 2] = static_cast<std::uint8_t>(v / 6);
        }
    }
    return stats;
}

// --- Descompresion BC: CPU contra GPU ---
// Bloques al azar (en BC7, los 8 modos por igual) subidos tal cual, copiados
// por la GPU a RGBA8 con un blit y comparados texel a texel con
// asset::decodeBlockCompressed. Devuelve el codigo de salida (0 = coinciden).
int runBcTest(const gfx::VulkanDevice& device) {
    struct Case {
        const char* name;
        asset::TextureFormat format;
        vk::Format vk_format;
        // BC7 tiene la interpolacion exacta en la especificacion: debe
        // coincidir. En BC1-BC5 cada GPU aproxima la suya (NVIDIA: 80/256 en
        // vez de 1/3, medido); el decodificador sigue la especificacion.
        int tolerance;
    };
    const Case cases[] = {
        {"BC1", asset::TextureFormat::Bc1, vk::Format::eBc1RgbaUnormBlock, 8},
        {"BC2", asset::TextureFormat::Bc2, vk::Format::eBc2UnormBlock, 8},
        {"BC3", asset::TextureFormat::Bc3, vk::Format::eBc3UnormBlock, 8},
        {"BC4", asset::TextureFormat::Bc4, vk::Format::eBc4UnormBlock, 8},
        {"BC5", asset::TextureFormat::Bc5, vk::Format::eBc5UnormBlock, 8},
        {"BC7", asset::TextureFormat::Bc7, vk::Format::eBc7UnormBlock, 0},
    };
    constexpr std::uint32_t kSize = 256;  // 64 x 64 bloques
    constexpr std::uint32_t kBlocksPerRow = kSize / 4;
    std::uint32_t seed = 12345u;
    const auto random_byte = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<std::uint8_t>(seed >> 24);
    };
    const vk::raii::Device& dev = device.handle();
    int failed = 0;
    for (const Case& c : cases) {
        const vk::FormatProperties properties = device.physicalDevice().getFormatProperties(c.vk_format);
        if (!(properties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitSrc)) {
            std::cout << c.name << ": la GPU no copia desde este formato (sin BC o sin blit); se salta\n";
            continue;
        }
        asset::TextureData texture;
        texture.width = texture.height = kSize;
        texture.format = c.format;
        const std::uint32_t block_bytes = asset::blockBytes(c.format);
        const std::uint32_t blocks = kBlocksPerRow * kBlocksPerRow;
        texture.pixels.resize(static_cast<std::size_t>(blocks) * block_bytes);
        for (std::uint8_t& b : texture.pixels) b = random_byte();
        if (c.format == asset::TextureFormat::Bc7) {
            for (std::uint32_t k = 0; k < blocks; ++k) {
                const unsigned mode = k % 8;  // el bit mas bajo a 1 dice el modo
                std::uint8_t& first = texture.pixels[static_cast<std::size_t>(k) * 16];
                first = static_cast<std::uint8_t>((first & ~((2u << mode) - 1u)) | (1u << mode));
            }
        }

        struct Image {
            vk::raii::DeviceMemory memory{nullptr};
            vk::raii::Image image{nullptr};
        };
        const auto make_image = [&](vk::Format format, vk::ImageUsageFlags usage) {
            Image out;
            vk::ImageCreateInfo info{};
            info.imageType = vk::ImageType::e2D;
            info.format = format;
            info.extent = vk::Extent3D{kSize, kSize, 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = vk::SampleCountFlagBits::e1;
            info.tiling = vk::ImageTiling::eOptimal;
            info.usage = usage;
            info.initialLayout = vk::ImageLayout::eUndefined;
            out.image = vk::raii::Image(dev, info);
            const vk::MemoryRequirements requirements = out.image.getMemoryRequirements();
            vk::MemoryAllocateInfo allocate{};
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex =
                device.findMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
            out.memory = vk::raii::DeviceMemory(dev, allocate);
            out.image.bindMemory(*out.memory, 0);
            return out;
        };
        const Image compressed =
            make_image(c.vk_format, vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc);
        const Image rgba = make_image(vk::Format::eR8G8B8A8Unorm,
                                      vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc);

        gfx::VulkanBuffer staging;
        staging.create(device, texture.pixels.size(), vk::BufferUsageFlagBits::eTransferSrc,
                       vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        staging.write(texture.pixels.data(), texture.pixels.size());
        gfx::VulkanBuffer readback;
        readback.create(device, static_cast<vk::DeviceSize>(kSize) * kSize * 4, vk::BufferUsageFlagBits::eTransferDst,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const vk::ImageSubresourceLayers layers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        const auto transition = [&](const vk::raii::CommandBuffer& cmd, vk::Image image, vk::ImageLayout from,
                                    vk::ImageLayout to, vk::AccessFlags src, vk::AccessFlags dst) {
            vk::ImageMemoryBarrier barrier{};
            barrier.srcAccessMask = src;
            barrier.dstAccessMask = dst;
            barrier.oldLayout = from;
            barrier.newLayout = to;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = range;
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {},
                                nullptr, nullptr, barrier);
        };
        device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
            transition(cmd, *compressed.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {},
                       vk::AccessFlagBits::eTransferWrite);
            transition(cmd, *rgba.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {},
                       vk::AccessFlagBits::eTransferWrite);
            vk::BufferImageCopy upload{};
            upload.imageSubresource = layers;
            upload.imageExtent = vk::Extent3D{kSize, kSize, 1};
            cmd.copyBufferToImage(*staging.handle(), *compressed.image, vk::ImageLayout::eTransferDstOptimal, upload);
            transition(cmd, *compressed.image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                       vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead);
            vk::ImageBlit blit{};
            blit.srcSubresource = layers;
            blit.dstSubresource = layers;
            blit.srcOffsets[1] = vk::Offset3D{static_cast<std::int32_t>(kSize), static_cast<std::int32_t>(kSize), 1};
            blit.dstOffsets[1] = blit.srcOffsets[1];
            cmd.blitImage(*compressed.image, vk::ImageLayout::eTransferSrcOptimal, *rgba.image,
                          vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
            transition(cmd, *rgba.image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                       vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead);
            vk::BufferImageCopy download{};
            download.imageSubresource = layers;
            download.imageExtent = vk::Extent3D{kSize, kSize, 1};
            cmd.copyImageToBuffer(*rgba.image, vk::ImageLayout::eTransferSrcOptimal, *readback.handle(), download);
        });

        const std::vector<std::uint8_t> cpu = asset::decodeBlockCompressed(texture);
        const auto* gpu = static_cast<const std::uint8_t*>(readback.mapped());
        if (cpu.size() != static_cast<std::size_t>(kSize) * kSize * 4 || gpu == nullptr) {
            std::cout << c.name << ": FALLO (la CPU no devolvio la imagen)\n";
            ++failed;
            continue;
        }
        int max_diff = 0;
        std::size_t bad_texels = 0;
        std::array<std::size_t, 8> bad_by_mode{};
        std::size_t first_bad = cpu.size();
        for (std::size_t texel = 0; texel < cpu.size() / 4; ++texel) {
            int texel_diff = 0;
            for (int ch = 0; ch < 4; ++ch) {
                texel_diff = std::max(texel_diff, std::abs(static_cast<int>(cpu[texel * 4 + ch]) -
                                                           static_cast<int>(gpu[texel * 4 + ch])));
            }
            max_diff = std::max(max_diff, texel_diff);
            if (texel_diff > c.tolerance) {
                ++bad_texels;
                if (first_bad == cpu.size()) first_bad = texel;
                const std::size_t x = texel % kSize;
                const std::size_t y = texel / kSize;
                bad_by_mode[((y / 4) * kBlocksPerRow + x / 4) % 8]++;
            }
        }
        const bool ok = bad_texels == 0;
        std::cout << c.name << ": " << (ok ? "OK" : "FALLO") << " (diferencia maxima " << max_diff << ", texeles fuera de "
                  << c.tolerance << ": " << bad_texels << " de " << kSize * kSize << ")\n";
        if (!ok) {
            ++failed;
            const std::size_t i = first_bad * 4;
            std::cout << "    primero: texel " << first_bad % kSize << "," << first_bad / kSize << " CPU ("
                      << int(cpu[i]) << "," << int(cpu[i + 1]) << "," << int(cpu[i + 2]) << "," << int(cpu[i + 3])
                      << ") GPU (" << int(gpu[i]) << "," << int(gpu[i + 1]) << "," << int(gpu[i + 2]) << ","
                      << int(gpu[i + 3]) << ")\n";
            if (c.format == asset::TextureFormat::Bc7) {
                std::cout << "    por modo:";
                for (int m = 0; m < 8; ++m) std::cout << " " << m << "=" << bad_by_mode[static_cast<std::size_t>(m)];
                std::cout << "\n";
            }
        }
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);
    if (!options.tier.empty()) {
#if defined(_WIN32)
        _putenv_s("CRAMION_HARDWARE_TIER", options.tier.c_str());
#else
        setenv("CRAMION_HARDWARE_TIER", options.tier.c_str(), 1);
#endif
    }
    if (options.compat) {
#if defined(_WIN32)
        _putenv_s("CRAMION_VK_COMPAT", "1");
#else
        setenv("CRAMION_VK_COMPAT", "1", 1);
#endif
    }
    try {
        dm::Window window;
        if (!window.create({.title = L"Cramion render bench", .width = options.width, .height = options.height,
                            .resizable = false, .visible = false})) {
            std::cerr << "No se pudo crear la ventana\n";
            return 1;
        }
        gfx::EngineInfo info;
        info.app_name = "cramion_render_bench";
        info.enable_validation = options.validation;
#if defined(__linux__) && !defined(__ANDROID__)
        // Linux: la conexion y la ventana de Xlib (vive mas que el renderizador).
        const gfx::XlibWindow xlib{window.handle()->display, window.handle()->window};
        const gfx::NativeWindow native = &xlib;
#else
        const gfx::NativeWindow native = window.handle();
#endif
        gfx::VulkanRenderer renderer;
        renderer.initialize(info, native, window.width(), window.height());
        renderer.setEditorHelpersEnabled(false);
        if (options.bc_test) return runBcTest(renderer.device());

        gfx::GraphicsSettings graphics = renderer.graphicsSettings();
        graphics.vsync = false;
        graphics.adaptive = options.adaptive;
        if (options.shadow_res > 0) graphics.shadow_resolution = options.shadow_res;
        if (options.upscaler == "taa") graphics.upscaler = gfx::Upscaler::Taa;
        else if (options.upscaler == "fsr1") graphics.upscaler = gfx::Upscaler::Fsr1;
        else graphics.upscaler = gfx::Upscaler::Off;
        if (options.scale < 0.999f && graphics.upscaler != gfx::Upscaler::Off) {
            graphics.quality = gfx::UpscaleQuality::Custom;
            graphics.custom_scale = options.scale;
        }
        renderer.setGraphicsSettings(graphics);
        if (options.freeze_budget) renderer.setBudgetSuspended(true);
        if (options.rt >= 0) renderer.setRayTracingEnabled(options.rt != 0 && renderer.rayTracingSupported());
        // Nada que se mueva solo: sin nubes (el viento las mueve), salvo que
        // se pidan para medir su coste.
        renderer.setCloudsEnabled(options.clouds);
        if (options.lite >= 0) renderer.setLiteLightingForced(options.lite != 0);

        scene::Scene scene;
        scene.initialize();
        scene.setDayCycleEnabled(false);
        scene.setTimeOfDayHours(15.5f);

        const demo::SceneEntry* demo_entry = nullptr;
        for (const demo::SceneEntry& entry : demo::kScenes) {
            if (options.demo == entry.name) demo_entry = &entry;
        }
        const std::filesystem::path assets_root =
            !options.assets.empty() ? options.assets : gfx::shaders::directory().parent_path().parent_path().parent_path() / "build" / "assets";
        const auto add = [&](asset::ModelData model, const core::Mat4& transform) {
            const std::uint32_t index = scene.addModel(std::move(model));
            scene.spawnStatic(index, transform);
        };
        if (demo_entry != nullptr) {
            const std::uint32_t model = scene.loadModel(assets_root / demo_entry->file, /*force_static=*/true);
            for (const demo::MaterialTweak& tweak : demo_entry->tweaks) {
                scene.overrideMaterial(model, tweak.material, tweak.roughness, tweak.metallic, tweak.reflectance,
                                       tweak.albedo_scale, tweak.base_color);
            }
            scene.setDirectXNormalMaps(model, demo_entry->directx_normals);
            scene.spawnStatic(model);
            if (demo_entry->ground != nullptr) {
                scene.spawnStatic(scene.loadModel(assets_root / demo_entry->ground, /*force_static=*/true));
            }
            renderer.setWeather(demo_entry->wetness, demo_entry->puddles);
            renderer.setWater(demo_entry->water_center, demo_entry->water_radii);
            if (demo_entry->environment != nullptr && renderer.loadEnvironment(assets_root / demo_entry->environment)) {
                scene.setFixedSun(renderer.environmentSunDirection());
            }
        } else {
        add(box({12.0f, 0.05f, 12.0f}, {0.55f, 0.55f, 0.52f, 1.0f}, 0.32f, 0.0f, "Suelo"),
            core::translate(core::Vec3{0.0f, -0.05f, 0.0f}));
        // Pocos poligonos: el borde de la sombra facetado (el caso del terminador).
        add(sphere(0.8f, 12, 8, {0.8f, 0.3f, 0.25f, 1.0f}, 0.28f, 0.0f, "Esfera facetada"),
            core::translate(core::Vec3{-1.6f, 0.8f, 0.0f}));
        add(sphere(0.7f, 48, 24, {0.95f, 0.8f, 0.55f, 1.0f}, 0.18f, 1.0f, "Esfera metal"),
            core::translate(core::Vec3{1.5f, 0.7f, 0.6f}));
        add(sphere(0.5f, 48, 24, {0.3f, 0.5f, 0.9f, 1.0f}, 0.45f, 0.0f, "Esfera satinada"),
            core::translate(core::Vec3{0.2f, 0.5f, 2.0f}));
        add(box({4.0f, 1.6f, 0.2f}, {0.75f, 0.72f, 0.68f, 1.0f}, 0.55f, 0.0f, "Pared"),
            core::translate(core::Vec3{0.0f, 1.6f, -2.8f}));
        for (int i = 0; i < 3; ++i) {
            add(box({0.18f, 1.2f, 0.18f}, {0.5f, 0.48f, 0.45f, 1.0f}, 0.4f, 0.0f, "Columna"),
                core::translate(core::Vec3{-2.5f + 2.5f * static_cast<float>(i), 1.2f, -1.4f}));
        }
        if (!options.model.empty() && std::filesystem::exists(options.model)) {
            const std::uint32_t index = scene.loadModel(options.model);
            scene.spawnStatic(index, core::translate(core::Vec3{-0.2f, 1.0f, 0.7f}));
        }
        // Luces locales con bombilla grande (penumbra de los rayos).
        scene::PointLight point;
        point.position = core::Vec3{-0.8f, 2.6f, 1.6f};
        point.color = core::Vec3{1.0f, 0.82f, 0.6f};
        point.intensity = 18.0f;
        point.range = 12.0f;
        point.source_radius = 0.25f;
        scene.lights().points.push_back(point);
        scene::SpotLight spot;
        spot.position = core::Vec3{2.8f, 3.4f, 2.2f};
        spot.direction = core::normalize(core::Vec3{-0.6f, -1.0f, -0.5f});
        spot.intensity = 40.0f;
        spot.source_radius = 0.15f;
        scene.lights().spots.push_back(spot);
        scene.placeCamera(core::Vec3{0.4f, 2.1f, 6.2f}, core::Vec3{0.0f, 0.8f, 0.0f});
        }
        if (demo_entry != nullptr) scene.placeCamera(demo_entry->camera, demo_entry->target);
        const core::Vec3 camera_home = demo_entry != nullptr ? demo_entry->camera : core::Vec3{0.4f, 2.1f, 6.2f};
        const core::Vec3 camera_target = demo_entry != nullptr ? demo_entry->target : core::Vec3{0.0f, 0.8f, 0.0f};
        if (!options.animated.empty()) {
            // Entre la camara y su objetivo, a un tercio, mirando a la camara.
            const core::Vec3 spot = camera_home + (camera_target - camera_home) * 0.33f;
            const core::Vec3 to_camera = camera_home - spot;
            scene.spawnActor(scene.loadModel(options.animated), spot.x, spot.z, 1.8f, std::atan2(to_camera.x, to_camera.z));
        }
        scene.camera().setAspectRatio(static_cast<float>(options.width) / static_cast<float>(options.height));
        renderer.uploadModels(scene);

        // Presets del editor (GraphicsConfig): la misma idea, sin depender de el.
        if (!options.preset.empty()) {
            gfx::PostProcessSettings post = renderer.postProcess();
            if (options.preset == "low") {
                post.global_illumination = post.reflections = post.volumetric_light = false;
                post.contact_shadows = post.light_shafts = false;
            } else if (options.preset == "medium") {
                post.volumetric_light = false;
            }
            renderer.setPostProcess(post);
        }

        if (options.volumetric >= 0) {
            gfx::PostProcessSettings post = renderer.postProcess();
            post.volumetric_light = options.volumetric != 0;
            renderer.setPostProcess(post);
        }

        const dm::Input input;
        int orbit_frame = 0;
        const auto frame = [&]() {
            window.pumpEvents();
            renderer.applyPendingResize();
            if (options.orbit) {
                // 20 grados por segundo (a 60 FPS) alrededor del objetivo.
                const float angle = static_cast<float>(orbit_frame++) * (20.0f / 60.0f) * (kPi / 180.0f);
                const core::Vec3 offset = camera_home - camera_target;
                const float c = std::cos(angle);
                const float s = std::sin(angle);
                scene.placeCamera(camera_target + core::Vec3{offset.x * c - offset.z * s, offset.y, offset.x * s + offset.z * c},
                                  camera_target);
            }
            scene.update(input, 1.0f / 60.0f);
            renderer.drawFrame(scene);
        };
        for (int i = 0; i < options.warmup; ++i) frame();

        // --- Tiempos ---
        std::map<std::string, double> pass_ms;
        std::vector<std::string> order;
        double gpu_total = 0.0;
        int gpu_samples = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < options.frames; ++i) {
            frame();
            // Sin la media del perfilador (olvida muy despacio lo caro de los
            // primeros frames): lo de cada frame medido, y una pasada que ese
            // frame no se grabo cuenta 0.
            const auto& timings = renderer.gpuProfiler().lastTimings();
            if (!timings.empty()) {
                for (const gfx::GpuTiming& t : timings) {
                    if (!pass_ms.count(t.name)) order.push_back(t.name);
                    pass_ms[t.name] += t.milliseconds;
                }
                gpu_total += renderer.gpuProfiler().lastTotalMilliseconds();
                ++gpu_samples;
            }
        }
        renderer.waitIdle();
        const double cpu_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / options.frames;

        std::cout << std::fixed << std::setprecision(3);
        std::cout << "GPU: " << renderer.device().name() << "\n";
        std::cout << "Resolucion interna: " << renderer.renderExtent().width << "x" << renderer.renderExtent().height
                  << ", RT " << (renderer.rayTracingActive() ? "si" : "no") << "\n";
        std::cout << "Frame (pared): " << cpu_ms << " ms, GPU media " << (gpu_samples ? gpu_total / gpu_samples : 0.0)
                  << " ms\n";
        for (const std::string& name : order) {
            std::cout << "  " << std::setw(34) << std::left << name << std::right << std::setw(8)
                      << (gpu_samples ? pass_ms[name] / gpu_samples : 0.0) << " ms\n";
        }

        // --- En movimiento: la luz da vueltas y la camara se desplaza ---
        if (options.move) {
            // (Los escenarios de demostracion no tienen luces puntuales.)
            const bool has_point = !scene.lights().points.empty();
            const scene::PointLight base = has_point ? scene.lights().points.front() : scene::PointLight{};
            for (int i = 0; i < 90; ++i) {
                const float t = static_cast<float>(i) / 30.0f;
                if (has_point) {
                    scene.lights().points.front().position =
                        base.position + core::Vec3{1.6f * std::sin(t * 2.0f), 0.0f, 1.2f * std::cos(t * 2.0f)};
                }
                // Con --orbit la camara ya la mueve frame(); sin el, de lado a lado.
                if (!options.orbit) {
                    scene.placeCamera(camera_home + core::Vec3{1.5f * std::sin(t * 0.8f), 0.0f, 0.0f}, camera_target);
                }
                frame();
            }
            asset::ImageRgba8 image;
            if (!options.out.empty() && renderer.readSceneImage(image)) {
                std::filesystem::create_directories(options.out);
                asset::saveImagePng(options.out / "movimiento.png", image);
            }
        }

        // --- Ruido temporal (camara quieta) ---
        if (options.noise_frames > 1 && !options.move) {
            std::vector<asset::ImageRgba8> images;
            for (int i = 0; i < options.noise_frames; ++i) {
                frame();
                asset::ImageRgba8 image;
                if (renderer.readSceneImage(image)) images.push_back(std::move(image));
            }
            asset::ImageRgba8 heatmap;
            const NoiseStats stats = noiseBetween(images, options.out.empty() ? nullptr : &heatmap);
            std::cout << "Ruido temporal: media " << stats.mean << " /255, p99 " << stats.p99 << ", cambian "
                      << stats.changed << " % de los pixeles\n";
            if (!options.out.empty() && !images.empty()) {
                std::filesystem::create_directories(options.out);
                asset::saveImagePng(options.out / "frame.png", images.back());
                asset::saveImagePng(options.out / "ruido.png", heatmap);
                std::cout << "Capturas en " << options.out.string() << "\n";
            }
        }
        renderer.waitIdle();
        renderer.shutdown();
        window.destroy();
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
    std::cout << "OK\n";
    return 0;
}
