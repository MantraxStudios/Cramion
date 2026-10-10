// Comprobacion del render en estereo sin casco (como -emulatestereo de
// Unreal): abre una ventana invisible y dibuja una escena con los dos ojos de
// un casco (cada uno con su estado temporal, lo del frame una vez, la ventana
// de espejo) mientras la cabeza gira y se mueve; despues, como en PC. Con las
// capas de validacion de Vulkan instaladas, sus avisos salen por la consola.
//
//   cramion_stereo_check [modelo] [frames] [carpeta de capturas]
//
// Con carpeta, guarda el ojo derecho dibujado en estereo (stereo_right.png) y
// la misma camara dibujada como en PC (pc_right.png): deben verse igual. Hay
// tambien un panel de UI en el mundo (rectangulos de colores) delante.
// Codigo 0 si termina sin excepciones.

#include <CramionDM/Input.h>
#include <CramionDM/Window.h>
#include <CramionFX/asset/ImageFile.h>
#include <CramionFX/scene/Scene.h>
#include <CramionFX/vk/VulkanRenderer.h>

#include <array>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>

using namespace cramion;

namespace {

// Campo de vision de un Quest 3 (radianes): mas hacia fuera que hacia la nariz.
constexpr float kOuter = 0.96f;
constexpr float kInner = 0.75f;
constexpr float kUp = 0.84f;
constexpr float kDown = -0.96f;
constexpr float kHalfIpd = 0.032f;

// Un panel de UI en el mundo delante de la cabeza y mirandola: rectangulos
// lisos (sin textura = blanca).
gfx::WorldUiCanvas testCanvas(const scene::Camera& head) {
    gfx::WorldUiCanvas canvas;
    canvas.id = 1;
    canvas.width = 400;
    canvas.height = 300;
    canvas.size = core::Vec2{0.4f, 0.3f};
    const core::Vec3 right = head.right();
    const core::Vec3 up = head.up();
    const core::Vec3 back = head.forward() * -1.0f;
    const core::Vec3 at = head.position() + head.forward() * 0.8f + right * 0.12f - up * 0.05f;
    canvas.transform = core::Mat4::identity();
    for (int i = 0; i < 3; ++i) {
        canvas.transform.m[0][i] = (&right.x)[i];
        canvas.transform.m[1][i] = (&up.x)[i];
        canvas.transform.m[2][i] = (&back.x)[i];
        canvas.transform.m[3][i] = (&at.x)[i];
    }
    const auto rect = [&](float x0, float y0, float x1, float y1, std::uint32_t color) {
        const auto base = static_cast<std::uint32_t>(canvas.vertices.size());
        canvas.vertices.push_back({x0, y0, 0.5f, 0.5f, color});
        canvas.vertices.push_back({x1, y0, 0.5f, 0.5f, color});
        canvas.vertices.push_back({x1, y1, 0.5f, 0.5f, color});
        canvas.vertices.push_back({x0, y1, 0.5f, 0.5f, color});
        for (const std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) canvas.indices.push_back(base + i);
    };
    rect(0.0f, 0.0f, 400.0f, 300.0f, 0xE0302018u);    // fondo (ABGR)
    rect(20.0f, 20.0f, 380.0f, 80.0f, 0xFFF0A040u);   // barra de titulo
    rect(20.0f, 120.0f, 180.0f, 180.0f, 0xFF40C060u); // boton verde
    rect(220.0f, 120.0f, 380.0f, 180.0f, 0xFF4060E0u); // boton rojo
    rect(20.0f, 220.0f, 300.0f, 250.0f, 0xFFFFFFFFu); // slider
    gfx::WorldUiBatch batch;
    batch.index_count = static_cast<std::uint32_t>(canvas.indices.size());
    batch.clip[2] = 400.0f;
    batch.clip[3] = 300.0f;
    canvas.batches.push_back(batch);
    return canvas;
}

scene::Camera eyeCamera(const scene::Camera& head, int eye) {
    scene::Camera camera = head;
    const float side = eye == 0 ? -1.0f : 1.0f;
    camera.setPosition(head.position() + head.right() * (side * kHalfIpd));
    if (eye == 0) {
        camera.setFovAngles(-kOuter, kInner, kUp, kDown);
    } else {
        camera.setFovAngles(-kInner, kOuter, kUp, kDown);
    }
    camera.setClipPlanes(0.05f, 500.0f);
    return camera;
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path model = argc > 1 ? argv[1] : "assets/DamagedHelmet.glb";
    const int frames = argc > 2 ? std::max(std::atoi(argv[2]), 1) : 240;
    const std::filesystem::path captures = argc > 3 ? argv[3] : "";
    try {
        dm::Window window;
        if (!window.create({.title = L"Cramion stereo check", .width = 1280, .height = 720, .resizable = false,
                            .visible = false})) {
            std::cerr << "No se pudo crear la ventana\n";
            return 1;
        }
        gfx::EngineInfo info;
        info.app_name = "cramion_stereo_check";
        info.enable_validation = true;
#if defined(__linux__) && !defined(__ANDROID__)
        const gfx::XlibWindow xlib{window.handle()->display, window.handle()->window};
        const gfx::NativeWindow native = &xlib;
#else
        const gfx::NativeWindow native = window.handle();
#endif
        gfx::VulkanRenderer renderer;
        renderer.initialize(info, native, window.width(), window.height());

        // Lo que tiene historia en el tiempo: TAA, SSR, GI, AO, sombras de contacto.
        gfx::GraphicsSettings graphics = renderer.graphicsSettings();
        graphics.upscaler = gfx::Upscaler::Taa;
        graphics.vsync = false;
        renderer.setGraphicsSettings(graphics);
        gfx::PostProcessSettings post = renderer.postProcess();
        post.reflections = post.global_illumination = post.ambient_occlusion = true;
        post.contact_shadows = post.volumetric_light = post.auto_exposure = true;
        renderer.setPostProcess(post);

        scene::Scene scene;
        scene.initialize();
        if (std::filesystem::exists(model)) {
            const std::uint32_t index = scene.loadModel(model);
            scene.spawnStatic(index, core::translate(core::Vec3{0.0f, 1.4f, -1.2f}));
            renderer.uploadModel(scene, index);
            std::cout << "Modelo: " << model.string() << "\n";
        } else {
            std::cout << "Sin modelo (" << model.string() << " no existe): solo cielo y luz\n";
        }

        const dm::Input input;
        // La cabeza gira y se mueve (las historias de cada ojo se reproyectan
        // con su propia vista anterior); los ultimos 90 frames, quieta.
        const auto head_at = [&](int frame, int count) {
            const float t = static_cast<float>(std::min(frame, count - 90)) / 72.0f;
            scene::Camera head = scene.camera();
            head.setPosition(core::Vec3{0.3f * std::sin(t * 1.3f), 1.6f, 0.2f * std::cos(t)});
            head.setRotation(-core::kPi * 0.5f + 0.6f * std::sin(t * 0.9f), -0.15f + 0.2f * std::sin(t * 1.7f));
            return head;
        };
        const auto save = [&](const char* name) {
            if (captures.empty()) return;
            asset::ImageRgba8 image;
            if (renderer.readSceneImage(image) && asset::saveImagePng(captures / name, image)) {
                std::cout << "Captura: " << (captures / name).string() << "\n";
            }
        };
        const auto run = [&](int count, bool stereo) {
            for (int frame = 0; frame < count; ++frame) {
                window.pumpEvents();
                renderer.applyPendingResize();
                scene.update(input, 1.0f / 72.0f);
                const scene::Camera head = head_at(frame, count);
                std::vector<gfx::WorldUiCanvas> ui;
                ui.push_back(testCanvas(head));
                renderer.setWorldUi(std::move(ui));
                scene.camera() = head;
                if (stereo) {
                    const std::array<scene::Camera, 2> eyes = {eyeCamera(head, 0), eyeCamera(head, 1)};
                    scene::Camera center = head;
                    center.setFovAngles(-kOuter, kOuter, kUp, kDown);
                    center.setClipPlanes(0.05f, 500.0f);
                    renderer.renderStereoEmulated(scene, eyes, center);
                } else {
                    scene.camera() = eyeCamera(head, 1);  // la del ojo derecho, como en PC
                }
                renderer.drawFrame(scene);  // en estereo, el espejo (el ultimo ojo)
            }
        };
        renderer.setStereoEmulation(true);
        run(frames, true);
        std::cout << "Estereo: " << frames << " frames\n";
        save("stereo_right.png");
        renderer.setStereoEmulation(false);
        run(std::max(frames, 120), false);
        std::cout << "PC: " << std::max(frames, 120) << " frames\n";
        save("pc_right.png");
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
