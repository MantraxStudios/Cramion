// Pruebas de la exportacion a Android (consola):
//   - controles tactiles: joystick, mirar, toque = clic, botones -> teclas
//   - game.ini de los controles y BuildConfigs.json de ida y vuelta
//   - manifiesto y nombres de paquete
//   - un APK de verdad (si hay Android SDK): aapt2, zip, zipalign, firma;
//     se comprueba con "aapt2 dump badging" y "apksigner verify".
//
// Uso: CramionAndroidBuildTests [libmain.so] [icono.png]

#include "AndroidBuild.h"
#include "BuildConfig.h"

#include <CramionCore/project/TouchInterface.h>
#include <CramionDM/TouchControls.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace cramion;
using namespace cramion::editor;

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("  FALLO: %s\n", what);
    }
}

dm::Event touch(dm::EventType type, int id, float x, float y) {
    dm::Event e;
    e.type = type;
    e.touchId = id;
    e.mouseX = x;
    e.mouseY = y;
    return e;
}

void testTouchControls() {
    std::printf("Controles tactiles\n");
    dm::TouchControls t;
    dm::TouchLayout layout = dm::defaultTouchLayout();
    layout.buttons = {dm::TouchButton{"Saltar", "Space", 0.9f, 0.8f, 1.0f}, dm::TouchButton{"Fuego", "Mouse0", 0.7f, 0.8f, 1.0f}};
    t.setLayout(layout);
    t.setScreen(2000.0f, 1000.0f, 2.0f);
    std::vector<dm::Event> out;
    dm::Input input;
    const auto feed = [&] {
        for (dm::Event& e : out) input.onEvent(e);
        out.clear();
    };

    // Joystick: dedo a la izquierda, arrastrar a la derecha y arriba.
    t.onEvent(touch(dm::EventType::TouchBegan, 1, 300.0f, 700.0f), 0.0, out);
    t.onEvent(touch(dm::EventType::TouchMoved, 1, 300.0f + 200.0f, 700.0f - 60.0f), 0.1, out);
    t.apply(input);
    check(out.empty(), "el joystick no genera teclas ni raton");
    check(input.virtualStickX() > 0.9f && input.virtualStickY() > 0.2f, "el stick marca derecha y algo arriba");
    check(std::sqrt(input.virtualStickX() * input.virtualStickX() + input.virtualStickY() * input.virtualStickY()) <= 1.001f,
          "el stick no pasa de 1");
    t.onEvent(touch(dm::EventType::TouchEnded, 1, 500.0f, 640.0f), 0.2, out);
    t.apply(input);
    check(input.virtualStickX() == 0.0f && input.virtualStickY() == 0.0f, "al soltar vuelve al centro");

    // Boton Saltar -> Space mientras se mantiene.
    t.onEvent(touch(dm::EventType::TouchBegan, 2, 1800.0f, 800.0f), 1.0, out);
    feed();
    check(input.isKeyDown(dm::Key::Space) && input.isKeyPressed(dm::Key::Space) && t.buttonDown(0), "el boton pulsa Space");
    input.newFrame();
    t.onEvent(touch(dm::EventType::TouchEnded, 2, 1800.0f, 800.0f), 1.2, out);
    feed();
    check(!input.isKeyDown(dm::Key::Space) && input.isKeyReleased(dm::Key::Space), "al soltar el boton se suelta Space");
    // Boton de raton.
    t.onEvent(touch(dm::EventType::TouchBegan, 3, 1400.0f, 800.0f), 2.0, out);
    feed();
    check(input.isMouseButtonDown(dm::MouseButton::Left), "Mouse0 pulsa el boton izquierdo");
    t.onEvent(touch(dm::EventType::TouchEnded, 3, 1400.0f, 800.0f), 2.1, out);
    feed();
    input.newFrame();

    // Mirar: arrastrar a la derecha = delta del raton; toque corto = clic.
    t.onEvent(touch(dm::EventType::TouchBegan, 4, 1300.0f, 300.0f), 3.0, out);
    t.onEvent(touch(dm::EventType::TouchMoved, 4, 1400.0f, 300.0f), 3.1, out);
    feed();
    check(input.mouseDeltaX() > 1.0f && std::fabs(input.mouseDeltaY()) < 0.001f, "arrastrar mueve mouseDelta");
    check(!input.isMouseButtonDown(dm::MouseButton::Left), "mirar no pulsa el raton");
    t.onEvent(touch(dm::EventType::TouchEnded, 4, 1400.0f, 300.0f), 3.2, out);
    feed();
    check(!input.isMouseButtonPressed(dm::MouseButton::Left), "un arrastre no es un clic");
    input.newFrame();
    t.onEvent(touch(dm::EventType::TouchBegan, 5, 1200.0f, 250.0f), 4.0, out);
    t.onEvent(touch(dm::EventType::TouchEnded, 5, 1201.0f, 250.0f), 4.1, out);
    feed();
    check(input.isMouseButtonPressed(dm::MouseButton::Left) && input.isMouseButtonReleased(dm::MouseButton::Left),
          "un toque corto es un clic");
    check(std::fabs(input.mouseX() - 1201.0f) < 0.01f, "el clic va donde se toco");

    // Varios dedos a la vez: joystick + mirar + boton.
    input.newFrame();
    t.onEvent(touch(dm::EventType::TouchBegan, 10, 200.0f, 800.0f), 5.0, out);
    t.onEvent(touch(dm::EventType::TouchBegan, 11, 1200.0f, 200.0f), 5.0, out);
    t.onEvent(touch(dm::EventType::TouchBegan, 12, 1800.0f, 800.0f), 5.0, out);
    t.onEvent(touch(dm::EventType::TouchMoved, 10, 200.0f, 700.0f), 5.1, out);
    t.onEvent(touch(dm::EventType::TouchMoved, 11, 1150.0f, 200.0f), 5.1, out);
    feed();
    t.apply(input);
    check(input.virtualStickY() > 0.5f && input.mouseDeltaX() < 0.0f && input.isKeyDown(dm::Key::Space),
          "tres dedos: andar, mirar y saltar a la vez");
    // Perder el foco lo suelta todo.
    t.releaseAll(out);
    feed();
    t.apply(input);
    check(!input.isKeyDown(dm::Key::Space) && input.virtualStickY() == 0.0f, "releaseAll suelta todo");

    // Lua oculta un boton o los controles con un dedo encima: se suelta.
    {
        dm::TouchControls lt;
        lt.setLayout(layout);
        lt.setScreen(2000.0f, 1000.0f, 2.0f);
        dm::Input li;
        std::vector<dm::Event> o;
        lt.onEvent(touch(dm::EventType::TouchBegan, 1, 1800.0f, 800.0f), 0.0, o);
        for (dm::Event& e : o) li.onEvent(e);
        o.clear();
        check(li.isKeyDown(dm::Key::Space), "boton pulsado");
        check(lt.setButtonVisible("saltar", false) && !lt.setButtonVisible("nada", false), "setButtonVisible por etiqueta");
        lt.update(o);
        for (dm::Event& e : o) li.onEvent(e);
        o.clear();
        check(!li.isKeyDown(dm::Key::Space) && !lt.buttonDown(0), "al ocultarlo se suelta");
        lt.onEvent(touch(dm::EventType::TouchBegan, 2, 1800.0f, 800.0f), 1.0, o);
        check(o.empty() || o.front().type != dm::EventType::KeyPressed, "un boton oculto no se pulsa");
        o.clear();
        lt.onEvent(touch(dm::EventType::TouchBegan, 3, 300.0f, 700.0f), 1.0, o);
        lt.onEvent(touch(dm::EventType::TouchMoved, 3, 450.0f, 700.0f), 1.1, o);
        lt.apply(li);
        check(li.virtualStickX() > 0.5f, "stick activo");
        lt.setEnabled(false);
        lt.update(o);
        lt.apply(li);
        check(li.virtualStickX() == 0.0f, "al apagar los controles el stick vuelve");
    }

    // Sin controles: el dedo es el raton.
    dm::TouchLayout off;
    off.enabled = false;
    t.setLayout(off);
    input.newFrame();
    t.onEvent(touch(dm::EventType::TouchBegan, 20, 100.0f, 100.0f), 6.0, out);
    feed();
    check(input.isMouseButtonDown(dm::MouseButton::Left) && std::fabs(input.mouseX() - 100.0f) < 0.01f,
          "sin controles el dedo hace de raton");

    // Input guarda los dedos para Lua.
    dm::Input raw;
    raw.onEvent(touch(dm::EventType::TouchBegan, 7, 10.0f, 20.0f));
    raw.onEvent(touch(dm::EventType::TouchMoved, 7, 15.0f, 25.0f));
    check(raw.touches().size() == 1 && raw.touches()[0].phase == dm::TouchPhase::Began && raw.touches()[0].deltaX == 5.0f,
          "Input: dedo nuevo con su movimiento");
    raw.newFrame();
    check(raw.touches().size() == 1 && raw.touches()[0].phase == dm::TouchPhase::Stationary, "Input: quieto al frame siguiente");
    raw.onEvent(touch(dm::EventType::TouchEnded, 7, 15.0f, 25.0f));
    check(raw.touches()[0].phase == dm::TouchPhase::Ended, "Input: termina");
    raw.newFrame();
    check(raw.touches().empty(), "Input: se va al frame siguiente");
}

