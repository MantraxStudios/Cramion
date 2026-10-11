// Steam (platform/Steam.h) y pruebas automaticas (Test / Assert) desde Lua.
//
//   Steam.unlockAchievement("PRIMERA_SANGRE")
//   Steam.uploadScore("Puntos", 1200, function(ok, puesto) ... end)
//   Steam.createLobby("public", 4, function(ok, sala) Steam.setLobbyData(sala, "ip", "1.2.3.4:7777") end)
//
//   Test.case("suma", function() Assert.equal(1 + 1, 2) end)
//   Test.case("salta", {scene = "Scenes/Nivel1.crscene"}, function()
//       local j = Scene.find("Jugador")
//       Test.waitSeconds(1)
//       Assert.greater(j.position.y, 0)
//   end)
//
// Los scripts de C++ tienen lo mismo (Steam::, Test::, Assert::) por la API
// generada (cramion_sdkgen). Se incluye al final de Scripting.cpp (necesita
// ScriptSystem::Impl).

namespace {

// Biblioteca de pruebas (en Lua: asi funciona igual en el editor, en el juego
// y desde C++ por el puente). El Test Runner del editor usa las funciones
// __cramion_test_* (internas: no salen en la API).
constexpr const char* kTestLibrary = R"lua(
local T = { cases = {}, assertions = 0, failures = 0, messages = {}, result = nil, first_failure = nil,
            active = false, timeout = nil, frames = nil, co = nil, wait = nil, wait_timed_out = false,
            elapsed = 0, name = "" }
__cramion_tests = T

local clock = (os and os.clock) or function() return 0 end

local function describe(v)
  if type(v) == "string" then return string.format("%q", v) end
  return tostring(v)
end

local function record(text)
  T.failures = T.failures + 1
  T.messages[#T.messages + 1] = text
  if T.first_failure == nil then T.first_failure = text end
end

local function check(ok, default, msg)
  T.assertions = T.assertions + 1
  if ok then return true end
  local text = default
  if msg ~= nil then text = tostring(msg) .. " (" .. default .. ")" end
  record(text)
  error("Assert: " .. text, 3)
end

local function same(a, b, depth)
  if a == b then return true end
  if type(a) ~= type(b) then return false end
  if type(a) == "number" then return a ~= a and b ~= b end
  if type(a) ~= "table" then return false end
  depth = depth or 0
  if depth > 20 then return false end
  for k, v in pairs(a) do
    if not same(v, b[k], depth + 1) then return false end
  end
  for k in pairs(b) do
    if a[k] == nil then return false end
  end
  return true
end

local function components(v)
  if type(v) == "number" then return { v } end
  if type(v) ~= "userdata" and type(v) ~= "table" then return nil end
  local ok, x = pcall(function() return v.x end)
  if not ok or type(x) ~= "number" then return nil end
  local out = { v.x, v.y }
  local okz, z = pcall(function() return v.z end)
  if okz and type(z) == "number" then out[#out + 1] = z end
  local okw, w = pcall(function() return v.w end)
  if okw and type(w) == "number" then out[#out + 1] = w end
  return out
end

Assert = {}
function Assert.isTrue(value, msg) return check(value and true or false, "se esperaba verdadero, es " .. describe(value), msg) end
function Assert.isFalse(value, msg) return check(not value, "se esperaba falso, es " .. describe(value), msg) end
function Assert.equal(actual, expected, msg)
  return check(same(actual, expected), "se esperaba " .. describe(expected) .. ", es " .. describe(actual), msg)
end
function Assert.notEqual(actual, other, msg)
  return check(not same(actual, other), "no deberia ser " .. describe(other), msg)
end
function Assert.approx(actual, expected, tolerance, msg)
  tolerance = tolerance or 1e-4
  local a, b = components(actual), components(expected)
  local ok = a ~= nil and b ~= nil and #a == #b
  if ok then
    for i = 1, #a do
      if math.abs(a[i] - b[i]) > tolerance then ok = false end
    end
  end
  return check(ok, "se esperaba " .. describe(expected) .. " (+-" .. tostring(tolerance) .. "), es " .. describe(actual), msg)
end
function Assert.isNil(value, msg) return check(value == nil, "se esperaba nil, es " .. describe(value), msg) end
function Assert.notNil(value, msg) return check(value ~= nil, "no deberia ser nil", msg) end
function Assert.greater(a, b, msg)
  return check(type(a) == "number" and type(b) == "number" and a > b, describe(a) .. " no es mayor que " .. describe(b), msg)
end
function Assert.less(a, b, msg)
  return check(type(a) == "number" and type(b) == "number" and a < b, describe(a) .. " no es menor que " .. describe(b), msg)
end
function Assert.atLeast(a, b, msg)
  return check(type(a) == "number" and type(b) == "number" and a >= b, describe(a) .. " es menor que " .. describe(b), msg)
end
function Assert.atMost(a, b, msg)
  return check(type(a) == "number" and type(b) == "number" and a <= b, describe(a) .. " es mayor que " .. describe(b), msg)
end
function Assert.contains(container, value, msg)
  local found = false
  if type(container) == "string" then
    found = string.find(container, tostring(value), 1, true) ~= nil
  elseif type(container) == "table" then
    for _, v in pairs(container) do
      if same(v, value) then found = true break end
    end
  end
  return check(found, describe(container) .. " no contiene " .. describe(value), msg)
end
function Assert.throws(fn, msg)
  local ok = pcall(fn)
  return check(not ok, "se esperaba un error", msg)
end
function Assert.noError(fn, msg)
  local ok, err = pcall(fn)
  return check(ok, "error: " .. tostring(err), msg)
end
function Assert.fail(msg)
  T.assertions = T.assertions + 1
  local text = msg ~= nil and tostring(msg) or "Assert.fail"
  record(text)
  error("Assert.fail: " .. text, 2)
end

Test = {}
local function needCoroutine(what)
  if T.co == nil or not coroutine.isyieldable() then
    error(what .. ": solo dentro de un test de Play (Test.case con {play = true} o {scene = ...})", 3)
  end
end
function Test.case(name, opts, fn)
  if type(opts) == "function" then fn, opts = opts, nil end
  opts = opts or {}
  local c = { name = tostring(name), fn = fn, scene = opts.scene, timeout = opts.timeout or 10,
              play = opts.play == true or opts.scene ~= nil }
  T.cases[#T.cases + 1] = c
  return c
end
function Test.wait(frames)
  needCoroutine("Test.wait")
  coroutine.yield({ frames = frames or 1 })
end
function Test.waitSeconds(seconds)
  needCoroutine("Test.waitSeconds")
  coroutine.yield({ seconds = seconds or 1 })
end
function Test.waitUntil(fn, timeout, msg)
  needCoroutine("Test.waitUntil")
  T.wait_timed_out = false
  coroutine.yield({ until_fn = fn, limit = timeout or 5 })
  if T.wait_timed_out then
    T.wait_timed_out = false
    Assert.fail(msg or ("Test.waitUntil: no se cumplio en " .. tostring(timeout or 5) .. " s"))
  end
end
function Test.pass(msg) T.result = { ok = true, message = msg ~= nil and tostring(msg) or "" } end
function Test.fail(msg)
  local text = msg ~= nil and tostring(msg) or "Test.fail"
  record(text)
  T.result = { ok = false, message = text }
end
function Test.log(msg)
  T.messages[#T.messages + 1] = tostring(msg)
  print("[Test] " .. tostring(msg))
end
function Test.setTimeout(seconds) T.timeout = seconds end
function Test.setFrames(frames) T.frames = frames end
function Test.isRunning()
  if T.active then return true end
  local ok, v = pcall(function() return CVar ~= nil and CVar.get("test.Running") == true end)
  return ok and v == true
end
function Test.name() return T.name end
function Test.elapsed() return T.elapsed end

-- --- Para el Test Runner del editor ---
local function json(v)
  local t = type(v)
  if t == "nil" then return "null" end
  if t == "boolean" then return v and "true" or "false" end
  if t == "number" then
    if v ~= v or v == math.huge or v == -math.huge then return "0" end
    return string.format("%.17g", v)
  end
  if t == "string" then
    local map = { ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
    local escaped = v:gsub('[%c"\\]', function(c) return map[c] or string.format("\\u%04x", c:byte()) end)
    return '"' .. escaped .. '"'
  end
  if t == "table" then
    local parts = {}
    if #v > 0 or next(v) == nil then
      for i = 1, #v do parts[i] = json(v[i]) end
      return "[" .. table.concat(parts, ",") .. "]"
    end
    for k, x in pairs(v) do parts[#parts + 1] = json(tostring(k)) .. ":" .. json(x) end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  return json(tostring(v))
end

local function resetRun(name)
  T.assertions, T.failures, T.messages, T.result, T.first_failure = 0, 0, {}, nil, nil
  T.name = name or ""
  T.elapsed = 0
  T.wait = nil
  T.wait_timed_out = false
  T.active = true
end

function __cramion_test_reset_cases() T.cases = {} end

function __cramion_test_list()
  local out = {}
  for _, c in ipairs(T.cases) do
    out[#out + 1] = { name = c.name, play = c.play, scene = c.scene or "", timeout = c.timeout }
  end
  return json(out)
end

function __cramion_test_run_edit(filter)
  local results = {}
  for _, c in ipairs(T.cases) do
    if not c.play and (filter == nil or filter == "" or filter == c.name) then
      resetRun(c.name)
      local start = clock()
      local ok, err = true, nil
      if type(c.fn) == "function" then ok, err = pcall(c.fn) else ok, err = false, "Test.case sin funcion" end
      local passed = ok and T.failures == 0 and (T.result == nil or T.result.ok)
      local message = ""
      if not ok then message = tostring(err)
      elseif T.first_failure then message = T.first_failure
      elseif T.result then message = T.result.message end
      results[#results + 1] = { name = c.name, ok = passed, message = message, assertions = T.assertions,
                                seconds = clock() - start, log = T.messages }
    end
  end
  T.active = false
  return json(results)
end

function __cramion_test_begin(name)
  for _, c in ipairs(T.cases) do
    if c.name == name then
      resetRun(name)
      if type(c.fn) ~= "function" then return "missing" end
      T.co = coroutine.create(c.fn)
      return "ok"
    end
  end
  return "missing"
end

function __cramion_test_step(dt)
  local status = { state = "running", message = "", assertions = T.assertions }
  if T.co == nil then
    status.state = "failed"
    status.message = "no hay ningun test en marcha"
    return json(status)
  end
  dt = dt or 0
  T.elapsed = T.elapsed + dt
  local w = T.wait
  local resume = true
  if w ~= nil then
    if w.frames ~= nil then
      w.frames = w.frames - 1
      resume = w.frames <= 0
    elseif w.seconds ~= nil then
      w.seconds = w.seconds - dt
      resume = w.seconds <= 0
    elseif w.until_fn ~= nil then
      local ok, v = pcall(w.until_fn)
      w.limit = w.limit - dt
      if ok and v then
        resume = true
      elseif w.limit <= 0 then
        T.wait_timed_out = true
        resume = true
      else
        resume = false
      end
    end
  end
  if resume then
    T.wait = nil
    local ok, request = coroutine.resume(T.co)
    if not ok then
      T.co = nil
      T.active = false
      status.state = "failed"
      status.message = tostring(request)
      status.assertions = T.assertions
      status.log = T.messages
      return json(status)
    end
    if coroutine.status(T.co) == "dead" then
      T.co = nil
      T.active = false
      local passed = T.failures == 0 and (T.result == nil or T.result.ok)
      status.state = passed and "passed" or "failed"
      status.message = T.first_failure or (T.result and T.result.message) or ""
      status.assertions = T.assertions
      status.log = T.messages
      return json(status)
    end
    if type(request) == "table" then T.wait = request end
  end
  status.assertions = T.assertions
  return json(status)
end

function __cramion_test_abort()
  T.co = nil
  T.active = false
end

function __cramion_scene_begin()
  T.active = true
  T.name = "escena"
end

function __cramion_scene_status()
  local result = ""
  if T.result ~= nil then result = T.result.ok and "passed" or "failed" end
  return json({ result = result, message = T.first_failure or (T.result and T.result.message) or "",
                assertions = T.assertions, failures = T.failures, timeout = T.timeout or 0, frames = T.frames or 0,
                log = T.messages })
end
)lua";

}  // namespace

void ScriptSystem::Impl::bindPlatform(sol::state& L) {
    // Test y Assert.
    {
        sol::protected_function_result r = L.safe_script(kTestLibrary, sol::script_pass_on_error, "@Test");
        if (!r.valid()) {
            sol::error e = r;
            write(2, std::string("Biblioteca de pruebas: ") + e.what());
        }
    }

    // Steam: los callbacks de Lua se guardan en una tabla del propio estado
    // (__cramion_steam_cb) y Steam solo recuerda su numero; si el estado ya no
    // existe (se paro el juego) su resultado no llama a nada.
    steam_session = std::make_shared<int>(0);
    L["__cramion_steam_cb"] = L.create_table();
    auto next_id = std::make_shared<int>(0);
    const auto keep = [this, next_id](const sol::object& fn) -> int {
        if (fn.get_type() != sol::type::function || lua == nullptr) return 0;
        const int id = ++*next_id;
        sol::table t = (*lua)["__cramion_steam_cb"];
        t[id] = fn;
        return id;
    };
    const std::weak_ptr<int> session = steam_session;
    // Llama al callback guardado (una vez) con lo que se le pase.
    const auto fire = [this, session](int id, auto&&... args) {
        if (id == 0 || session.expired() || lua == nullptr) return;
        sol::table t = (*lua)["__cramion_steam_cb"];
        sol::protected_function f = t[id];
        t[id] = sol::lua_nil;
        if (!f.valid()) return;
        sol::protected_function_result r = f(std::forward<decltype(args)>(args)...);
        if (!r.valid()) {
            sol::error e = r;
            write(2, std::string("Steam (callback): ") + e.what());
        }
    };
    platform::Steam& steam = platform::Steam::instance();

    sol::table st = L.create_named_table("Steam");
    st["available"] = [&steam]() { return steam.available(); };
    st["error"] = [&steam]() { return steam.error(); };
    st["appId"] = [&steam]() { return static_cast<double>(steam.appId()); };
    st["userId"] = [&steam]() { return steam.userId(); };
    st["userName"] = [&steam]() { return steam.userName(); };
    st["friendName"] = [&steam](const std::string& id) { return steam.friendName(id); };
    st["language"] = [&steam]() { return steam.language(); };
    st["isDlcInstalled"] = [&steam](double app) { return steam.isDlcInstalled(static_cast<std::uint32_t>(app)); };
    st["isSteamDeck"] = [&steam]() { return steam.onSteamDeck(); };

    // Logros y estadisticas.
    st["unlockAchievement"] = [&steam](const std::string& name) { return steam.unlockAchievement(name); };
    st["clearAchievement"] = [&steam](const std::string& name) { return steam.clearAchievement(name); };
    st["isAchievementUnlocked"] = [&steam](const std::string& name) { return steam.achievementUnlocked(name); };
    st["achievementProgress"] = [&steam](const std::string& name, double current, double max) {
        return steam.indicateAchievementProgress(name, static_cast<std::uint32_t>(std::max(0.0, current)),
                                                 static_cast<std::uint32_t>(std::max(1.0, max)));
    };
    st["setStatInt"] = [&steam](const std::string& name, double value) { return steam.setStatInt(name, static_cast<int>(value)); };
    st["setStatFloat"] = [&steam](const std::string& name, double value) { return steam.setStatFloat(name, static_cast<float>(value)); };
    st["getStatInt"] = [&steam](const std::string& name) -> sol::optional<int> {
        if (const auto v = steam.statInt(name)) return *v;
        return sol::nullopt;
    };
    st["getStatFloat"] = [&steam](const std::string& name) -> sol::optional<double> {
        if (const auto v = steam.statFloat(name)) return static_cast<double>(*v);
        return sol::nullopt;
    };
    st["storeStats"] = [&steam]() { return steam.storeStats(); };

    // Marcadores.
    st["uploadScore"] = [this, &steam, keep, fire](const std::string& board, double score, sol::object callback,
                                                    sol::optional<bool> keep_best) {
        const int id = keep(callback);
        steam.uploadScore(board, static_cast<int>(score), keep_best.value_or(true), {}, [fire, id](bool ok, int rank, bool changed) {
            fire(id, ok, rank, changed);
        });
    };
    st["downloadScores"] = [this, &steam, keep, fire](const std::string& board, sol::object callback, sol::optional<std::string> mode,
                                                       sol::optional<int> start, sol::optional<int> end) {
        const int id = keep(callback);
        const std::string m = mode.value_or("global");
        const int kind = m == "around" || m == "alrededor" ? 1 : (m == "friends" || m == "amigos" ? 2 : 0);
        const int first = start.value_or(kind == 1 ? -5 : 1);
        const int last = end.value_or(kind == 1 ? 5 : 10);
        steam.downloadScores(board, kind, first, last, [this, fire, id](bool ok, std::vector<platform::SteamLeaderboardEntry> entries) {
            if (lua == nullptr) return;
            sol::table list = lua->create_table();
            int i = 1;
            for (const platform::SteamLeaderboardEntry& e : entries) {
                sol::table row = lua->create_table();
                row["id"] = e.user_id;
                row["name"] = e.name;
                row["rank"] = e.rank;
                row["score"] = e.score;
                list[i++] = row;
            }
            fire(id, ok, list);
        });
    };

    // Presencia y overlay.
    st["setRichPresence"] = [&steam](const std::string& key, const std::string& value) { return steam.setRichPresence(key, value); };
    st["clearRichPresence"] = [&steam]() { steam.clearRichPresence(); };
    st["openOverlay"] = [&steam](sol::optional<std::string> dialog) { steam.openOverlay(dialog.value_or("friends")); };
    st["openOverlayUrl"] = [&steam](const std::string& url) { steam.openOverlayUrl(url); };
    st["openStore"] = [&steam](sol::optional<double> app) { steam.openStore(static_cast<std::uint32_t>(app.value_or(0.0))); };
    st["overlayEnabled"] = [&steam]() { return steam.overlayEnabled(); };
    st["overlayActive"] = [&steam]() { return steam.overlayActive(); };

    // Steam Cloud.
    st["cloudEnabled"] = [&steam]() { return steam.cloudEnabled(); };
    st["cloudWrite"] = [&steam](const std::string& file, const std::string& data) { return steam.cloudWrite(file, data); };
    st["cloudRead"] = [&steam](const std::string& file) -> sol::optional<std::string> {
        if (auto v = steam.cloudRead(file)) return *v;
        return sol::nullopt;
    };
    st["cloudExists"] = [&steam](const std::string& file) { return steam.cloudExists(file); };
    st["cloudDelete"] = [&steam](const std::string& file) { return steam.cloudDelete(file); };
    st["cloudFiles"] = [this, &steam]() {
        sol::table out = lua->create_table();
        int i = 1;
        for (const auto& [name, size] : steam.cloudFiles()) {
            sol::table row = lua->create_table();
            row["name"] = name;
            row["size"] = size;
            out[i++] = row;
        }
        return out;
    };

    // Workshop.
    st["workshopItems"] = [this, &steam]() {
        sol::table out = lua->create_table();
        int i = 1;
        for (const platform::SteamWorkshopItem& item : steam.subscribedItems()) {
            sol::table row = lua->create_table();
            row["id"] = item.id;
            row["folder"] = item.folder;
            row["size"] = static_cast<double>(item.size);
            row["installed"] = item.installed;
            row["state"] = static_cast<double>(item.state);
            out[i++] = row;
        }
        return out;
    };
    st["workshopUpload"] = [this, &steam, keep, fire](const sol::table& options, sol::object callback) {
        const int id = keep(callback);
        platform::SteamWorkshopUpload item;
        item.id = options.get_or("id", std::string());
        item.title = options.get_or("title", std::string());
        item.description = options.get_or("description", std::string());
        item.folder = options.get_or("folder", std::string());
        item.preview = options.get_or("preview", std::string());
        item.change_note = options.get_or("changeNote", std::string());
        const std::string visibility = options.get_or("visibility", std::string("public"));
        item.visibility = visibility == "friends" ? 1 : (visibility == "private" ? 2 : (visibility == "unlisted" ? 3 : 0));
        // Las rutas de Assets (las del juego) a absolutas.
        for (std::string* path : {&item.folder, &item.preview}) {
            if (!path->empty() && std::filesystem::path(*path).is_relative() && !root.empty()) {
                const std::u8string full = (root / std::filesystem::path(std::u8string(path->begin(), path->end()))).u8string();
                *path = std::string(full.begin(), full.end());
            }
        }
        if (const sol::optional<sol::table> tags = options.get<sol::optional<sol::table>>("tags")) {
            for (std::size_t i = 1; i <= tags->size(); ++i) item.tags.push_back(tags->get_or(i, std::string()));
        }
        steam.workshopUpload(item, [fire, id](bool ok, const std::string& file, bool legal, const std::string& error) {
            fire(id, ok, file, legal, error);
        });
    };
    st["workshopProgress"] = [&steam]() { return steam.workshopUploadProgress(); };

    // Salas (lobbies): con la Network API, el anfitrion guarda su direccion en
    // los datos de la sala y los demas se conectan a ella.
    const auto lobby_type = [](const std::string& t) {
        return t == "private" ? 0 : (t == "friends" ? 1 : (t == "invisible" ? 3 : 2));
    };
    st["createLobby"] = [this, &steam, keep, fire, lobby_type](sol::optional<std::string> type, sol::optional<int> max_members,
                                                                sol::object callback) {
        const int id = keep(callback);
        steam.createLobby(lobby_type(type.value_or("public")), max_members.value_or(4),
                          [fire, id](bool ok, const std::string& lobby) { fire(id, ok, lobby); });
    };
    st["joinLobby"] = [this, &steam, keep, fire](const std::string& lobby, sol::object callback) {
        const int id = keep(callback);
        steam.joinLobby(lobby, [fire, id](bool ok, const std::string& joined) { fire(id, ok, joined); });
    };
    st["leaveLobby"] = [&steam](const std::string& lobby) { steam.leaveLobby(lobby); };
    st["findLobbies"] = [this, &steam, keep, fire](sol::object filters, sol::object callback) {
        // findLobbies(function(ok, salas) end) o findLobbies({modo = "carreras"}, function...)
        if (filters.get_type() == sol::type::function) std::swap(filters, callback);
        const int id = keep(callback);
        std::vector<std::pair<std::string, std::string>> list;
        if (filters.get_type() == sol::type::table) {
            filters.as<sol::table>().for_each([&](const sol::object& k, const sol::object& v) {
                if (k.is<std::string>() && (v.is<std::string>() || v.get_type() == sol::type::number)) {
                    list.emplace_back(k.as<std::string>(), v.is<std::string>() ? v.as<std::string>() : std::to_string(v.as<long long>()));
                }
            });
        }
        steam.findLobbies(list, 50, [this, fire, id](bool ok, std::vector<std::string> lobbies) {
            if (lua == nullptr) return;
            sol::table out = lua->create_table();
            int i = 1;
            for (const std::string& l : lobbies) out[i++] = l;
            fire(id, ok, out);
        });
    };
    st["setLobbyData"] = [&steam](const std::string& lobby, const std::string& key, const std::string& value) {
        return steam.setLobbyData(lobby, key, value);
    };
    st["getLobbyData"] = [&steam](const std::string& lobby, const std::string& key) { return steam.lobbyData(lobby, key); };
    st["lobbyMembers"] = [this, &steam](const std::string& lobby) {
        sol::table out = lua->create_table();
        int i = 1;
        for (const platform::SteamLobbyMember& m : steam.lobbyMembers(lobby)) {
            sol::table row = lua->create_table();
            row["id"] = m.id;
            row["name"] = m.name;
            out[i++] = row;
        }
        return out;
    };
    st["lobbyOwner"] = [&steam](const std::string& lobby) { return steam.lobbyOwner(lobby); };
    st["inviteToLobby"] = [&steam](const std::string& lobby) { steam.inviteToLobby(lobby); };
    // Steam.onLobbyJoinRequested(function(sala) Steam.joinLobby(sala, ...) end)
    st["onLobbyJoinRequested"] = [this, &steam, session](sol::object callback) {
        if (callback.get_type() != sol::type::function) {
            steam.setLobbyJoinRequestedListener(nullptr);
            return;
        }
        sol::table t = (*lua)["__cramion_steam_cb"];
        t["join"] = callback;
        steam.setLobbyJoinRequestedListener([this, session](const std::string& lobby) {
            if (session.expired() || lua == nullptr) return;
            sol::table cb = (*lua)["__cramion_steam_cb"];
            sol::protected_function f = cb["join"];
            if (!f.valid()) return;
            sol::protected_function_result r = f(lobby);
            if (!r.valid()) {
                sol::error e = r;
                write(2, std::string("Steam.onLobbyJoinRequested: ") + e.what());
            }
        });
    };
    st["onOverlay"] = [this, &steam, session](sol::object callback) {
        if (callback.get_type() != sol::type::function) {
            steam.setOverlayListener(nullptr);
            return;
        }
        sol::table t = (*lua)["__cramion_steam_cb"];
        t["overlay"] = callback;
        steam.setOverlayListener([this, session](bool active) {
            if (session.expired() || lua == nullptr) return;
            sol::table cb = (*lua)["__cramion_steam_cb"];
            sol::protected_function f = cb["overlay"];
            if (!f.valid()) return;
            sol::protected_function_result r = f(active);
            if (!r.valid()) {
                sol::error e = r;
                write(2, std::string("Steam.onOverlay: ") + e.what());
            }
        });
    };
}

// --- API publica: el Test Runner del editor y --run-tests ---

bool ScriptSystem::loadTestFile(const std::string& file, std::string* error) {
    Impl& d = *impl_;
    if (!d.lua) {
        if (error) *error = "no hay Play";
        return false;
    }
    const std::filesystem::path path = std::filesystem::path(file).is_absolute() ? std::filesystem::path(file)
                                                                                    : d.root / fromUtf8(file);
    sol::protected_function_result r = d.lua->safe_script_file(path.string(), sol::script_pass_on_error);
    if (!r.valid()) {
        sol::error e = r;
        if (error) *error = e.what();
        return false;
    }
    return true;
}

std::string ScriptSystem::testCall(const std::string& function, const std::string& arg) {
    Impl& d = *impl_;
    if (!d.lua) return {};
    sol::protected_function f = (*d.lua)[function];
    if (!f.valid()) return {};
    sol::protected_function_result r = arg.empty() ? f() : f(arg);
    if (!r.valid()) {
        sol::error e = r;
        d.write(2, function + ": " + e.what());
        return {};
    }
    sol::object value = r;
    return value.is<std::string>() ? value.as<std::string>() : std::string{};
}

std::string ScriptSystem::testStep(float delta_seconds) {
    Impl& d = *impl_;
    if (!d.lua) return {};
    sol::protected_function f = (*d.lua)["__cramion_test_step"];
    if (!f.valid()) return {};
    sol::protected_function_result r = f(delta_seconds);
    if (!r.valid()) {
        sol::error e = r;
        return std::string("{\"state\":\"failed\",\"message\":\"") + e.what() + "\"}";
    }
    sol::object value = r;
    return value.is<std::string>() ? value.as<std::string>() : std::string{};
}