void testIniAndConfigs() {
    std::printf("Configuracion de Android\n");
    dm::TouchLayout layout = dm::defaultTouchLayout();
    layout.look_sensitivity = 2.5f;
    layout.buttons.push_back(dm::TouchButton{"A|B", "Mouse1", 0.25f, 0.5f, 1.5f});
    const std::string ini = dm::touchLayoutIni(layout);
    dm::TouchLayout back;
    std::istringstream in(ini);
    std::string line;
    int lines = 0;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq != std::string::npos && dm::parseTouchLayoutLine(line.substr(0, eq), line.substr(eq + 1), back)) ++lines;
    }
    check(lines > 5 && back.buttons.size() == layout.buttons.size(), "game.ini: todos los botones");
    check(std::fabs(back.look_sensitivity - 2.5f) < 0.001f && back.buttons.back().action == "Mouse1" &&
              back.buttons.back().label == "A/B",
          "game.ini: valores y etiqueta sin |");
    check(dm::keyFromName("space") == dm::Key::Space && dm::keyFromName("LeftShift") == dm::Key::LeftShift &&
              dm::keyFromName("e") == dm::Key::E && dm::keyFromName("nada") == dm::Key::Unknown,
          "nombres de teclas");

    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_android_configs.json";
    BuildConfigs configs;
    BuildConfig c;
    c.name = "Movil";
    c.platform = BuildPlatform::Android;
    c.android.package = "com.estudio.prueba";
    c.android.version_code = 42;
    c.android.orientation = 1;
    c.android.make_aab = true;
    c.android.split_obb = true;
    c.android.keystore = "claves/mia.jks";
    c.android.key_alias = "subida";
    c.android.store_password = "secreta";
    configs.configs.push_back(c);
    check(saveBuildConfigs(file, configs), "se guarda");
    {
        std::ifstream saved(file);
        const std::string text((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
        check(text.find("secreta") == std::string::npos, "la contrasena no se guarda en el proyecto");
    }
    const BuildConfigs loaded = loadBuildConfigs(file);
    const BuildConfig& l = loaded.configs.front();
    check(l.platform == BuildPlatform::Android && l.android.package == "com.estudio.prueba" && l.android.version_code == 42 &&
              l.android.orientation == 1 && l.android.make_aab && l.android.split_obb && l.android.key_alias == "subida",
          "ida y vuelta de los ajustes de Android");
    const std::string game_ini = buildConfigIni(l, "Prueba");
    check(game_ini.find("platform=android") != std::string::npos && game_ini.find("android_fps=") != std::string::npos,
          "game.ini lleva el perfil movil");
    std::filesystem::remove(file);

    // Interfaz tactil del proyecto (ProjectSettings/TouchInterface.json).
    const std::filesystem::path touch_file = std::filesystem::temp_directory_path() / "cramion_touch_interface.json";
    dm::TouchLayout t = dm::defaultTouchLayout();
    t.look = false;
    t.buttons[1].visible = false;
    t.buttons.push_back(dm::TouchButton{"Disparar", "Mouse0", 0.6f, 0.7f, 1.3f, true});
    check(project::saveTouchInterface(touch_file, t), "TouchInterface.json se guarda");
    const dm::TouchLayout tl = project::loadTouchInterface(touch_file);
    check(!tl.look && tl.buttons.size() == t.buttons.size() && !tl.buttons[1].visible && tl.buttons.back().action == "Mouse0" &&
              std::fabs(tl.buttons.back().size - 1.3f) < 0.001f,
          "TouchInterface.json de ida y vuelta");
    check(project::loadTouchInterface(touch_file.parent_path() / "no_existe.json").buttons.size() ==
              dm::defaultTouchLayout().buttons.size(),
          "sin archivo: los controles por defecto");
    std::filesystem::remove(touch_file);
}

void testManifest() {
    std::printf("Manifiesto\n");
    check(validAndroidPackage("com.estudio.juego") && validAndroidPackage("a.b_2"), "paquetes validos");
    check(!validAndroidPackage("juego") && !validAndroidPackage("com..x") && !validAndroidPackage("com.1x") &&
              !validAndroidPackage("com.ju ego") && !validAndroidPackage(".com.x"),
          "paquetes no validos");
    check(defaultAndroidPackage("Mi Juego 2!") == "com.cramion.mijuego2" && validAndroidPackage(defaultAndroidPackage("9 vidas")),
          "paquete por defecto");
    AndroidPackageInput in;
    in.package = "com.estudio.juego";
    in.version_code = 7;
    in.version_name = "1.2 <beta>";
    in.orientation = 1;
    in.vibrate = false;
    const std::string m = androidManifest(in);
    check(m.find("package=\"com.estudio.juego\"") != std::string::npos && m.find("versionCode=\"7\"") != std::string::npos,
          "paquete y version");
    check(m.find("1.2 &lt;beta&gt;") != std::string::npos, "texto escapado");
    check(m.find("android.app.lib_name") != std::string::npos && m.find("android:value=\"main\"") != std::string::npos &&
              m.find("hasCode=\"false\"") != std::string::npos,
          "NativeActivity con libmain.so");
    // Vulkan 1.0.3 (modo compatible en los moviles): no 1.3, que dejaba fuera
    // a la mayoria de los Mali, Adreno y PowerVR.
    check(m.find("sensorPortrait") != std::string::npos && m.find("VIBRATE") == std::string::npos &&
              m.find("INTERNET") != std::string::npos && m.find("0x400003") != std::string::npos &&
              m.find("0x403000") == std::string::npos,
          "orientacion, permisos y Vulkan 1.0");
}

void testRealApk(const std::filesystem::path& so, const std::filesystem::path& icon) {
    std::printf("APK real\n");
    const AndroidToolchain tools = findAndroidToolchain(std::filesystem::current_path(), 35);
    if (!tools.ok()) {
        std::printf("  (sin Android SDK: %s)\n", tools.error.c_str());
        return;
    }
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "cramion_android_test";
    std::error_code e;
    std::filesystem::remove_all(dir, e);
    std::filesystem::create_directories(dir, e);
    std::filesystem::path lib = so;
    if (lib.empty() || !std::filesystem::exists(lib)) {
        lib = dir / "libmain.so";  // basta para empaquetar (no se ejecuta)
        std::ofstream(lib, std::ios::binary) << std::string(10000, 'x');
    }
    std::ofstream(dir / "game.ini") << "title=Prueba\nbuild=1\n";
    std::ofstream(dir / "pack.crpack", std::ios::binary) << std::string(300000, 'p');
    std::ofstream(dir / "a.spv", std::ios::binary) << std::string(1234, 's');

    AndroidPackageInput in;
    in.package = "com.cramion.pruebaapk";
    in.label = "Prueba \"APK\" d'Andrés";
    in.version_code = 3;
    in.icon = icon;
    in.fallback_icon = icon;
    in.libraries = {{"arm64-v8a", lib}};
    in.assets = {{"shaders/a.spv", dir / "a.spv"}, {"Game/game.ini", dir / "game.ini"}};
    in.pack = dir / "pack.crpack";
    in.pack_name = "Prueba.crpack";
    in.make_apk = true;
    in.make_aab = true;  // sin bundletool junto al editor, se descarga
    in.output_folder = dir / "out";
    in.file_stem = "Prueba";
    in.work_folder = dir / "work";
    AndroidPackageResult result;
    std::string error;
    const bool ok = buildAndroidPackage(tools, in, nullptr, nullptr, result, &error);
    if (!ok) std::printf("  %s\n%s\n", error.c_str(), result.log.c_str());
    check(ok && std::filesystem::exists(result.apk), "se crea el APK");
    if (in.make_aab) check(ok && std::filesystem::exists(result.aab), "se crea el AAB");
    if (!ok) return;

    // aapt2 dump: paquete, version, nombre y la actividad nativa.
    const std::filesystem::path dump = dir / "dump.txt";
    const std::string command = "\"\"" + (tools.build_tools / "aapt2.exe").string() + "\" dump badging \"" + result.apk.string() +
                                "\" > \"" + dump.string() + "\" 2>&1\"";
    std::system(command.c_str());
    std::ifstream d(dump);
    const std::string text((std::istreambuf_iterator<char>(d)), std::istreambuf_iterator<char>());
    check(text.find("name='com.cramion.pruebaapk'") != std::string::npos && text.find("versionCode='3'") != std::string::npos,
          "aapt2: paquete y version");
    check(text.find("application-label:'Prueba \"APK\" d'Andr") != std::string::npos ||
              text.find("application-label:'Prueba") != std::string::npos,
          "aapt2: nombre");
    check(text.find("native-code: 'arm64-v8a'") != std::string::npos, "aapt2: libmain.so arm64");
    if (text.find("native-code") == std::string::npos) std::printf("%s\n", text.c_str());

    // apksigner verify.
    const std::filesystem::path verify = dir / "verify.txt";
    _putenv_s("JAVA_HOME", tools.java_bin.parent_path().string().c_str());
    const std::string vcommand = "\"\"" + (tools.build_tools / "apksigner.bat").string() + "\" verify --verbose \"" +
                                 result.apk.string() + "\" > \"" + verify.string() + "\" 2>&1\"";
    std::system(vcommand.c_str());
    std::ifstream v(verify);
    const std::string vtext((std::istreambuf_iterator<char>(v)), std::istreambuf_iterator<char>());
    // Con minSdk >= 28 apksigner firma con v3 (v2 solo para Android 7-8).
    check(vtext.find("Verifies") != std::string::npos && (vtext.find("(APK Signature Scheme v2): true") != std::string::npos ||
                                                          vtext.find("(APK Signature Scheme v3): true") != std::string::npos),
          "apksigner verify (firma v2/v3)");
    if (vtext.find("Verifies") == std::string::npos) std::printf("%s\n", vtext.c_str());

    // zipalign -c: alineado (las .so a 16 KB).
    const std::string zcommand = "\"\"" + (tools.build_tools / "zipalign.exe").string() + "\" -c -P 16 4 \"" + result.apk.string() + "\"\"";
    check(std::system(zcommand.c_str()) == 0, "zipalign -c -P 16");

    // Con OBB: el paquete fuera del APK.
    in.split_obb = true;
    in.make_aab = false;
    in.work_folder = dir / "work2";
    in.file_stem = "PruebaObb";
    AndroidPackageResult obb;
    check(buildAndroidPackage(tools, in, nullptr, nullptr, obb, &error), "APK con OBB");
    check(std::filesystem::exists(obb.obb) && obb.obb.filename() == "main.3.com.cramion.pruebaapk.obb" &&
              std::filesystem::file_size(obb.apk) + 200000 < std::filesystem::file_size(result.apk),
          "el OBB lleva los assets y el APK queda pequeno");
    std::printf("  APK %llu bytes, con OBB %llu + %llu\n", static_cast<unsigned long long>(std::filesystem::file_size(result.apk)),
                static_cast<unsigned long long>(std::filesystem::file_size(obb.apk)),
                static_cast<unsigned long long>(std::filesystem::file_size(obb.obb)));
}

}  // namespace

// Herramienta: un juego ya exportado para Windows (su carpeta Game/) como APK
// para probar en un movil o emulador. Se ejecuta desde build/:
//   CramionAndroidBuildTests --game <Game> <salida> [x86_64]
int packageGame(const std::filesystem::path& game, const std::filesystem::path& out, bool x86) {
    const std::filesystem::path editor = std::filesystem::current_path();
    const AndroidToolchain tools = findAndroidToolchain(editor, 35);
    if (!tools.ok()) {
        std::printf("%s\n", tools.error.c_str());
        return 1;
    }
    AndroidPackageInput in;
    in.package = "com.cramion.prueba";
    in.label = "Prueba Cramion";
    in.version_code = 1;
    in.orientation = 2;
    in.fallback_icon = editor / "editor_icons" / "logo.png";
    in.libraries.emplace_back("arm64-v8a", editor / "android" / "arm64-v8a" / "libmain.so");
    if (x86) in.libraries.emplace_back("x86_64", editor / "android" / "x86_64" / "libmain.so");
    std::error_code e;
    for (std::filesystem::recursive_directory_iterator it(editor / "shaders", e); !e && it != std::filesystem::recursive_directory_iterator();
         it.increment(e)) {
        if (!it->is_regular_file()) continue;
        in.assets.emplace_back(std::filesystem::relative(it->path(), editor).generic_string(), it->path());
    }
    std::filesystem::create_directories(out / "_game", e);
    {
        std::ifstream src(game / "game.ini");
        std::ofstream dst(out / "_game" / "game.ini");
        dst << src.rdbuf() << "\nplatform=android\nbuild=" << std::filesystem::file_time_type::clock::now().time_since_epoch().count()
            << "\n";
    }
    in.assets.emplace_back("Game/game.ini", out / "_game" / "game.ini");
    in.assets.emplace_back("Game/banner.png", game / "banner.png");
    for (const auto& entry : std::filesystem::directory_iterator(game)) {
        if (entry.path().extension() == ".crpack") {
            in.pack = entry.path();
            in.pack_name = entry.path().filename().string();
        }
    }
    in.output_folder = out;
    in.file_stem = "Prueba";
    in.work_folder = out / "_work";
    AndroidPackageResult result;
    std::string error;
    const bool ok = buildAndroidPackage(tools, in, [](float f, const std::string& s) { std::printf("  %3.0f%% %s\n", f * 100.0f, s.c_str()); },
                                        nullptr, result, &error);
    std::printf("%s\n", ok ? result.apk.string().c_str() : (error + "\n" + result.log).c_str());
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--game") {
        return packageGame(argv[2], argv[3], argc > 4 && std::string(argv[4]) == "x86_64");
    }
    testTouchControls();
    testIniAndConfigs();
    testManifest();
    testRealApk(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(),
                argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path("assets/icon.png"));
    std::printf("%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
