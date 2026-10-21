#include "selftest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include <SDL3/SDL.h>

#ifdef SDL_PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

#include "app.h"
#include "disc.h"
#include "game.h"
#include "http.h"
#include "input.h"
#include "json_value.h"
#include "mods.h"
#include "paths.h"
#include "settings.h"
#include "update.h"
#include "update_apply.h"

namespace psxstack {

static int checks, failures;

static void check(bool ok, const std::string &what) {
    checks++;
    if (!ok) {
        failures++;
        std::fprintf(stderr, "self-test: FAILED: %s\n", what.c_str());
    }
}

static void write(const std::string &path, const std::string &text) {
    std::string err;
    check(SDL_SaveFile(path.c_str(), text.data(), text.size()), "write " + path + ": " + SDL_GetError());
}

static std::string read(const std::string &path) {
    std::string text, err;
    file_read(path, &text, &err);
    return text;
}

// Deletes `path` and everything under it (the test's own directory only).
static SDL_EnumerationResult remove_entry(void *, const char *dir, const char *name) {
    std::string p = path_join(dir, name);
    SDL_PathInfo info;
    if (SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
        SDL_EnumerateDirectory(p.c_str(), remove_entry, nullptr);
    }
    SDL_RemovePath(p.c_str());
    return SDL_ENUM_CONTINUE;
}

static void remove_tree(const std::string &path) {
    if (path_is_dir(path)) {
        SDL_EnumerateDirectory(path.c_str(), remove_entry, nullptr);
    }
    SDL_RemovePath(path.c_str());
}

// The settled example of the file (docs/LAUNCHER.md "Settings file", schema 1), with other values, a chord, no card 2 and a
// member no one knows yet.
static const char SAMPLE_SETTINGS[] = R"({
  "schema": 1,
  "disc": { "path": "/games/game.cue", "sha1": "0123456789abcdef0123456789abcdef01234567" },
  "video": { "window": true, "scale": 4, "fullscreen": true, "refresh": 60, "renderer": "gpu", "internal_scale": 3 },
  "audio": { "mute": true },
  "memcard1": "cards/card1.mcd",
  "memcard2": null,
  "watchdog": 0,
  "input": {
    "keyboard": { "cross": "X", "start": ["Return", "Keypad Enter"] },
    "gamepad":  { "cross": "south", "up": ["dpup", "lefty-"] },
    "hotkeys":  { "pause": "P", "fullscreen": "F11" }
  },
  "mods": {
    "fast_forward":   { "enabled": true, "hold": [["pad:guide", "pad:south"], "Tab"] },
    "skip_dialogues": { "enabled": false, "toggle": "F2", "fast_forward_waits": false }
  },
  "launcher": { "last_dir": "/games" },
  "future": [1, 2.5, "x\ny", null, {}]
}
)";

static void test_paths() {
    check(path_is_absolute("/a") && path_is_absolute("C:\\a") && path_is_absolute("c:/a") && path_is_absolute("\\a"),
          "absolute paths");
    check(!path_is_absolute("a/b") && !path_is_absolute("C:a") && !path_is_absolute(""), "relative paths");
    check(path_join("/a", "b") == "/a/b" && path_join("/a/", "b") == "/a/b" && path_join("C:\\a\\", "b") == "C:\\a\\b",
          "path_join");
    check(path_join("C:\\a", "b") == "C:\\a\\b" && path_join("C:\\a/x", "b") == "C:\\a/x/b",
          "path_join keeps a Windows path's backslashes");
    check(path_join("/a", "/b") == "/b" && path_join("", "b") == "b", "path_join with an absolute name");
    check(path_dir("/a/b") == "/a" && path_dir("/b") == "/" && path_dir("C:\\b") == "C:\\" && path_dir("b") == "",
          "path_dir");
    check(path_base("/a/b.cue") == "b.cue" && path_base("C:\\x\\y.bin") == "y.bin", "path_base");
    check(path_strip_slash("/a/") == "/a" && path_strip_slash("/") == "/" && path_strip_slash("C:\\") == "C:\\",
          "path_strip_slash");
    check(path_to_url("/home/a b/x") == "file:///home/a%20b/x" && path_to_url("C:\\D W\\") == "file:///C:/D%20W/",
          "path_to_url");
}

static void test_json() {
    std::string err;
    Json j = Json::parse(SAMPLE_SETTINGS, &err);
    check(j.is_object() && err.empty(), "the sample settings parse: " + err);
    Json again = Json::parse(j.dump(), &err);
    check(again == j, "dump() reads back equal");
    check(again.dump() == j.dump(), "dump() is stable");
    check(j.members().front().first == "schema" && j.members().back().first == "future", "member order kept");
    check(Json::parse("{\"a\":1,}", &err).is_null() && !err.empty(), "a trailing comma is an error");
    Json n = Json::number(0.1);
    check(Json::parse(n.dump(), nullptr) == n && n.dump() == "0.1\n", "numbers: the shortest text that reads back");
    check(Json::number(3).dump() == "3\n", "whole numbers without a fraction");
    check(Json::string("a\"\\\x01é").dump() == "\"a\\\"\\\\\\u0001é\"\n", "string escapes");
}

static void test_lookup(const std::string &root) {
    const std::string exe = path_join(root, "exe"), cwd = path_join(root, "cwd"), user = path_join(root, "user");
    std::string err;
    for (const std::string &d : { exe, cwd }) {
        check(path_make_dir(d, &err), err);
    }
    int user_calls = 0;
    DirLookup in;
    in.exe_dir = exe;
    in.cwd = cwd;
    in.user_dir = [&] {
        user_calls++;
        path_make_dir(user, nullptr);
        return user;
    };
    // Nothing there: the per-user directory.
    SettingsDir d = settings_dir_choose(in);
    check(d.source == DirSource::User && d.dir == user && user_calls == 1, "lookup: the per-user directory last");
    // A settings.json in the current directory.
    write(path_join(cwd, SETTINGS_FILE), "{}");
    d = settings_dir_choose(in);
    check(d.source == DirSource::CurrentDir && d.dir == cwd, "lookup: a settings.json in the current directory");
    // portable.txt beside the executable wins over it.
    write(path_join(exe, SETTINGS_PORTABLE_FILE), "");
    d = settings_dir_choose(in);
    check(d.source == DirSource::Portable && d.dir == exe, "lookup: portable.txt beside the executable");
    // The environment wins over portable mode; relative to the current directory.
    in.env = "from-env";
    d = settings_dir_choose(in);
    check(d.source == DirSource::Environment && d.dir == path_join(cwd, "from-env"), "lookup: the environment");
    // --config-dir wins over everything; its trailing separator dropped.
    in.arg = path_join(root, "arg") + "/";
    d = settings_dir_choose(in);
    check(d.source == DirSource::Argument && d.dir == path_join(root, "arg"), "lookup: --config-dir first");
    check(user_calls == 1, "lookup: the per-user directory is only made when it is chosen");
}

static void test_settings_file(const std::string &root) {
    std::string err;
    // A new directory (missing, two levels): the defaults; the first save makes the directory and the file.
    const std::string fresh = path_join(root, "new/settings");
    SettingsFile f;
    f.load(fresh);
    check(f.state() == SettingsFile::State::New && f.values.scale == 2 && f.values.memcard[0].path == "card1.mcd" &&
              f.values.memcard[1].path == "card2.mcd" && f.values.memcard[1].present,
          "a missing file gives the defaults");
    f.values.disc_path = "../disc/game.cue";
    check(f.save(&err), "the first save: " + err);
    check(path_is_file(path_join(fresh, SETTINGS_FILE)), "the first save writes settings.json");
    check(f.resolve("card1.mcd") == path_join(fresh, "card1.mcd"), "paths resolve against the file's directory");
    check(f.resolve("/abs/x.mcd") == "/abs/x.mcd", "absolute paths stay");
    SettingsFile g;
    g.load(fresh);
    check(g.state() == SettingsFile::State::Loaded && g.values.disc_path == "../disc/game.cue" &&
              g.messages().empty(),
          "the saved file reads back");
    Json doc = Json::parse(read(g.path()), nullptr);
    check(doc.find("schema") != nullptr && doc.find("schema")->as_int(0, 0, 99) == SETTINGS_SCHEMA,
          "the file has its schema number");

    // The sample: every value read, every unknown member kept through a save.
    const std::string sample = path_join(root, "sample");
    path_make_dir(sample, nullptr);
    write(path_join(sample, SETTINGS_FILE), SAMPLE_SETTINGS);
    SettingsFile s;
    s.load(sample);
    const Settings &v = s.values;
    check(s.state() == SettingsFile::State::Loaded && s.messages().empty(), "the sample loads without a warning");
    check(v.disc_path == "/games/game.cue" && v.scale == 4 && v.fullscreen && v.refresh == 60 &&
              v.renderer == "gpu" && v.internal_scale == 3 && v.mute &&
              v.memcard[0].present && v.memcard[0].path == "cards/card1.mcd" && !v.memcard[1].present,
          "the sample's values");
    s.values.scale = 2;
    check(s.save(&err), "saving the sample: " + err);
    Json before = Json::parse(SAMPLE_SETTINGS, nullptr), after = Json::parse(read(s.path()), nullptr);
    check(after.find("video") != nullptr && after.find("video")->find("scale")->as_int(0, 0, 99) == 2,
          "the change is written");
    after.member("video").set("scale", Json::number(4));
    after.member("video").erase("subpixel"); // a member the sample predates: written with its default
    check(after == before, "everything else is kept as it was (input, mods, an unknown member, the order)");

    // Invalid values: a warning each, the defaults used.
    const std::string bad = path_join(root, "bad");
    path_make_dir(bad, nullptr);
    write(path_join(bad, SETTINGS_FILE),
          R"({"schema":1,"video":{"scale":99,"refresh":55,"fullscreen":"yes","renderer":"vulkan","internal_scale":9},)"
          R"("audio":3,)"
          R"("memcard1":7,"memcard2":""})");
    SettingsFile b;
    b.load(bad);
    check(b.state() == SettingsFile::State::Loaded && b.messages().size() == 8, "eight warnings for eight bad values");
    check(b.values.scale == 2 && b.values.refresh == GAME_RATE && !b.values.fullscreen && b.values.renderer == "software" &&
              b.values.internal_scale == 1 &&
              b.values.memcard[0].path == "card1.mcd" && b.values.memcard[1].path == "card2.mcd",
          "bad values fall back to the defaults");

    // video.subpixel: "off" read and written back; a bad value warned about, "on" used.
    {
        const std::string sp = path_join(root, "subpixel");
        path_make_dir(sp, nullptr);
        write(path_join(sp, SETTINGS_FILE), R"({"schema":1,"video":{"subpixel":"off"}})");
        SettingsFile f;
        f.load(sp);
        check(f.messages().empty() && f.values.subpixel == "off", "video.subpixel \"off\" is read");
        f.values.subpixel = "on";
        check(f.save(&err), "saving video.subpixel: " + err);
        Json saved = Json::parse(read(f.path()), nullptr);
        check(saved.find("video") != nullptr && saved.find("video")->find("subpixel") != nullptr &&
                  saved.find("video")->find("subpixel")->as_string() == "on",
              "video.subpixel is written");
        write(path_join(sp, SETTINGS_FILE), R"({"schema":1,"video":{"subpixel":"perspective"}})");
        SettingsFile h;
        h.load(sp);
        check(h.messages().empty() && h.values.subpixel == "perspective", "video.subpixel \"perspective\" is read");
        write(path_join(sp, SETTINGS_FILE), R"({"schema":1,"video":{"subpixel":"smooth"}})");
        SettingsFile g;
        g.load(sp);
        check(g.messages().size() == 1 && g.values.subpixel == "on", "a bad video.subpixel: one warning, \"on\"");
    }
    // video.filter (the hardware renderer's present filter): read; written back once the file has it, not added to a
    // file that never chose one; a bad name warned about and "none" used.
    check(Json::parse(read(g.path()), nullptr).find("video")->find("filter") == nullptr,
          "a file that never chose a filter gets no video.filter");
    const std::string filtered = path_join(root, "filter");
    path_make_dir(filtered, nullptr);
    write(path_join(filtered, SETTINGS_FILE), R"({"schema":1,"video":{"renderer":"gpu","filter":"sharp"}})");
    SettingsFile fl;
    fl.load(filtered);
    check(fl.messages().empty() && fl.values.filter == "sharp", "video.filter is read");
    fl.values.filter = "none";
    check(fl.save(&err), "saving the filter: " + err);
    const Json written = Json::parse(read(fl.path()), nullptr);
    const Json *saved = written.find("video") != nullptr ? written.find("video")->find("filter") : nullptr;
    check(saved != nullptr && saved->as_string() == "none", "a filter the file had is written back, \"none\" too");
    write(path_join(filtered, SETTINGS_FILE), R"({"schema":1,"video":{"filter":"blur"}})");
    fl.load(filtered);
    check(fl.messages().size() == 1 && fl.values.filter == "none", "a bad filter: a warning, \"none\"");
    // video.crt (the scanlines and crt filters' parameters): read, written once set, bad values warned about.
    write(path_join(filtered, SETTINGS_FILE),
          R"({"schema":1,"video":{"filter":"crt","crt":{"scanlines":80,"mask":10,"curvature":25}}})");
    fl.load(filtered);
    check(fl.messages().empty() && fl.values.filter == "crt" && fl.values.crt_scanlines == 80 &&
              fl.values.crt_mask == 10 && fl.values.crt_curvature == 25,
          "video.crt is read");
    fl.values.crt_mask = 60;
    check(fl.save(&err), "saving video.crt: " + err);
    const Json crt_written = Json::parse(read(fl.path()), nullptr);
    const Json *crt = crt_written.find("video") != nullptr ? crt_written.find("video")->find("crt") : nullptr;
    check(crt != nullptr && crt->find("mask") != nullptr && crt->find("mask")->as_int(0, 0, 100) == 60 &&
              crt->find("scanlines")->as_int(0, 0, 100) == 80,
          "video.crt is written back");
    write(path_join(filtered, SETTINGS_FILE), R"({"schema":1,"video":{"crt":{"scanlines":101,"mask":-1}}})");
    fl.load(filtered);
    check(fl.messages().size() == 2 && fl.values.crt_scanlines == 50 && fl.values.crt_mask == 30,
          "bad video.crt values: a warning each, the defaults");

    // Not JSON: the defaults; the first save keeps the old file aside.
    const std::string broken = path_join(root, "broken");
    path_make_dir(broken, nullptr);
    write(path_join(broken, SETTINGS_FILE), "{ \"schema\": 1, oops");
    SettingsFile k;
    k.load(broken);
    check(k.state() == SettingsFile::State::Broken && k.messages().size() == 1, "a broken file is reported");
    check(k.save(&err), "saving over a broken file: " + err);
    check(read(path_join(broken, std::string(SETTINGS_FILE) + ".broken")) == "{ \"schema\": 1, oops",
          "the broken file is kept as settings.json.broken");
    check(Json::parse(read(k.path()), nullptr).is_object(), "a valid file replaces it");

    // A newer schema: read, never written.
    const std::string newer = path_join(root, "newer");
    path_make_dir(newer, nullptr);
    const std::string newer_text = R"({"schema":2,"video":{"scale":5}})";
    write(path_join(newer, SETTINGS_FILE), newer_text);
    SettingsFile n;
    n.load(newer);
    check(n.state() == SettingsFile::State::Newer && !n.writable() && n.values.scale == 5, "a newer schema is read");
    n.values.scale = 1;
    check(!n.save(&err) && read(n.path()) == newer_text, "a newer schema is never written");
}

// ---- the controls

static SDL_Event key_event(SDL_Scancode key, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = key;
    e.key.down = down;
    return e;
}

static SDL_Event pad_button_event(SDL_GamepadButton b, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.button = (Uint8)b;
    e.gbutton.down = down;
    return e;
}

static SDL_Event pad_axis_event(SDL_GamepadAxis a, int v) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.axis = (Uint8)a;
    e.gaxis.value = (Sint16)v;
    return e;
}

static void test_input(const std::string &root) {
    std::string err;
    // The binding grammar, both ways (the shortest form written).
    for (const char *text : { R"("F2")", R"("")", R"(["F2", "pad:guide"])", R"([["pad:guide", "pad:south"], "Tab"])",
                              R"("Keypad Enter")", R"("pad:lefty-")" }) {
        Json j = Json::parse(text, nullptr);
        Binding b;
        check(binding_from_json(j, &b, &err) && binding_to_json(b) == j, std::string("a binding round trip: ") + text +
                                                                             " " + err);
    }
    Binding b;
    check(binding_from_json(Json::parse("[]", nullptr), &b, &err) && b.empty() && binding_to_json(b) == Json::string(""),
          "[] is unbound, written \"\"");
    for (const char *bad : { "3", R"(["F2", 3])", R"("pad:nope")", R"("NotAKey")", R"([[]])", R"([["F2", 1]])",
                             R"([["A", "B", "C", "D", "E"]])", R"(["A", "B", "C", "D", "E", "F", "G", "H", "I"])" }) {
        check(!binding_from_json(Json::parse(bad, nullptr), &b, &err) && !err.empty(),
              std::string("a bad binding is refused: ") + bad);
    }
    check(binding_label({ { "F2" }, { "pad:guide", "pad:south" } }) == "F2 or Pad Guide + Pad South", "binding_label");
    std::vector<std::string> names;
    check(names_from_json(Json::parse(R"(["Return", "Keypad Enter"])", nullptr), false, &names, &err) &&
              names.size() == 2 && names_to_json(names) == Json::parse(R"(["Return", "Keypad Enter"])", nullptr),
          "key names: a list");
    check(!names_from_json(Json::string("south"), false, &names, &err) &&
              names_from_json(Json::string("south"), true, &names, &err),
          "a gamepad name is not a key name");
    // The game's defaults (port/src/input.c) are what an empty file means.
    Settings d;
    check(d.keys_for("start") == std::vector<std::string>({ "Return", "Keypad Enter" }) &&
              d.pad_for("up") == std::vector<std::string>({ "dpup", "lefty-" }) &&
              d.hotkey_for("fullscreen") == Binding({ { "F11" } }) && d.hotkey_for("save_state") == Binding({ { "F5" } }) &&
              d.hotkey_for("load_state") == Binding({ { "F9" } }) &&
              d.hotkey_for("menu") == Binding({ { "Escape" }, { "pad:back", "pad:start" } }),
          "the defaults");
    for (const PadButton &pb : pad_buttons()) {
        for (const std::string &k : d.keys_for(pb.id)) {
            check(is_key_name(k), "a default key is an SDL scancode name: " + k);
        }
    }

    // Only changes are written; a reset removes them, and the objects the launcher emptied.
    const std::string dir = path_join(root, "input");
    SettingsFile f;
    f.load(dir);
    f.values.keyboard["cross"] = { "X", "V" };
    f.values.hotkeys["pause"] = { { "pad:guide", "pad:start" } };
    check(f.save(&err), err);
    Json doc = Json::parse(read(f.path()), nullptr);
    check(doc.find("input") != nullptr &&
              *doc.find("input") == Json::parse(R"({"keyboard": {"cross": ["X", "V"]}, "hotkeys": {"pause": [["pad:guide", "pad:start"]]}})",
                                                nullptr),
          "only the changed bindings are written");
    SettingsFile g;
    g.load(dir);
    check(g.values.keys_for("cross") == std::vector<std::string>({ "X", "V" }) &&
              g.values.hotkey_for("pause") == Binding({ { "pad:guide", "pad:start" } }) && g.messages().empty(),
          "the bindings read back");
    g.values.keyboard.clear();
    g.values.hotkeys.clear();
    check(g.save(&err) && Json::parse(read(g.path()), nullptr).find("input") == nullptr,
          "a reset removes the input object the launcher made");
    // A bad binding in the file: a warning, the default.
    write(g.path(), R"({"schema": 1, "input": {"keyboard": {"cross": "Nope", "circle": "V"}, "future": 1}})");
    SettingsFile h;
    h.load(dir);
    check(h.messages().size() == 1 && h.values.keys_for("cross") == std::vector<std::string>({ "X" }) &&
              h.values.keys_for("circle") == std::vector<std::string>({ "V" }),
          "a bad key name: a warning and the default");
    h.values.keyboard["circle"] = { "B" };
    check(h.save(&err) && Json::parse(read(h.path()), nullptr).find("input")->find("future") != nullptr,
          "unknown members of input are kept");

    // The capture, fed events.
    InputCapture c;
    Trigger t;
    c.begin(InputCapture::Kind::Key);
    c.feed(pad_button_event(SDL_GAMEPAD_BUTTON_SOUTH, true));
    check(c.active(), "a key capture ignores the gamepad");
    c.feed(key_event(SDL_SCANCODE_V, true));
    check(c.take_done(&t) && t == Trigger({ "V" }) && !c.active(), "a key capture: the first key");
    c.begin(InputCapture::Kind::Pad);
    c.feed(pad_axis_event(SDL_GAMEPAD_AXIS_LEFTY, -30000));
    check(c.take_done(&t) && t == Trigger({ "lefty-" }), "a gamepad capture: a stick direction");
    c.begin(InputCapture::Kind::Pad);
    c.feed(pad_axis_event(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 20000));
    check(c.take_done(&t) && t == Trigger({ "righttrigger" }), "a gamepad capture: a trigger");
    c.begin(InputCapture::Kind::Binding);
    c.feed(pad_button_event(SDL_GAMEPAD_BUTTON_GUIDE, true));
    c.feed(pad_button_event(SDL_GAMEPAD_BUTTON_SOUTH, true));
    check(!c.take_done(&t) && c.held() == Trigger({ "pad:guide", "pad:south" }), "a chord builds while held");
    c.feed(pad_button_event(SDL_GAMEPAD_BUTTON_SOUTH, false));
    c.feed(pad_button_event(SDL_GAMEPAD_BUTTON_GUIDE, false));
    check(c.take_done(&t) && t == Trigger({ "pad:guide", "pad:south" }), "a chord: everything held, once released");
    c.begin(InputCapture::Kind::Binding);
    c.feed(key_event(SDL_SCANCODE_F2, true));
    c.feed(key_event(SDL_SCANCODE_F2, false));
    check(c.take_done(&t) && t == Trigger({ "F2" }), "a binding capture: one key");
    c.begin(InputCapture::Kind::Binding);
    for (SDL_Scancode k : { SDL_SCANCODE_A, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D, SDL_SCANCODE_E }) {
        c.feed(key_event(k, true));
    }
    for (SDL_Scancode k : { SDL_SCANCODE_A, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D, SDL_SCANCODE_E }) {
        c.feed(key_event(k, false));
    }
    check(c.take_done(&t) && t == Trigger({ "A", "B", "C", "D" }), "a chord stops at the game's 4 inputs");
    c.begin(InputCapture::Kind::Binding);
    c.feed(key_event(SDL_SCANCODE_ESCAPE, true));
    c.feed(key_event(SDL_SCANCODE_ESCAPE, false));
    check(!c.active() && !c.take_done(&t), "Escape cancels");
}

// ---- the mods

// The test manifests (tests/fixtures/mods/<id>/mod.json, compiled in by launcher/CMakeLists.txt as
// <ID>_MANIFEST): fast_forward as the first game shipped it, skip_dialogues (the documented example in
// docs/LAUNCHER.md "Mod manifest"), every_type (one option of each type), presets (a slider capped below its
// range, typed past it with a toggle, and presets); three unusable ones are written below.
#include "selftest_fixtures.h"

static void write_mod(const std::string &mods, const std::string &id, const std::string &text) {
    path_make_dir(path_join(mods, id), nullptr);
    write(path_join(path_join(mods, id), "mod.json"), text);
}

// A game directory with a stand-in game file and the mods above; returns the game's path.
static std::string make_game_dir(const std::string &root) {
    const std::string dir = path_join(root, "gamedir"), mods = path_join(dir, "mods");
    path_make_dir(mods, nullptr);
    write(path_join(dir, GAME_EXE), "");
    write_mod(mods, "fast_forward", FAST_FORWARD_MANIFEST);
    write_mod(mods, "skip_dialogues", SKIP_DIALOGUES_MANIFEST);
    write_mod(mods, "every_type", EVERY_TYPE_MANIFEST);
    write_mod(mods, "presets", PRESETS_MANIFEST);
    write_mod(mods, "broken", "{ \"schema\": 1, ");
    write_mod(mods, "elsewhere", R"({"schema": 1, "id": "other", "name": "Wrong id", "kind": "builtin"})");
    write_mod(mods, "textures", R"({"schema": 1, "id": "textures", "name": "Texture pack", "kind": "data"})");
    return path_join(dir, GAME_EXE);
}

static void test_mods(const std::string &root) {
    const std::string game = make_game_dir(root);
    std::vector<ModManifest> mods = mods_scan(path_join(path_dir(game), "mods"));
    check(mods.size() == 7, "seven manifests found: " + std::to_string(mods.size()));
    auto find = [&](const std::string &id) -> const ModManifest * {
        for (const ModManifest &m : mods) {
            if (m.id == id) {
                return &m;
            }
        }
        return nullptr;
    };
    const ModManifest *ff = find("fast_forward"), *every = find("every_type");
    check(ff != nullptr && ff->error.empty() && ff->options.size() == 4 && ff->requires_port == 1 &&
              ff->option("speed")->type == ModOption::Type::Enum && ff->option("speed")->values.size() == 6 &&
              ff->option("hold")->group == "Controls",
          "path B's fast_forward manifest reads: " + (ff != nullptr ? ff->error : std::string("missing")));
    check(find("skip_dialogues") != nullptr && find("skip_dialogues")->error.empty(), "the documented example reads");
    check(every != nullptr && every->error.empty() && every->option("ratio")->restart &&
              every->option("ratio")->step == 0.05 && every->option("mode")->values[1].label == "b",
          "every option type reads: " + (every != nullptr ? every->error : std::string()));
    check(find("broken") != nullptr && !find("broken")->error.empty(), "a manifest that is not JSON is listed, unusable");
    check(find("elsewhere") != nullptr && find("elsewhere")->error.find("directory") != std::string::npos,
          "an id that is not its directory's name");
    check(find("textures") != nullptr && find("textures")->error.find("textures") != std::string::npos,
          "a data mod without \"textures\": unusable");

    // The settings directory's mods/ (docs/LAUNCHER.md "Data mods"): data mods only; their order (mod_order).
    const std::string user = path_join(root, "usermods");
    path_make_dir(user, nullptr);
    write_mod(user, "pack_b", R"({"schema": 1, "id": "pack_b", "name": "Pack B", "kind": "data",
                                 "textures": {"dir": "png", "filter": "nearest"}})");
    write_mod(user, "pack_a", R"({"schema": 1, "id": "pack_a", "name": "Pack A", "kind": "data", "textures": {}})");
    write_mod(user, "pack_c", R"({"schema": 1, "id": "pack_c", "name": "Pack C", "kind": "data", "textures": {}})");
    write_mod(user, "code", R"({"schema": 1, "id": "code", "name": "Code", "kind": "builtin"})");
    write_mod(user, "filter", R"({"schema": 1, "id": "filter", "name": "Bad filter", "kind": "data",
                                 "textures": {"filter": "cubic"}})");
    write_mod(user, "opts", R"({"schema": 1, "id": "opts", "name": "Options", "kind": "data", "textures": {},
                               "options": [{"id": "x", "name": "X", "type": "bool", "default": false}]})");
    std::vector<ModManifest> um = mods_scan(user, true);
    auto ufind = [&](const std::string &id) -> const ModManifest * {
        for (const ModManifest &m : um) {
            if (m.id == id) {
                return &m;
            }
        }
        return nullptr;
    };
    check(um.size() == 6 && ufind("pack_b") != nullptr && ufind("pack_b")->error.empty() && ufind("pack_b")->user &&
              ufind("pack_b")->textures && ufind("pack_b")->textures_dir == "png" &&
              ufind("pack_b")->textures_filter == "nearest" && ufind("pack_a")->textures_dir == "textures" &&
              ufind("pack_a")->textures_filter == "linear",
          "the user's data mods read, with their textures' directory and filter");
    check(ufind("code") != nullptr && ufind("code")->error.find("comes with the game") != std::string::npos,
          "a built-in mod in the user's mods/: unusable");
    check(ufind("filter") != nullptr && ufind("filter")->error.find("filter") != std::string::npos &&
              ufind("opts") != nullptr && ufind("opts")->error.find("no options") != std::string::npos,
          "a data mod with a bad filter or with options: unusable");
    Json odoc = Json::parse(R"({"mod_order": ["gone", "pack_c"]})", nullptr);
    ModValues ov(&odoc);
    const std::vector<std::string> ids = { "pack_b", "pack_a", "pack_c" };
    check(ov.data_order(ids) == std::vector<std::string>({ "pack_c", "pack_a", "pack_b" }),
          "the data mods' order: mod_order's first, then by id");
    ov.move(ids, "pack_b", -1);
    check(ov.data_order(ids) == std::vector<std::string>({ "pack_c", "pack_b", "pack_a" }) &&
              *odoc.find("mod_order") ==
                  Json::parse(R"(["pack_c", "pack_b", "pack_a", "gone"])", nullptr),
          "a move writes the whole order, an id not installed kept at its end");
    ov.move(ids, "pack_c", -1);
    ov.move(ids, "pack_a", 1);
    check(ov.data_order(ids) == std::vector<std::string>({ "pack_c", "pack_b", "pack_a" }),
          "the first cannot go earlier, the last not later");
    std::string err;
    check(!mod_manifest_load(path_join(root, "nowhere")).error.empty(), "a missing manifest");

    // The values: only changes written; "enabled" false is the default.
    Json doc = Json::object();
    ModValues v(&doc);
    check(!v.enabled("fast_forward") && v.value(*ff, *ff->option("speed")) == Json::string("4x"), "defaults");
    v.set_enabled("fast_forward", true);
    v.set(*ff, *ff->option("speed"), Json::string("8x"));
    check(doc == Json::parse(R"({"mods": {"fast_forward": {"enabled": true, "speed": "8x"}}})", nullptr),
          "enabled and a changed option are written: " + doc.dump());
    v.set(*ff, *ff->option("speed"), Json::string("4x"));
    check(doc == Json::parse(R"({"mods": {"fast_forward": {"enabled": true}}})", nullptr),
          "an option set back to its default is removed");
    v.set(*ff, *ff->option("mute"), Json::boolean(false));
    v.set_enabled("fast_forward", false);
    check(doc == Json::parse(R"({"mods": {"fast_forward": {"mute": false, "enabled": false}}})", nullptr),
          "a mod turned off keeps its options, and says it is off: " + doc.dump());
    v.reset("fast_forward", "mute");
    check(doc == Json::object(), "nothing left: the mods object goes");
    // A value the manifest rejects: reported, the default used; unknown mods and options are kept.
    doc = Json::parse(R"({"mods": {"fast_forward": {"enabled": true, "speed": "9x", "future": 1}, "other": {"enabled": true}}})",
                      nullptr);
    check(v.stored(*ff, *ff->option("speed"), &err) == nullptr && err.find("speed") != std::string::npos &&
              v.value(*ff, *ff->option("speed")) == Json::string("4x"),
          "an invalid stored value: the default, and why");
    v.set(*ff, *ff->option("speed"), Json::string("2x"));
    check(doc.find("mods")->find("other") != nullptr && doc.find("mods")->find("fast_forward")->find("future") != nullptr,
          "unknown mods and options are kept");
    // The option types' checks.
    const ModOption &count = *every->option("count");
    check(count.valid(Json::number(9), &err) && !count.valid(Json::number(10), &err) &&
              !count.valid(Json::number(2.5), &err) && !count.valid(Json::string("3"), &err),
          "int: whole, in range");
    check(every->option("chord")->valid(Json::parse(R"([["pad:guide", "pad:south"], "F9"])", nullptr), &err) &&
              mod_binding(v, *every, *every->option("chord")).size() == 2,
          "a binding option's default chord");

    // slider_max, input_toggle and presets.
    const ModManifest *pr = find("presets");
    check(pr != nullptr && pr->error.empty() && pr->presets.size() == 2 && pr->option("rate")->has_slider_max &&
              pr->option("rate")->slider_max == 5 && pr->option("rate")->input_toggle == "typed",
          "slider_max, input_toggle and presets read: " + (pr != nullptr ? pr->error : std::string("missing")));
    if (pr != nullptr && pr->error.empty()) {
        const ModOption &rate = *pr->option("rate");
        doc = Json::object();
        check(!v.preset_active(*pr, pr->presets[0]) && !v.typed(*pr, rate), "no preset in place at the defaults");
        v.apply_preset(*pr, pr->presets[0]);
        check(v.preset_active(*pr, pr->presets[0]) && !v.preset_active(*pr, pr->presets[1]) &&
                  doc == Json::parse(R"({"mods": {"presets": {"rate": 2}}})", nullptr),
              "a preset writes its values: " + doc.dump());
        v.apply_preset(*pr, pr->presets[1]);
        check(v.typed(*pr, rate) && v.effective(*pr, rate) == Json::number(8) && v.preset_active(*pr, pr->presets[1]),
              "a typed value above the slider's top, with the toggle on");
        v.set(*pr, *pr->option("typed"), Json::boolean(false));
        check(v.effective(*pr, rate) == Json::number(5) && v.value(*pr, rate) == Json::number(8),
              "the toggle off: the value counts as the slider's top, and stays in the file");
    }
    auto load_text = [&](const std::string &id, const std::string &text) {
        write_mod(path_join(root, "bad"), id, text);
        return mod_manifest_load(path_join(path_join(root, "bad"), id)).error;
    };
    const std::string head = R"({"schema": 1, "id": "x", "name": "X", "kind": "builtin", )";
    const std::string rate = R"({"id": "r", "name": "R", "type": "int", "min": 1, "max": 10, "default": 1)";
    check(load_text("x", head + R"("options": [)" + rate + R"(, "slider_max": 5}]})").find("together") !=
              std::string::npos,
          "slider_max without input_toggle");
    check(load_text("x", head + R"("options": [)" + rate + R"(, "slider_max": 5, "input_toggle": "r"}]})")
                  .find("not a bool") != std::string::npos,
          "an input_toggle that is not a bool option");
    check(load_text("x", head + R"("options": [)" + rate + R"(, "slider_max": 11, "input_toggle": "t"},)" +
                    R"({"id": "t", "name": "T", "type": "bool", "default": false}]})")
                  .find("at most max") != std::string::npos,
          "a slider_max above max");
    const std::string capped = R"("options": [)" + rate + R"(, "slider_max": 5, "input_toggle": "t"},)" +
                               R"({"id": "t", "name": "T", "type": "bool", "default": false}]})";
    check(load_text("x", head + R"("presets": [{"id": "p", "name": "P", "values": {"r": 8}}], )" + capped)
                  .find("above slider_max") != std::string::npos,
          "a preset above a slider's top without its toggle");
    check(load_text("x", head + R"("presets": [{"id": "p", "name": "P", "values": {"q": 2}}], )" + capped)
                  .find("not an option") != std::string::npos,
          "a preset of an unknown option");
    check(load_text("x", head + R"("presets": [{"id": "p", "name": "P", "values": {"r": 11}}], )" + capped)
                  .find("out of range") != std::string::npos,
          "a preset value out of range");
}

// ---- the disc check

static DiscCheck::State wait_check(DiscCheck &c) {
    for (int i = 0; i < 120000 / 5 && c.poll() == DiscCheck::State::Running; i++) {
        SDL_Delay(5);
    }
    return c.state();
}

// A file of the EU BIN's exact size without its data (sparse where the filesystem allows): with the right disc.sha1 in
// the settings the launcher counts it as verified (it never hashes a verified disc again), so the play path can be
// tested without the disc.
static bool write_sized_bin(const std::string &path) {
    SDL_IOStream *io = SDL_IOFromFile(path.c_str(), "wb");
    if (io == nullptr) {
        return false;
    }
    bool ok = SDL_SeekIO(io, (Sint64)GAME_DISCS[0].size - 1, SDL_IO_SEEK_SET) >= 0 && SDL_WriteU8(io, 0);
    return SDL_CloseIO(io) && ok;
}

static void test_disc(const std::string &root) {
    std::string bin, err;
    path_make_dir(path_join(root, "sub dir"), nullptr);
    write(path_join(root, "a.cue"), "FILE \"sub dir/Game (Europe).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n");
    check(disc_bin_path(path_join(root, "a.cue"), &bin, &err) && bin == path_join(root, "sub dir/Game (Europe).bin"),
          "a .cue: a quoted relative FILE, CRLF lines: " + bin + err);
    write(path_join(root, "b.CUE"), "REM x\n\tFILE b.bin BINARY\n");
    check(disc_bin_path(path_join(root, "b.CUE"), &bin, &err) && bin == path_join(root, "b.bin"),
          "a .CUE: an unquoted FILE after a tab");
    write(path_join(root, "c.cue"), "FILE \"/abs/c.bin\" BINARY\n");
    check(disc_bin_path(path_join(root, "c.cue"), &bin, &err) && bin == "/abs/c.bin", "a .cue: an absolute FILE");
    write(path_join(root, "d.cue"), "TRACK 01 MODE2/2352\n");
    check(!disc_bin_path(path_join(root, "d.cue"), &bin, &err) && err.find("no FILE") != std::string::npos,
          "a .cue without a FILE line");
    check(disc_bin_path(path_join(root, "x.bin"), &bin, &err) && bin == path_join(root, "x.bin"), "a .bin is itself");

    // The check over a small file: the SHA-1 of "abc" (FIPS 180-4's example), and the wrong disc.
    write(path_join(root, "abc.bin"), "abc");
    DiscCheck c;
    c.start(path_join(root, "abc.bin"));
    check(wait_check(c) == DiscCheck::State::Failed && c.sha1() == "a9993e364706816aba3e25717850c26c9cd0d89d" &&
              c.message().find(" is not " + disc_labels()) != std::string::npos,
          "the check of a wrong file: its SHA-1 and the message");
    c.start(path_join(root, "a.cue"));
    check(wait_check(c) == DiscCheck::State::Failed && c.message().find("cannot open") != std::string::npos &&
              c.sha1().empty(),
          "the check of a .cue whose BIN is missing");
    // The real disc, when the environment names one (<PREFIX>_SELFTEST_DISC: a .cue or .bin of an accepted disc).
    if (const char *disc = SDL_getenv(ENV_SELFTEST_DISC)) {
        Uint64 t0 = SDL_GetTicks();
        c.start(disc);
        check(wait_check(c) == DiscCheck::State::Passed && disc_by_sha1(c.sha1()) != nullptr,
              "the real disc passes: " + c.message());
        std::fprintf(stderr, "self-test: the real disc checked in %.1f s\n", (SDL_GetTicks() - t0) / 1000.0);
    }
}

// ---- the game's stand-in and the launch path

int self_test_fake_game(const char *mode, int argc, char **argv) {
    bool config = false, print = false;
    for (int i = 1; i < argc; i++) {
        config |= std::strcmp(argv[i], "--config") == 0;
        print |= std::strcmp(argv[i], "--print-settings") == 0;
    }
    if (std::strcmp(mode, "update") == 0) { // the self-update's child processes (selftest_update.cpp)
        return self_test_update_child(argc, argv);
    }
    if (std::strcmp(mode, "old") == 0) { // a game before --config: rejects it like any unknown option
        if (config) {
            std::fprintf(stderr, "usage: %s [--disc CUE|BIN] [--no-disc-check] ...\n", argv[0]);
            return 64;
        }
        for (int i = 1; i < argc; i++) {
            std::printf("arg %s\n", argv[i]);
        }
        return 0;
    }
    if (std::strcmp(mode, "nomodsdir") == 0) { // a game with --config and --crash-dir, before --mods-dir
        for (int i = 1; i < argc; i++) {
            if (std::strcmp(argv[i], "--mods-dir") == 0) {
                std::fprintf(stderr, "usage: %s [--config FILE] [--crash-dir DIR] ...\n", argv[0]);
                return 64;
            }
        }
    }
    if (std::strcmp(mode, "invalid") == 0 && print) {
        std::fprintf(stderr, "port: settings %s: video.scale: an integer from 1 to 16, not 17\n", argv[2]);
        return 64;
    }
    if (std::strcmp(mode, "abort") == 0) {
        std::abort();
    }
    if (print) {
        std::puts("{}");
        return 0;
    }
    if (std::strcmp(mode, "crash") == 0) { // a game that writes a crash report where --crash-dir says, then aborts
        std::string dir;
        for (int i = 1; i + 1 < argc; i++) {
            if (std::strcmp(argv[i], "--crash-dir") == 0) {
                dir = argv[i + 1];
            }
        }
        std::printf("port: version 0.0-selftest (none)\n");
        std::printf("port: start: the stand-in\n");
        if (dir.empty()) {
            std::printf("port: fatal: no --crash-dir\n");
            std::fflush(stdout);
            return 1;
        }
        path_make_dir(dir, nullptr);
        const std::string report = path_join(dir, "crash-selftest.txt");
        write(report, std::string(GAME_ID) + " crash report\nkind: crash\nstatus: -6\nbuild: 0.0-selftest (none)\n"
                          "vsync: 123\n\nsignal: SIGABRT (6)\n");
        std::printf("port: crash report: %s\n", report.c_str());
        std::fflush(stdout);
        std::abort();
    }
    if (std::strcmp(mode, "fail") == 0) { // more lines than the launcher keeps, then a fatal error
        for (int i = 0; i < 250; i++) {
            std::printf("line %d\n", i);
        }
        std::fflush(stdout);
        std::fprintf(stderr, "port: fatal: the stand-in's error");
        return 1;
    }
    if (std::strcmp(mode, "wait") == 0) { // a game that runs for two seconds (the update prompt waits for it)
        SDL_Delay(2000);
    }
    std::puts("port: start");
    return 0;
}

// ---- curl's stand-in (http.h)

// Where the stand-in writes its arguments, one a line (the test reads them back); unset: nowhere.
static const char *const SELF_TEST_CURL_ARGS_ENV = PSXSTACK_GAME_ENV_PREFIX "_SELFTEST_CURL_ARGS";

int self_test_fake_curl(const char *reply, int argc, char **argv) {
#ifdef SDL_PLATFORM_WINDOWS
    _setmode(_fileno(stdout), _O_BINARY); // the body byte for byte, as curl writes it
#endif
    std::string out_path, write_out, url;
    std::string args;
    for (int i = 1; i < argc; i++) {
        args += std::string(argv[i]) + "\n";
        const std::string a = argv[i];
        if ((a == "-o" || a == "-w") && i + 1 < argc) {
            (a == "-o" ? out_path : write_out) = argv[i + 1];
            args += std::string(argv[++i]) + "\n";
        } else if ((a == "-A" || a == "-H" || a == "--max-time" || a == "--max-filesize" || a == "--connect-timeout" ||
                    a == "--speed-limit" || a == "--speed-time") &&
                   i + 1 < argc) {
            args += std::string(argv[++i]) + "\n";
        } else if (a.compare(0, 1, "-") != 0) {
            url = a;
        }
    }
    if (const char *log = SDL_getenv(SELF_TEST_CURL_ARGS_ENV)) {
        write(log, args);
    }
    // -w: curl's escapes and %{http_code}, after the body.
    auto finish = [&](const char *code) {
        std::string w;
        for (size_t i = 0; i < write_out.size(); i++) {
            if (write_out.compare(i, 2, "\\n") == 0) {
                w += '\n';
                i++;
            } else if (write_out.compare(i, 12, "%{http_code}") == 0) {
                w += code;
                i += 11;
            } else {
                w += write_out[i];
            }
        }
        std::fwrite(w.data(), 1, w.size(), stdout);
        std::fflush(stdout);
    };
    // A download (-o) gets $<PREFIX>_SELFTEST_CURL_DOWNLOAD's answer when it is set: the check and the package differ.
    const char *download = SDL_getenv(ENV_SELFTEST_CURL_DOWNLOAD);
    const std::string r = !out_path.empty() && download != nullptr && *download != '\0' ? download : reply;
    if (r == "offline") {
        const size_t host = url.find("://") == std::string::npos ? 0 : url.find("://") + 3;
        const std::string name = url.substr(host, url.find('/', host) - host);
        std::fprintf(stderr, "curl: (6) Could not resolve host: %s\n", name.c_str());
        finish("000");
        return 6;
    }
    if (r.compare(0, 5, "http:") == 0) {
        std::fprintf(stderr, "curl: (22) The requested URL returned error: %s\n", r.c_str() + 5);
        finish(r.c_str() + 5);
        return 22;
    }
    const bool slow = r.compare(0, 5, "slow:") == 0;
    const std::string file = slow ? r.substr(5) : r;
    std::string body, err;
    if (!file_read(file, &body, &err)) {
        std::fprintf(stderr, "curl: (37) Couldn't read %s\n", file.c_str());
        finish("000");
        return 37;
    }
    if (out_path.empty()) {
        std::fwrite(body.data(), 1, body.size(), stdout);
    } else if (SDL_IOStream *io = SDL_IOFromFile(out_path.c_str(), "wb")) {
        // In pieces (with pauses when slow), so the launcher sees the file grow.
        for (size_t at = 0; at < body.size(); at += 16384) {
            SDL_WriteIO(io, body.data() + at, SDL_min((size_t)16384, body.size() - at));
            SDL_FlushIO(io);
            if (slow) {
                SDL_Delay(25);
            }
        }
        SDL_CloseIO(io);
    } else {
        std::fprintf(stderr, "curl: (23) Failure writing output to destination\n");
        finish("200");
        return 23;
    }
    finish("200");
    return 0;
}

// The stand-in's abort(): SIGABRT on POSIX (the exit status is the signal's, -6); on Windows the C runtime ends the
// process with status 3, the same as the port's "unimplemented" stop (game_exit_text names both).
#ifdef SDL_PLATFORM_WINDOWS
static const int ABORT_STATUS = 3;
static const char ABORT_TEXT[] = "status 3";
static const char ABORT_RESULT[] = "result: The game stopped with status 3 (abort(), or an unimplemented part of the port)";
#else
static const int ABORT_STATUS = -6;
static const char ABORT_TEXT[] = "signal 6";
static const char ABORT_RESULT[] = "result: The game was killed by signal 6";
#endif

static std::string self_exe() {
    const char *base = SDL_GetBasePath();
    return path_join(base != nullptr ? base : "", LAUNCHER_EXE);
}

static void set_fake_mode(const char *mode) {
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), SELF_TEST_GAME_ENV, mode, true);
}

static void test_game(const std::string &root) {
    const std::string exe = self_exe();
    const std::string settings = path_join(root, SETTINGS_FILE);
    path_make_dir(root, nullptr);
    write(settings, "{\"schema\": 1}\n");
    check(path_is_file(exe), "the launcher's own executable: " + exe);

    std::vector<std::string> tried;
    check(game_find(exe, "/nowhere", &tried) == exe && tried.size() == 1, "--game is the only place looked at");
    tried.clear();
    const std::string dev_build = std::string("/nowhere/port-sdl/") + GAME_EXE;
    check(game_find("", "/nowhere/launcher", &tried).empty() && tried.back() == dev_build,
          "the development tree's SDL build is looked at last");

    set_fake_mode("new");
    check(game_probe(exe, settings).result == GameProbe::Result::Valid, "the probe: a game with --config");
    set_fake_mode("old");
    check(game_probe(exe, settings).result == GameProbe::Result::NoConfig, "the probe: a game without --config");
    set_fake_mode("nomodsdir");
    check(game_probe(exe, settings).result == GameProbe::Result::NoConfig, "the probe: a game without --mods-dir");
    set_fake_mode("invalid");
    GameProbe p = game_probe(exe, settings);
    check(p.result == GameProbe::Result::Invalid && p.message.find("video.scale") != std::string::npos,
          "the probe: a file the game rejects, with its message");
    set_fake_mode("abort");
    p = game_probe(exe, settings);
    check(p.result == GameProbe::Result::Failed && p.message.find(ABORT_TEXT) != std::string::npos,
          "the probe: a game that crashes: " + p.message);
    // The exit statuses in words: the port's own, and a crash as each platform reports it (a signal; on Windows the
    // exception's NTSTATUS code, which SDL_WaitProcess hands out as a negative int).
    check(game_exit_text(0) == "ended normally" && game_exit_text(1).find("fatal error") != std::string::npos &&
              game_exit_text(4).find("watchdog") != std::string::npos && game_exit_text(64).find("64") != std::string::npos &&
              game_exit_text(7) == "ended with status 7",
          "game_exit_text: the port's statuses");
#ifdef SDL_PLATFORM_WINDOWS
    check(game_exit_text((int)0xC0000005u) == "crashed: access violation (0xC0000005)" &&
              game_exit_text((int)0xC00000FDu) == "crashed: stack overflow (0xC00000FD)" &&
              game_exit_text((int)0xC0000409u) == "crashed: an exception (0xC0000409)" &&
              game_exit_text(3).find("abort()") != std::string::npos,
          "game_exit_text: Windows exception codes and abort()");
#else
    check(game_exit_text(-11) == "was killed by signal 11 (a crash: SIGSEGV)" && game_exit_text(-6).find("SIGABRT") != std::string::npos &&
              game_exit_text(3).find("unimplemented") != std::string::npos,
          "game_exit_text: signals and the port's status 3");
#endif

    // A run that writes 251 lines and fails: the status, the last lines kept, the line without a newline.
    set_fake_mode("fail");
    GameRun run;
    std::string err;
    check(run.start({ exe, "--config", settings }, root, &err, false), "the stand-in starts: " + err);
    for (int i = 0; i < 2000 && run.poll(); i++) {
        SDL_Delay(5);
    }
    check(!run.running() && run.exit_code() == 1, "the run's exit status");
    check(run.lines().size() == GameRun::kept_lines && run.lines().back() == "port: fatal: the stand-in's error" &&
              run.lines().front() == "line 51",
          "the last 200 lines are kept, the unterminated last one too");

    // A run that writes a crash report and aborts: the report's path is taken from its output, the output streamed
    // to a log file, the version line kept.
    set_fake_mode("crash");
    const std::string crashes = path_join(root, "crashes"), log = path_join(root, "run.log");
    GameRun crash;
    check(crash.start({ exe, "--config", settings, "--crash-dir", crashes }, root, &err, false, log),
          "the crashing stand-in starts: " + err);
    for (int i = 0; i < 2000 && crash.poll(); i++) {
        SDL_Delay(5);
    }
    check(!crash.running() && crash.exit_code() == ABORT_STATUS, "the crashing run's exit status is abort()'s");
    check(crash.report_path() == path_join(crashes, "crash-selftest.txt") && path_is_file(crash.report_path()),
          "the crash report's path is taken from the output: " + crash.report_path());
    check(crash.version() == "0.0-selftest (none)", "the game's version line is kept: " + crash.version());
    check(crash.log_path() == log && read(log).find("port: crash report: ") != std::string::npos,
          "the whole output went to the log file");

    SettingsFile f;
    f.load(path_join(root, "command"));
    check(game_args("g", f) == std::vector<std::string>({ "g", "--config", f.path(), "--crash-dir",
                                                          path_join(f.dir(), "crashes"), "--mods-dir",
                                                          path_join(f.dir(), "mods") }),
          "the --config --crash-dir --mods-dir command");
    SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), SELF_TEST_GAME_ENV);

    // The real game, when the environment names it (<PREFIX>_SELFTEST_GAME: an SDL build of the game): the probe must
    // accept the file; with <PREFIX>_SELFTEST_DISC too, the game runs 300 frames from the launcher's
    // command (offscreen video, no audio device, unthrottled) and must end normally with the disc checked.
    // <PREFIX>_SELFTEST_GAME=beside: the game the launcher finds by itself beside its own executable, with its mods there
    // too (the release's layout: the AppImage's usr/bin/; the game's package script runs this inside the AppImage).
    const char *real_env = SDL_getenv(ENV_SELFTEST_GAME);
    if (real_env == nullptr) {
        return;
    }
    std::string real = real_env;
    const bool beside = real == "beside";
    if (beside) {
        const char *base = SDL_GetBasePath();
        const std::string exe_dir = path_strip_slash(base != nullptr ? base : "");
        const std::string want = path_join(exe_dir, GAME_EXE);
        tried.clear();
        real = game_find("", exe_dir, &tried);
        check(real == want, "the game beside the launcher is found by its own lookup: " + want + " (found: " +
                                (real.empty() ? std::string("none") : real) + ")");
        if (real != want) {
            return;
        }
        std::fprintf(stderr, "self-test: the game beside the launcher: %s\n", real.c_str());
    }
    const std::string real_dir = path_join(root, "real");
    SettingsFile r;
    r.load(real_dir);
    if (const char *disc = SDL_getenv(ENV_SELFTEST_DISC)) {
        r.values.disc_path = disc;
        r.values.disc_sha1 = GAME_DISCS[0].sha1; // the first described disc (the one a one-disc game has)
    }
    // What the controls screen writes, for the game's own parser: rebinds, a chord, an unbound hotkey.
    r.values.keyboard["cross"] = { "X", "V" };
    r.values.gamepad["circle"] = { "east", "rightx+" };
    r.values.gamepad["select"] = {};
    r.values.hotkeys["pause"] = { { "pad:guide", "pad:start" }, { "P" } };
    r.values.hotkeys["fullscreen"] = {};
    // Every mod beside the game turned on, every option off its default (the game's own checks of mods.<id>).
    int mods_set = 0, mods_found = 0;
    ModValues mv(&r.doc);
    for (const ModManifest &m : mods_scan(path_join(path_dir(real), "mods"))) {
        mods_found++;
        check(m.error.empty(), "the game's manifest " + m.id + ": " + m.error);
        if (!m.error.empty()) {
            continue;
        }
        mv.set_enabled(m.id, true);
        for (const ModOption &o : m.options) {
            Json v;
            switch (o.type) {
            case ModOption::Type::Bool:
                v = Json::boolean(!o.def.as_bool(false));
                break;
            case ModOption::Type::Int:
            case ModOption::Type::Float:
                v = Json::number(o.has_max ? o.max : o.has_min ? o.min : o.def.as_number(0) + 1);
                break;
            case ModOption::Type::Enum:
                v = Json::string(o.values.back().id == o.def.as_string("") ? o.values.front().id
                                                                             : o.values.back().id);
                break;
            case ModOption::Type::Binding:
                v = Json::parse(R"([["pad:guide", "pad:north"], "F9"])", nullptr);
                break;
            }
            mv.set(m, o, v);
            mods_set++;
        }
    }
    if (beside) {
        check(mods_found > 0, "the mods' manifests beside the game: " + path_join(path_dir(real), "mods"));
    }
    // A data mod (an empty texture pack) in the settings directory's mods/, switched on: the game finds it there.
    write_mod(path_join(real_dir, "mods"), "selftest_pack",
              R"({"schema": 1, "id": "selftest_pack", "name": "Self-test pack", "kind": "data", "textures": {}})");
    mv.set_enabled("selftest_pack", true);
    check(r.save(&err), "the real game's settings: " + err);
    p = game_probe(real, r.path());
    check(p.result == GameProbe::Result::Valid, "the real game accepts the launcher's file: " + p.message);
    std::fprintf(stderr, "self-test: the real game accepts the file (%d mod options set)\n", mods_set);
    if (r.values.disc_path.empty() || p.result != GameProbe::Result::Valid) {
        return;
    }
    std::vector<std::string> args = game_args(real, r);
    args.insert(args.end(), { "--max-frames", "300", "--fps", "0" });
    SDL_Environment *env = SDL_GetEnvironment();
    SDL_SetEnvironmentVariable(env, "SDL_VIDEO_DRIVER", "offscreen", true);
    SDL_SetEnvironmentVariable(env, "SDL_AUDIO_DRIVER", "dummy", true);
    GameRun game;
    check(game.start(args, r.dir(), &err, false), "the real game starts: " + err);
    for (int i = 0; i < 120000 / 10 && game.poll(); i++) {
        SDL_Delay(10);
    }
    // The game's own disc line: "disc: PATH: N sectors, SHA-1 <hash> (<the disc's label>)"; the label comes from the
    // game description, so the check keys on the hash of one of its discs.
    bool disc_ok = false, data_mod = false;
    for (const std::string &l : game.lines()) {
        data_mod |= l.find("mods: ") != std::string::npos && l.find(": 1 data mod") != std::string::npos;
        if (l.find("disc: ") == std::string::npos) {
            continue;
        }
        for (const PsxstackGameDisc &d : GAME_DISCS) {
            disc_ok |= l.find(std::string("SHA-1 ") + d.sha1) != std::string::npos;
        }
    }
    check(!game.running() && game.exit_code() == 0 && disc_ok,
          "the real game runs 300 frames from the launcher's command: it " + game_exit_text(game.exit_code()) +
              (game.lines().empty() ? std::string() : "; last line: " + game.lines().back()));
    check(path_is_file(path_join(real_dir, "card1.mcd")), "the game made memory card 1 in the settings directory");
    check(data_mod, "the game found the data mod in the settings directory's mods/ (--mods-dir)");
}

// ---- the window

static void push_key(SDL_Window *w, SDL_Scancode key, SDL_Keymod mod, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = SDL_GetWindowID(w);
    e.key.scancode = key;
    e.key.key = SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, false);
    e.key.mod = mod;
    e.key.down = down;
    check(SDL_PushEvent(&e), std::string("SDL_PushEvent: ") + SDL_GetError());
}

static void pump(App &app, int frames) {
    for (int i = 0; i < frames; i++) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            app.handle_event(e);
        }
        app.frame();
    }
}

static void pump_until(App &app, const std::function<bool()> &done, int max_ms) {
    for (int t = 0; t < max_ms && !done(); t += 5) {
        pump(app, 1);
        SDL_Delay(5);
    }
}

// `path` must stay valid until the event has been handled (SDL_PushEvent does not copy it).
static void drop_file(SDL_Window *w, const std::string &path) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_DROP_FILE;
    e.drop.windowID = SDL_GetWindowID(w);
    e.drop.data = path.c_str();
    SDL_PushEvent(&e);
}

// The play path with the game's stand-in and a verified disc (a file of the BIN's size and the right disc.sha1).
static void test_play(SDL_Window *window, const std::string &root) {
    const std::string dir = path_join(root, "play"), shots = path_join(root, "screens");
    path_make_dir(path_join(dir, "disc"), nullptr);
    if (!write_sized_bin(path_join(dir, "disc/game.bin"))) {
        check(false, std::string("a file of the BIN's size: ") + SDL_GetError());
        return;
    }
    write(path_join(dir, "disc/game.cue"), "FILE \"game.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
    write(path_join(dir, SETTINGS_FILE), std::string("{\"schema\": 1, \"disc\": {\"path\": \"disc/game.cue\", \"sha1\": \"") +
                                             GAME_DISCS[0].sha1 + "\"}}\n");
    SettingsDir location;
    location.dir = dir;
    location.source = DirSource::Argument;
    AppOptions options;
    options.game = self_exe();
    options.echo_game = false;
    App app(window, SDL_GetRenderer(window), location, options);
    check(app.disc_status() == DiscStatus::Verified && app.screen() == Screen::Play,
          "a verified disc: the launcher opens on Play");
    pump(app, 2);

    // The game ends with an error: the window comes back with the status and the last lines.
    set_fake_mode("fail");
    app.play();
    check(app.game_run().running() || !app.play_error().empty(), "Play starts the game: " + app.play_error());
    check((SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) != 0, "the launcher hides while the game runs");
    pump_until(app, [&] { return !app.game_run().running(); }, 10000);
    check(app.play_error().find("fatal error (status 1)") != std::string::npos, "the exit status is shown: " +
                                                                                   app.play_error());
    check((SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) == 0, "the launcher comes back when the game ends");
    pump(app, 2);
    std::string png = path_join(shots, "5-Play-error.png");
    app.frame(png.c_str());
    check(path_is_file(path_join(dir, "logs/last-run.log")), "the run's output went to logs/last-run.log");
    check(app.play_report_path().empty(), "no crash report on a fatal error of the stand-in");

    // The game crashes with a report: the report is shown and Copy's text holds the launcher, the game, the
    // command, the report and the last lines; the previous run's log was kept.
    set_fake_mode("crash");
    app.play();
    pump_until(app, [&] { return !app.game_run().running(); }, 10000);
    check(app.play_error().find(ABORT_TEXT) != std::string::npos, "the crash's exit status is shown: " + app.play_error());
    check(app.play_report_path() == path_join(dir, "crashes/crash-selftest.txt"),
          "the crash report's path is shown: " + app.play_report_path());
    {
        const std::string text = app.play_copy_text();
        check(text.compare(0, std::strlen(LAUNCHER_NAME) + 1, std::string(LAUNCHER_NAME) + " ") == 0 &&
                  text.find(SDL_GetPlatform()) != std::string::npos,
              "Copy's header names the launcher and the platform");
        check(text.find("game: " + self_exe() + " (version 0.0-selftest (none))") != std::string::npos,
              "Copy's header names the game and its version");
        check(text.find("command: " + self_exe() + " --config ") != std::string::npos, "Copy has the command");
        check(text.find(ABORT_RESULT) != std::string::npos, "Copy has the result");
        check(text.find("--- crash report: " + app.play_report_path()) != std::string::npos &&
                  text.find("signal: SIGABRT (6)") != std::string::npos,
              "Copy has the crash report's text");
        check(text.find("--- the last lines") != std::string::npos &&
                  text.find("port: crash report: ") != std::string::npos,
              "Copy has the last lines");
        check(text.find("(the whole output: " + path_join(dir, "logs/last-run.log") + ")") != std::string::npos,
              "Copy names the log file");
    }
    check(read(path_join(dir, "logs/last-run.log")).find("port: crash report: ") != std::string::npos &&
              read(path_join(dir, "logs/last-run.1.log")).find("line 249") != std::string::npos,
          "logs/last-run.log is the crash's, last-run.1.log the previous run's");
    pump(app, 2);
    png = path_join(shots, "5b-Play-crash.png");
    app.frame(png.c_str());

    // A game too old for --config: told so, nothing started.
    set_fake_mode("old");
    app.play();
    check(!app.game_run().running() && app.play_error().find("too old") != std::string::npos,
          "a game without --config is reported: " + app.play_error());

    // The game rejects the file: shown, nothing started.
    set_fake_mode("invalid");
    app.play();
    check(!app.game_run().running() && app.play_error().find("video.scale") != std::string::npos,
          "a file the game rejects is reported: " + app.play_error());
    SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), SELF_TEST_GAME_ENV);

    // The controls: Cross gets V through the prompt (injected key events), the file has it.
    app.set_screen(Screen::Controls);
    app.capture_for(0, "cross");
    pump(app, 2);
    png = path_join(shots, "7-Controls-prompt.png");
    app.frame(png.c_str());
    push_key(window, SDL_SCANCODE_V, SDL_KMOD_NONE, true);
    pump(app, 1);
    push_key(window, SDL_SCANCODE_V, SDL_KMOD_NONE, false);
    pump(app, 3);
    check(!app.capture().active() &&
              app.settings().values.keys_for("cross") == std::vector<std::string>({ "X", "V" }),
          "the prompt adds a key");
    Json written = Json::parse(read(app.settings().path()), nullptr);
    const Json *kb = written.find("input") != nullptr ? written.find("input")->find("keyboard") : nullptr;
    check(kb != nullptr && kb->find("cross") != nullptr && *kb->find("cross") == Json::parse(R"(["X", "V"])", nullptr),
          "the new binding is saved");
    // The same key on Circle is marked as a conflict (a picture), Escape cancels the prompt.
    app.settings().values.keyboard["circle"] = { "C", "V" };
    pump(app, 2);
    png = path_join(shots, "8-Controls-keyboard.png");
    app.frame(png.c_str());
    app.capture_for(2, "pause");
    pump(app, 1);
    push_key(window, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, true);
    pump(app, 1);
    push_key(window, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, false);
    pump(app, 2);
    check(!app.capture().active() && app.settings().values.hotkeys.count("pause") == 0, "Escape leaves the hotkey");
    app.set_screen(Screen::Settings);
    pump(app, 2);
    png = path_join(shots, "9-Settings.png");
    app.frame(png.c_str());
    // The GPU renderer shows its internal-resolution slider (a picture), then back to the default.
    app.settings().values.renderer = "gpu";
    app.settings().values.internal_scale = 4;
    pump(app, 2);
    png = path_join(shots, "9b-Settings-gpu.png");
    app.frame(png.c_str());
    // The CRT filter shows its sliders (a picture).
    app.settings().values.filter = "crt";
    pump(app, 2);
    png = path_join(shots, "9c-Settings-crt.png");
    app.frame(png.c_str());
    app.settings().values.filter = "none";
    app.settings().values.renderer = "software";
    app.settings().values.internal_scale = 1;
    pump(app, 2);

    // A wrong file dropped on the window: checked, refused, the verified disc stays.
    write(path_join(dir, "wrong.bin"), "abc");
    const std::string wrong = path_join(dir, "wrong.bin");
    drop_file(window, wrong);
    pump(app, 1);
    check(app.screen() == Screen::Disc, "a dropped file opens the disc screen");
    pump_until(app, [&] { return app.disc_check().state() != DiscCheck::State::Running; }, 10000);
    check(app.disc_message().find(" is not " + disc_labels()) != std::string::npos,
          "a wrong file is refused: " + app.disc_message());
    check(app.settings().values.disc_path == "disc/game.cue" && app.disc_status() == DiscStatus::Verified,
          "the verified disc stays");
    pump(app, 2);
    png = path_join(shots, "6-Disc-wrong-file.png");
    app.frame(png.c_str());
    // "Check again" on the stored disc (here a file of zeros): it fails, the disc loses its SHA-1 and Play its go, the
    // path (relative, as written) stays to be shown.
    app.choose_disc(app.settings().resolve("disc/game.cue"));
    pump_until(app, [&] { return app.disc_check().state() != DiscCheck::State::Running; }, 30000);
    check(app.disc_status() == DiscStatus::Unverified && app.settings().values.disc_sha1.empty() &&
              app.settings().values.disc_path == "disc/game.cue",
          "a stored disc that fails a new check loses its SHA-1");
    // A typed path that does not exist.
    app.choose_disc("  \"" + path_join(dir, "nothing.cue") + "\"  ");
    check(app.disc_message().find("cannot read") != std::string::npos, "a typed path that does not exist: " +
                                                                            app.disc_message());
}

// The mods screen over the fixtures: a rebind of a mod's key through the prompt, a picture of each kind of page.
static void test_mods_window(SDL_Window *window, const std::string &root) {
    const std::string shots = path_join(root, "screens");
    SettingsDir location;
    location.dir = path_join(root, "modsui");
    location.source = DirSource::Argument;
    AppOptions options;
    options.game = make_game_dir(path_join(root, "modsui-game"));
    write_mod(path_join(location.dir, "mods"), "hd_pack",
              R"({"schema": 1, "id": "hd_pack", "name": "HD pack", "version": "1.0", "kind": "data",
                  "description": "A texture pack in the settings directory.", "textures": {}})");
    write_mod(path_join(location.dir, "mods"), "hd_pack_2",
              R"({"schema": 1, "id": "hd_pack_2", "name": "Second pack", "kind": "data", "textures": {}})");
    App app(window, SDL_GetRenderer(window), location, options);
    check(app.mods().size() == 9 && app.mods_dir() == path_join(path_dir(options.game), "mods") &&
              app.user_mods_dir() == path_join(location.dir, "mods") && app.data_mod_ids().size() == 2,
          "the launcher finds the mods beside the game and the data mods in the settings directory");
    app.set_screen(Screen::Mods);
    app.select_mod("fast_forward");
    pump(app, 3);
    std::string png = path_join(shots, "10-Mods-fast_forward.png");
    app.frame(png.c_str());
    // Toggle gets X through the prompt: the file has it, and X is marked (Cross uses it).
    app.capture_for_mod("fast_forward", "toggle");
    pump(app, 1);
    push_key(window, SDL_SCANCODE_X, SDL_KMOD_NONE, true);
    pump(app, 1);
    push_key(window, SDL_SCANCODE_X, SDL_KMOD_NONE, false);
    pump(app, 3);
    Json written = Json::parse(read(path_join(location.dir, SETTINGS_FILE)), nullptr);
    const Json *ff = written.find("mods") != nullptr ? written.find("mods")->find("fast_forward") : nullptr;
    check(ff != nullptr && ff->find("toggle") != nullptr && *ff->find("toggle") == Json::string("X") &&
              ff->find("enabled") == nullptr,
          "a mod's binding through the prompt is saved (and the mod stays off)");
    pump(app, 2);
    png = path_join(shots, "11-Mods-binding-conflict.png");
    app.frame(png.c_str());
    app.select_mod("every_type");
    pump(app, 3);
    png = path_join(shots, "12-Mods-every-type.png");
    app.frame(png.c_str());
    app.select_mod("broken");
    pump(app, 3);
    png = path_join(shots, "13-Mods-broken.png");
    app.frame(png.c_str());
    // The presets' buttons and the slider (up to slider_max), then a typed value past it (the toggle on).
    app.select_mod("presets");
    pump(app, 3);
    png = path_join(shots, "14-Mods-presets.png");
    app.frame(png.c_str());
    for (const ModManifest &m : app.mods()) {
        if (m.id == "presets" && m.presets.size() == 2) {
            ModValues(&app.settings().doc).apply_preset(m, m.presets[1]);
        }
    }
    pump(app, 3);
    png = path_join(shots, "15-Mods-presets-typed.png");
    app.frame(png.c_str());
    app.select_mod("hd_pack_2");
    pump(app, 3);
    png = path_join(shots, "16-Mods-data.png");
    app.frame(png.c_str());
    check(app.last_error().empty(), "no error in the status bar: " + app.last_error());
}

// ---- the update check (docs/LAUNCHER.md "Updates"): the version rules, GitHub's answer, curl through its stand-in,
// the prompt and the Settings screen. Never the network: the stand-in answers.

// The mouse over the page, its wheel turned down: the Settings screen scrolled to its end (the Updates part).
static void scroll_page_down(SDL_Window *w, App &app) {
    int width = 0, height = 0;
    SDL_GetWindowSize(w, &width, &height);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = SDL_GetWindowID(w);
    e.motion.x = (float)width * 0.6f;
    e.motion.y = (float)height * 0.5f;
    SDL_PushEvent(&e);
    pump(app, 2);
    for (int i = 0; i < 4; i++) {
        SDL_zero(e);
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.windowID = SDL_GetWindowID(w);
        e.wheel.y = -10.0f;
        e.wheel.mouse_x = (float)width * 0.6f;
        e.wheel.mouse_y = (float)height * 0.5f;
        SDL_PushEvent(&e);
        pump(app, 2);
    }
}

static void set_env(const char *name, const std::string &value) {
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), name, value.c_str(), true);
}

static void replace_all(std::string *s, const std::string &from, const std::string &to) {
    for (size_t at = s->find(from); at != std::string::npos; at = s->find(from, at + to.size())) {
        s->replace(at, from.size(), to);
    }
}

// The fixture's answer (tests/fixtures/updates/release.json) for this game, as release `version`.
static std::string release_json(const std::string &version) {
    std::string s = UPDATE_RELEASE_JSON;
    replace_all(&s, "{id}", GAME_ID);
    replace_all(&s, "0.5.0", version);
    return s;
}

static HttpResult http_ok(const std::string &body) {
    HttpResult r;
    r.status = HttpResult::Status::Ok;
    r.http_code = 200;
    r.body = body;
    return r;
}

static void test_update_rules() {
    Version v;
    check(version_parse("0.4.1", &v) && v.major == 0 && v.minor == 4 && v.patch == 1, "version 0.4.1");
    check(version_parse("v10.20.30", &v) && v.major == 10 && v.minor == 20 && v.patch == 30, "version v10.20.30");
    for (const char *bad : { "", "v", "0.4", "0.4.", "0.4.1-3-gabc1234", "0.4.1-dirty", "dev-abc1234", "dev-v0.4.1",
                             "v0.4.1.2", "0.4.x", " 0.4.1", "V0.4.1", "1234567890.0.0", "unknown" }) {
        check(!version_parse(bad, &v), std::string("not a release version: \"") + bad + "\"");
    }
    Version a, b;
    version_parse("0.10.0", &a);
    version_parse("0.9.9", &b);
    check(version_compare(a, b) > 0 && version_compare(b, a) < 0 && version_compare(a, a) == 0,
          "versions compare as three numbers (0.10.0 > 0.9.9)");
    version_parse("1.0.0", &b);
    check(version_compare(a, b) < 0 && version_text(b) == "1.0.0", "1.0.0 > 0.10.0");
    check(update_asset_name("game", "0.5.0", false) == "game-0.5.0-x86_64.AppImage" &&
              update_asset_name("game", "0.5.0", true) == "game-0.5.0-windows-x86_64.zip",
          "the packages' names");
    check(update_api_url("owner/game") == "https://api.github.com/repos/owner/game/releases/latest" &&
              update_releases_url("owner/game") == "https://github.com/owner/game/releases/latest",
          "the API's and the browser's URLs");

    // GitHub's answer: the tag, the page, this platform's package (not the symbols' zip), the digest lower-cased.
    for (bool windows : { false, true }) {
        Release r;
        std::string err;
        const bool ok = update_parse_release(release_json("0.5.0"), GAME_ID, windows, &r, &err);
        check(ok && r.tag == "v0.5.0" && version_text(r.version) == "0.5.0" && r.name == "game v0.5.0" &&
                  r.html_url == "https://github.com/owner/game/releases/tag/v0.5.0",
              "the release's tag, name and page: " + err);
        const std::string name = update_asset_name(GAME_ID, "0.5.0", windows);
        check(r.has_asset && r.asset.name == name &&
                  r.asset.url == "https://github.com/owner/game/releases/download/v0.5.0/" + name &&
                  r.asset.size == (windows ? 4740723 : 5814776) &&
                  r.asset.sha256 == (windows ? "19c3513fae8a2cc9b6ca605ad379539604bd8e6d63a27116bf9afb36f6a78f57"
                                             : "d73d2474cb751190a162326d4666a60aa1f0078aa65ebf8e39c8b52cc0f886a5"),
              std::string("the package for ") + (windows ? "Windows" : "Linux") + ": its name, URL, size, SHA-256");
    }
    {
        Release r;
        std::string err;
        check(update_parse_release(release_json("0.5.0"), "othergame", false, &r, &err) && !r.has_asset,
              "a release without this game's package is still a release");
        check(!update_parse_release(UPDATE_RELEASE_BAD_JSON, GAME_ID, false, &r, &err) &&
                  err.find("\"nightly\"") != std::string::npos,
              "a tag that is not vX.Y.Z is refused: " + err);
        const std::string whole = release_json("0.5.0");
        check(!update_parse_release(whole.substr(0, whole.size() / 2), GAME_ID, false, &r, &err) &&
                  err.find("not a JSON object") != std::string::npos,
              "a cut answer is refused: " + err);
        check(!update_parse_release("[]", GAME_ID, false, &r, &err), "an answer that is not an object is refused");
    }

    // The outcome for a stamp: an update only for an exact X.Y.Z older than the release.
    struct {
        const char *current;
        UpdateCheck::State want;
    } cases[] = { { "0.4.1", UpdateCheck::State::Newer },
                  { "0.5.0", UpdateCheck::State::UpToDate },
                  { "0.6.0", UpdateCheck::State::UpToDate },
                  { "dev-abc1234", UpdateCheck::State::DevBuild },
                  { "0.4.1-3-gabc1234", UpdateCheck::State::DevBuild },
                  { "0.4.1-dirty", UpdateCheck::State::DevBuild } };
    for (const auto &c : cases) {
        Release r;
        std::string msg;
        UpdateCheck::State s = UpdateCheck::evaluate(http_ok(release_json("0.5.0")), "owner/game", c.current, &r, &msg);
        check(s == c.want && version_text(r.version) == "0.5.0",
              std::string("the latest release against the stamp ") + c.current);
    }
    {
        Release r;
        std::string msg;
        HttpResult no;
        no.status = HttpResult::Status::NoCurl;
        no.message = "curl could not be run";
        check(UpdateCheck::evaluate(no, "owner/game", "0.4.1", &r, &msg) == UpdateCheck::State::NoCurl,
              "no curl: its own outcome");
        HttpResult nf;
        nf.http_code = 404;
        nf.message = "curl: (22) The requested URL returned error: 404";
        check(UpdateCheck::evaluate(nf, "owner/game", "0.4.1", &r, &msg) == UpdateCheck::State::Failed &&
                  msg.find("owner/game has no published release") != std::string::npos,
              "HTTP 404: no release: " + msg);
        nf.http_code = 403;
        check(UpdateCheck::evaluate(nf, "owner/game", "0.4.1", &r, &msg) == UpdateCheck::State::Failed &&
                  msg.find("try again later") != std::string::npos,
              "HTTP 403: the rate limit: " + msg);
        check(UpdateCheck::evaluate(http_ok("<html>"), "owner/game", "0.4.1", &r, &msg) == UpdateCheck::State::Failed,
              "an answer that is not JSON fails the check");
    }
    // No repository: nothing to check.
    UpdateCheck none;
    none.configure("", "0.4.1");
    none.start();
    check(!none.available() && none.poll() == UpdateCheck::State::Idle, "no repository: no check");
}

static void test_update_settings(const std::string &root) {
    std::string err;
    path_make_dir(root, nullptr);
    // launcher.updates read; written back with its other members; skipped removed when cleared.
    write(path_join(root, SETTINGS_FILE),
          R"({"schema":1,"launcher":{"last_dir":"/x","updates":{"check":false,"skipped":"v0.5.0","future":1}}})");
    SettingsFile f;
    f.load(root);
    check(f.messages().empty() && !f.values.update_check && f.values.update_skipped == "v0.5.0",
          "launcher.updates is read");
    f.values.update_check = true;
    f.values.update_skipped.clear();
    check(f.save(&err), "saving launcher.updates: " + err);
    Json j = Json::parse(read(f.path()), nullptr);
    const Json *u = j.find("launcher") != nullptr ? j.find("launcher")->find("updates") : nullptr;
    check(u != nullptr && u->find("check") != nullptr && u->find("check")->as_bool(false) &&
              u->find("skipped") == nullptr && u->find("future") != nullptr &&
              j.find("launcher")->find("last_dir")->as_string() == "/x",
          "launcher.updates is written back (skipped gone, an unknown member and last_dir kept)");
    // A file that never changed them gets none; bad values: a warning each.
    write(path_join(root, SETTINGS_FILE), R"({"schema":1})");
    f.load(root);
    f.values.scale = 3;
    check(f.save(&err) && Json::parse(read(f.path()), nullptr).find("launcher") == nullptr,
          "the defaults write no launcher.updates");
    write(path_join(root, SETTINGS_FILE), R"({"schema":1,"launcher":{"updates":{"check":"no","skipped":5}}})");
    f.load(root);
    check(f.messages().size() == 2 && f.values.update_check && f.values.update_skipped.empty(),
          "bad launcher.updates values: a warning each, the defaults");
    write(path_join(root, SETTINGS_FILE), R"({"schema":1,"launcher":{"updates":3}})");
    f.load(root);
    check(f.messages().size() == 1, "launcher.updates not an object: a warning");
}

// http.h through the stand-in: a GET, curl's failures, curl missing, a download with its progress and a cancel.
static void test_http(const std::string &root) {
    path_make_dir(root, nullptr);
    const std::string body_file = path_join(root, "release.json"), args_file = path_join(root, "curl-args.txt");
    write(body_file, release_json("0.5.0"));
    set_env(ENV_FAKE_CURL, self_exe());
    set_env(SELF_TEST_CURL_ARGS_ENV, args_file);
    const std::string url = update_api_url("owner/game");

    set_env(ENV_SELFTEST_CURL_REPLY, body_file);
    HttpResult r = http_get(url, { "Accept: application/vnd.github+json" });
    check(r.status == HttpResult::Status::Ok && r.http_code == 200 && r.body == release_json("0.5.0"),
          "a GET: the body as it came, the status read from -w: " + r.message);
    const std::string args = read(args_file);
    check(args.find("-fsSL\n") != std::string::npos &&
              args.find("-A\n" + http_user_agent() + "\n") != std::string::npos &&
              args.find("-H\nAccept: application/vnd.github+json\n") != std::string::npos &&
              args.find("--max-time\n10\n") != std::string::npos && args.find(url + "\n") != std::string::npos,
          "curl's arguments: -fsSL, the User-Agent, the header, the timeout, the URL:\n" + args);
    check(http_user_agent() == std::string(LAUNCHER_NAME) + "/" + launcher_version,
          "the User-Agent names the launcher");

    set_env(ENV_SELFTEST_CURL_REPLY, "offline");
    r = http_get(url);
    check(r.status == HttpResult::Status::Failed &&
              r.message.find("Could not resolve host: api.github.com") != std::string::npos,
          "no network: curl's own error: " + r.message);
    set_env(ENV_SELFTEST_CURL_REPLY, "http:404");
    r = http_get(url);
    check(r.status == HttpResult::Status::Failed && r.http_code == 404 && r.message.find("404") != std::string::npos,
          "an HTTP error: its status: " + r.message);
    set_env(ENV_FAKE_CURL, path_join(root, "no-such-curl"));
    r = http_get(url);
    check(r.status == HttpResult::Status::NoCurl, "curl missing: its own outcome: " + r.message);
    set_env(ENV_FAKE_CURL, self_exe());

    // On a thread.
    set_env(ENV_SELFTEST_CURL_REPLY, body_file);
    HttpGet get;
    get.start(url);
    for (int t = 0; t < 20000 && get.poll() == HttpGet::State::Running; t += 5) {
        SDL_Delay(5);
    }
    check(get.state() == HttpGet::State::Finished && get.result().status == HttpResult::Status::Ok &&
              get.result().body == release_json("0.5.0"),
          "a GET on a thread: " + get.result().message);

    // A download: 256 KiB of every byte value, in pieces; the progress seen between 0 and 1; the file as sent.
    std::string payload;
    for (int i = 0; i < 256 * 1024; i++) {
        payload += (char)((i * 7 + i / 256) & 0xFF);
    }
    const std::string payload_file = path_join(root, "package.bin"), out = path_join(root, "download/package.bin");
    path_make_dir(path_join(root, "download"), nullptr);
    write(payload_file, payload);
    set_env(ENV_SELFTEST_CURL_REPLY, "slow:" + payload_file);
    HttpDownload d;
    d.start("https://example.invalid/package.bin", out, (int64_t)payload.size());
    bool partial = false;
    for (int t = 0; t < 30000 && d.poll() == HttpDownload::State::Running; t += 5) {
        partial |= d.progress() > 0.0f && d.progress() < 1.0f;
        SDL_Delay(5);
    }
    check(d.state() == HttpDownload::State::Finished && d.result().status == HttpResult::Status::Ok &&
              read(out) == payload,
          "a download: the whole file, byte for byte: " + d.result().message);
    check(partial, "the download's progress is seen between 0 and 1");
    check(read(args_file).find("-o\n" + out + "\n") != std::string::npos &&
              read(args_file).find("--max-filesize\n") != std::string::npos,
          "the download's arguments: -o and --max-filesize");
    // Cancelled once it has started: the process is killed and the partial file removed.
    d.start("https://example.invalid/package.bin", out, (int64_t)payload.size());
    for (int t = 0; t < 20000 && d.received() == 0 && d.poll() == HttpDownload::State::Running; t += 5) {
        SDL_Delay(5);
    }
    d.cancel();
    for (int t = 0; t < 20000 && d.poll() == HttpDownload::State::Running; t += 5) {
        SDL_Delay(5);
    }
    check(d.result().status == HttpResult::Status::Cancelled && !path_is_file(out),
          "a cancelled download: its file removed: " + d.result().message);
    // A size other than the expected one, and an HTTP error: Failed, no file left.
    set_env(ENV_SELFTEST_CURL_REPLY, payload_file);
    d.start("https://example.invalid/package.bin", out, (int64_t)payload.size() + 1);
    for (int t = 0; t < 20000 && d.poll() == HttpDownload::State::Running; t += 5) {
        SDL_Delay(5);
    }
    check(d.result().status == HttpResult::Status::Failed && d.result().message.find("bytes") != std::string::npos &&
              !path_is_file(out),
          "a download of the wrong size fails, its file removed: " + d.result().message);
    set_env(ENV_SELFTEST_CURL_REPLY, "http:404");
    d.start("https://example.invalid/package.bin", out, (int64_t)payload.size());
    for (int t = 0; t < 20000 && d.poll() == HttpDownload::State::Running; t += 5) {
        SDL_Delay(5);
    }
    check(d.result().status == HttpResult::Status::Failed && d.result().http_code == 404 && !path_is_file(out),
          "a download's HTTP error: " + d.result().message);
    SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), SELF_TEST_CURL_ARGS_ENV);
    set_env(ENV_SELFTEST_CURL_REPLY, "offline");
}

// The flow in the window: the check at start, the prompt over Play (Escape is Later, the Release page, Skip and a
// newer release), the setting off and Check now, --no-update-check, the game running, curl missing, a cut answer, a
// development build, no repository; pictures of the prompt and of the Settings screen's part.
static void test_update_window(SDL_Window *window, const std::string &root) {
    const std::string dir = path_join(root, "updates-ui"), shots = path_join(root, "screens");
    const std::string files = path_join(root, "updates-files");
    path_make_dir(files, nullptr);
    const std::string release = path_join(files, "release.json");
    write(release, release_json("0.5.0"));
    set_env(ENV_FAKE_CURL, self_exe());
    set_env(ENV_SELFTEST_CURL_REPLY, release);
    SettingsDir location;
    location.dir = dir;
    location.source = DirSource::Argument;
    std::vector<std::string> opened;
    AppOptions o;
    o.update_repo = "owner/game";
    o.version = "0.4.1";
    o.open_url = [&](const std::string &u) {
        opened.push_back(u);
        return true;
    };
    o.echo_game = false;
    SDL_Renderer *renderer = SDL_GetRenderer(window);
    const auto wait_check = [&](App &app) {
        pump_until(app, [&] { return !app.update_check().checking(); }, 20000);
        pump(app, 3);
    };
    const auto skipped_in_file = [&] {
        Json j = Json::parse(read(path_join(dir, SETTINGS_FILE)), nullptr);
        const Json *l = j.find("launcher"), *u = l != nullptr ? l->find("updates") : nullptr;
        const Json *s = u != nullptr ? u->find("skipped") : nullptr;
        return s != nullptr ? s->as_string() : std::string();
    };
    std::string png;
    {
        App app(window, renderer, location, o);
        check(app.update_check().checking(), "the update check starts with the launcher");
        app.set_screen(Screen::Play);
        wait_check(app);
        check(app.update_check().state() == UpdateCheck::State::Newer && app.update_prompt() &&
                  app.update_prompt_shown(),
              "a newer release: the prompt over the Play screen: " + app.update_check().summary());
        png = path_join(shots, "17-Play-update.png");
        app.frame(png.c_str());
        push_key(window, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, true);
        pump(app, 1);
        push_key(window, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, false);
        pump(app, 3);
        check(!app.update_prompt() && !app.update_prompt_shown() && app.settings().values.update_skipped.empty(),
              "Escape is Later: the prompt closes, nothing is written");
        app.set_screen(Screen::Settings);
        pump(app, 3);
        scroll_page_down(window, app);
        png = path_join(shots, "18-Settings-update.png");
        app.frame(png.c_str());
        check(opened.empty(), "no page opened by itself");
    }
    {
        App app(window, renderer, location, o);
        app.set_screen(Screen::Play);
        wait_check(app);
        check(app.update_prompt_shown(), "Later is not remembered: the next start prompts again");
        app.update_answer(UpdateAnswer::ReleasePage);
        pump(app, 3);
        check(!app.update_prompt_shown() && opened.size() == 1 &&
                  opened.back() == "https://github.com/owner/game/releases/tag/v0.5.0",
              "Release page opens the release's page");
    }
    {
        App app(window, renderer, location, o);
        app.set_screen(Screen::Play);
        wait_check(app);
        app.update_answer(UpdateAnswer::Skip);
        pump(app, 3);
        app.flush();
        check(!app.update_prompt_shown() && skipped_in_file() == "v0.5.0",
              "Skip this version writes launcher.updates.skipped: " + skipped_in_file());
    }
    {
        App app(window, renderer, location, o);
        app.set_screen(Screen::Play);
        wait_check(app);
        check(app.update_check().state() == UpdateCheck::State::Newer && !app.update_prompt() &&
                  !app.update_prompt_shown(),
              "a skipped release is not offered again");
    }
    write(release, release_json("0.6.0"));
    {
        App app(window, renderer, location, o);
        app.set_screen(Screen::Play);
        wait_check(app);
        check(app.update_prompt_shown(), "a release newer than the skipped one is offered");
        app.update_answer(UpdateAnswer::Later);
        pump(app, 2);
    }
    // The setting off: no check at start; Check now still works, and only reports (no prompt).
    {
        SettingsFile f;
        std::string err;
        f.load(dir);
        f.values.update_check = false;
        check(f.save(&err), "turning the check off: " + err);
    }
    {
        App app(window, renderer, location, o);
        app.set_screen(Screen::Play);
        pump(app, 3);
        check(app.update_check().state() == UpdateCheck::State::Idle && !app.update_prompt(),
              "the setting off: no check at start");
        app.update_check_now();
        check(app.update_check().checking(), "Check now starts a check with the setting off");
        wait_check(app);
        check(app.update_check().state() == UpdateCheck::State::Newer && !app.update_prompt(),
              "Check now reports the release without a prompt: " + app.update_check().summary());
        app.set_screen(Screen::Settings);
        pump(app, 3);
        scroll_page_down(window, app);
        png = path_join(shots, "18b-Settings-update-off.png");
        app.frame(png.c_str());
        app.settings().values.update_check = true; // the checkbox: written back
        app.settings().values.update_skipped.clear(); // the Forget button
        app.flush();
    }
    {
        SettingsFile f;
        f.load(dir);
        f.values.update_check = true;
        f.values.update_skipped.clear();
        std::string err;
        check(f.save(&err), "the check back on: " + err);
    }
    // --no-update-check: none at start.
    {
        AppOptions no = o;
        no.update_check = false;
        App app(window, renderer, location, no);
        pump(app, 3);
        check(app.update_check().state() == UpdateCheck::State::Idle, "--no-update-check: no check at start");
    }
    // The game running: the prompt waits until it has ended.
    {
        AppOptions play = o;
        play.game = self_exe();
        set_fake_mode("wait");
        App app(window, renderer, location, play);
        app.set_screen(Screen::Play);
        app.play();
        check(app.game_run().running(), "the stand-in game runs: " + app.play_error());
        pump_until(app, [&] { return !app.update_check().checking(); }, 20000);
        pump(app, 3);
        check(app.game_run().running() && app.update_prompt() && !app.update_prompt_shown(),
              "no prompt while the game runs");
        pump_until(app, [&] { return !app.game_run().running(); }, 20000);
        pump(app, 3);
        check(app.update_prompt_shown(), "the prompt once the game has ended");
        SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), SELF_TEST_GAME_ENV);
    }
    // curl missing: said so, the release page offered; no prompt.
    set_env(ENV_FAKE_CURL, path_join(files, "no-such-curl"));
    {
        App app(window, renderer, location, o);
        app.set_screen(Screen::Play);
        wait_check(app);
        check(app.update_check().state() == UpdateCheck::State::NoCurl && !app.update_prompt() &&
                  app.update_check().summary().find("curl was not found") != std::string::npos &&
                  app.update_check().release_page() == "https://github.com/owner/game/releases/latest",
              "curl missing: reported, the releases page offered: " + app.update_check().summary());
        app.set_screen(Screen::Settings);
        pump(app, 3);
        scroll_page_down(window, app);
        png = path_join(shots, "18c-Settings-update-no-curl.png");
        app.frame(png.c_str());
    }
    set_env(ENV_FAKE_CURL, self_exe());
    // A cut answer: the check failed, no prompt.
    write(release, release_json("0.6.0").substr(0, 300));
    {
        App app(window, renderer, location, o);
        app.set_screen(Screen::Play);
        wait_check(app);
        check(app.update_check().state() == UpdateCheck::State::Failed && !app.update_prompt(),
              "a malformed answer: the check failed: " + app.update_check().summary());
    }
    // A development build: the latest is reported, never offered.
    write(release, release_json("0.6.0"));
    {
        AppOptions dev = o;
        dev.version = "dev-abc1234";
        App app(window, renderer, location, dev);
        app.set_screen(Screen::Play);
        wait_check(app);
        check(app.update_check().state() == UpdateCheck::State::DevBuild && !app.update_prompt() &&
                  app.update_check().summary().find("development build") != std::string::npos,
              "a development build: the latest reported, no prompt: " + app.update_check().summary());
    }
    // No repository in the description: no check, nothing of it on the screens.
    {
        AppOptions none = o;
        none.update_repo = "";
        App app(window, renderer, location, none);
        app.set_screen(Screen::Settings);
        pump(app, 3);
        scroll_page_down(window, app);
        check(!app.update_check().available() && app.update_check().state() == UpdateCheck::State::Idle &&
                  !app.update_prompt(),
              "no repository: no update check");
        png = path_join(shots, "18d-Settings-no-repo.png");
        app.frame(png.c_str());
        check(app.last_error().empty(), "no error in the status bar: " + app.last_error());
    }
    set_env(ENV_SELFTEST_CURL_REPLY, "offline");
}

// GitHub's answer for the flow below: release v<version> whose package (this platform's name) is `file`, with `digest`
// as the asset's (a JSON value: "\"sha256:...\"" or "null").
static std::string flow_release_json(const std::string &version, const std::string &file, const std::string &digest) {
    const std::string name = update_asset_name(version);
    SDL_PathInfo info;
    const long long size = SDL_GetPathInfo(file.c_str(), &info) ? (long long)info.size : -1;
    return "{\"tag_name\":\"v" + version + "\",\"name\":\"game v" + version +
           "\",\"html_url\":\"https://github.com/owner/game/releases/tag/v" + version + "\",\"assets\":[{\"name\":\"" +
           name + "\",\"size\":" + std::to_string(size) + ",\"digest\":" + digest +
           ",\"browser_download_url\":\"https://github.com/owner/game/releases/download/v" + version + "/" + name +
           "\"}]}";
}

static std::string flow_digest(const std::string &file) {
    std::string err;
    return "\"sha256:" + update_sha256_file(file, &err) + "\"";
}

static bool ends_with_text(const std::string &s, const std::string &tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Update now in the window (docs/LAUNCHER.md "Updates"), with curl's stand-in "downloading" fixture packages:
// the line instead of the button when the plan is not possible, the cancel, a wrong digest and a missing one (nothing
// applied, the message shown, the prompt still usable), then the whole flow into a fake AppImage install (Enter: the
// default button) and into a fake Windows folder, each ending with the new launcher started and this one quitting;
// and the relaunched launcher (its stamp the release's) is not prompted again.
static void test_update_flow(SDL_Window *window, const std::string &root) {
    const std::string files = path_join(root, "update-flow"), shots = path_join(root, "screens");
    path_make_dir(files, nullptr);
    const std::string release = path_join(files, "release.json"), settings_dir = path_join(files, "settings");
    const std::string images = path_join(files, "images"), image = path_join(images, "Game-0.4.1-x86_64.AppImage");
    const std::string new_image = path_join(files, "new.AppImage");
    path_make_dir(images, nullptr);
    check(self_test_copy_self(image, "") && self_test_copy_self(new_image, SELF_TEST_NEW_MARK),
          "the fake AppImage install and the release's image (copies of this executable)");
    const std::string old_image = read(image);
    set_env(ENV_FAKE_CURL, self_exe());
    set_env(ENV_SELFTEST_CURL_REPLY, release);
    SettingsDir location;
    location.dir = settings_dir;
    location.source = DirSource::Argument;
    std::vector<std::string> opened;
    AppOptions o;
    o.update_repo = "owner/game";
    o.version = "0.4.1";
    o.echo_game = false;
    o.open_url = [&](const std::string &u) {
        opened.push_back(u);
        return true;
    };
    SDL_Renderer *renderer = SDL_GetRenderer(window);
    const auto start = [&](App &app) {
        app.set_screen(Screen::Play);
        pump_until(app, [&] { return !app.update_check().checking(); }, 20000);
        pump(app, 3);
    };
    const auto settle = [&](App &app) {
        pump_until(app,
                   [&] {
                       return app.update_step() != UpdateStep::Downloading &&
                              app.update_step() != UpdateStep::Installing;
                   },
                   60000);
        pump(app, 2);
    };
    const std::string download = path_join(settings_dir, std::string(GAME_ID) + "-update.AppImage");
    std::string png;

    // Not possible here (no $APPIMAGE): the prompt keeps Release page and says why in a line.
    {
        AppOptions none = o;
        none.update_system = [] {
            UpdateSystem sys;
            sys.kind = UpdateKind::AppImage;
            return sys;
        };
        write(release, flow_release_json("0.5.0", new_image, flow_digest(new_image)));
        App app(window, renderer, location, none);
        start(app);
        check(app.update_prompt_shown() && !app.update_now_offered() &&
                  app.update_now_unavailable().find("AppImage") != std::string::npos,
              "a build that cannot update itself: no Update now, the plan's reason: " + app.update_now_unavailable());
        png = path_join(shots, "19-Play-update-cannot.png");
        app.frame(png.c_str());
        app.update_answer(UpdateAnswer::Later);
    }

    // The fake AppImage install: $APPIMAGE (update_system() reads it on Linux; on Windows the plan is given).
    set_env("APPIMAGE", image);
    AppOptions img = o;
#ifdef SDL_PLATFORM_WINDOWS
    img.update_system = [image] {
        UpdateSystem sys;
        sys.kind = UpdateKind::AppImage;
        sys.appimage = image;
        return sys;
    };
#endif
    write(release, flow_release_json("0.5.0", new_image, flow_digest(new_image)));

    // The cancel: a slow download stopped halfway; its file removed, the prompt closed as Later, nothing changed.
    set_env(ENV_SELFTEST_CURL_DOWNLOAD, "slow:" + new_image);
    {
        App app(window, renderer, location, img);
        start(app);
        check(app.update_prompt_shown() && app.update_now_offered() && app.update_plan().download == download,
              "an AppImage install: Update now offered, the download into the settings directory: " +
                  app.update_now_unavailable());
        png = path_join(shots, "19b-Play-update-now.png");
        app.frame(png.c_str());
        app.update_answer(UpdateAnswer::UpdateNow);
        check(app.update_step() == UpdateStep::Downloading, "Update now starts the download");
        pump_until(app, [&] { return app.update_download().progress() > 0.25f; }, 20000);
        app.update_check_now();
        check(!app.update_check().checking(), "no update check during a download");
        pump(app, 2);
        png = path_join(shots, "19c-Play-update-download.png");
        app.frame(png.c_str());
        check(app.update_step() == UpdateStep::Downloading && app.update_download().progress() > 0.0f &&
                  app.update_download().progress() < 1.0f,
              "the download's progress is between 0 and 1");
        push_key(window, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, true);
        pump(app, 1);
        push_key(window, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, false);
        pump(app, 3);
        check(app.update_step() == UpdateStep::None && !app.update_prompt() && !app.update_prompt_shown() &&
                  !path_is_file(download) && read(image) == old_image && !app.quit_requested(),
              "Cancel (Escape): the download stopped and removed, the prompt closed, the install unchanged");
        app.set_screen(Screen::Settings); // the Settings screen's Update now
        pump(app, 3);
        scroll_page_down(window, app);
        png = path_join(shots, "19e-Settings-update-now.png");
        app.frame(png.c_str());
    }
    set_env(ENV_SELFTEST_CURL_DOWNLOAD, new_image);

    // A wrong digest: downloaded, refused, removed; the message in the prompt with Release page and Close.
    write(release, flow_release_json("0.5.0", new_image, "\"sha256:" + std::string(64, '0') + "\""));
    {
        App app(window, renderer, location, img);
        start(app);
        app.update_answer(UpdateAnswer::UpdateNow);
        settle(app);
        check(app.update_step() == UpdateStep::Failed &&
                  app.update_message().find("did not pass its check") != std::string::npos &&
                  app.update_prompt_shown() && read(image) == old_image && !path_is_file(download) &&
                  !app.quit_requested(),
              "a wrong digest: nothing applied, the download removed, the message shown: " + app.update_message());
        png = path_join(shots, "19d-Play-update-failed.png");
        app.frame(png.c_str());
        opened.clear();
        app.update_answer(UpdateAnswer::ReleasePage);
        pump(app, 3);
        check(!app.update_prompt_shown() && app.update_step() == UpdateStep::None && opened.size() == 1,
              "after a failure, Release page still opens the release's page");
    }
    // No digest from GitHub: refused before anything is downloaded.
    write(release, flow_release_json("0.5.0", new_image, "null"));
    {
        App app(window, renderer, location, img);
        start(app);
        app.update_answer(UpdateAnswer::UpdateNow);
        check(app.update_step() == UpdateStep::Failed &&
                  app.update_message().find("no SHA-256") != std::string::npos && !path_is_file(download) &&
                  read(image) == old_image,
              "a package without a digest: nothing downloaded or applied: " + app.update_message());
        push_key(window, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, true);
        pump(app, 1);
        push_key(window, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, false);
        pump(app, 3);
        check(!app.update_prompt_shown() && app.update_step() == UpdateStep::None, "Escape is Close");
    }

    // The whole flow into the AppImage: Enter (Update now is the default), downloaded, verified, swapped, the new
    // image started with the launcher's arguments, this one quitting.
    write(release, flow_release_json("0.5.0", new_image, flow_digest(new_image)));
    const std::string marker = path_join(files, "appimage-relaunched.txt");
    set_fake_mode(SELF_TEST_UPDATE_MODE); // the new image is this executable: it writes the marker
    {
        AppOptions run = img;
        run.args = { "relaunched", marker };
        App app(window, renderer, location, run);
        start(app);
        push_key(window, SDL_SCANCODE_RETURN, SDL_KMOD_NONE, true);
        pump(app, 1);
        push_key(window, SDL_SCANCODE_RETURN, SDL_KMOD_NONE, false);
        pump(app, 1);
        check(app.update_step() != UpdateStep::None, "Enter is Update now (the default button)");
        settle(app);
        check(app.quit_requested() && ends_with_text(read(image), SELF_TEST_NEW_MARK) && !path_is_file(download),
              "the AppImage updated: the new image in place, the download gone, the launcher quits: " +
                  app.update_message());
    }
    std::string said;
    for (int t = 0; t < 20000 && said.empty(); t += 50) {
        said = read(marker);
        SDL_Delay(50);
    }
    check(said.find("relaunched") != std::string::npos, "the new AppImage was started with the arguments: " + said);
    SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "APPIMAGE");
    // The relaunched launcher: its stamp is the release it installed, so the check finds nothing newer.
    {
        AppOptions after = o;
        after.version = "0.5.0";
        App app(window, renderer, location, after);
        start(app);
        check(app.update_check().state() == UpdateCheck::State::UpToDate && !app.update_prompt(),
              "the relaunched launcher (stamp 0.5.0) is not prompted again: " + app.update_check().summary());
    }

    // A fake Windows folder: the release's zip (made with the self-test's tar) unpacked over it, the running
    // launcher's file renamed to .old, the new launcher started; it removes the .old at its start.
    const std::string install = path_join(files, "install"), package = path_join(files, "package");
    const std::string top = std::string(GAME_ID) + "-0.5.0-windows-x86_64", zip = path_join(files, "release.zip");
    path_make_dir(install, nullptr);
    path_make_dir(path_join(package, top), nullptr);
    write(path_join(install, GAME_EXE), "old game");
    write(path_join(install, "notes.txt"), "the user's notes");
    write(path_join(path_join(package, top), GAME_EXE), "new game");
    write(path_join(path_join(package, top), "README.txt"), "new readme");
    const std::string tar = self_test_tar();
    check(self_test_copy_self(path_join(install, LAUNCHER_EXE), "") &&
              self_test_copy_self(path_join(path_join(package, top), LAUNCHER_EXE), SELF_TEST_NEW_MARK) &&
              self_test_make_zip(tar, package, top, zip),
          "the fake Windows folder and the release's zip, made with " + tar);
    write(release, flow_release_json("0.5.0", zip, flow_digest(zip)));
    set_env(ENV_SELFTEST_CURL_DOWNLOAD, zip);
    const std::string marker2 = path_join(files, "folder-relaunched.txt");
    {
        AppOptions win = o;
        win.update_system = [install, tar] {
            UpdateSystem sys;
            sys.kind = UpdateKind::WindowsFolder;
            sys.install_dir = install;
            sys.tar = tar;
            return sys;
        };
        win.args = { "relaunched", marker2 };
        App app(window, renderer, location, win);
        start(app);
        check(app.update_now_offered() && app.update_plan().kind == UpdateKind::WindowsFolder,
              "a Windows folder: Update now offered: " + app.update_now_unavailable());
        app.update_answer(UpdateAnswer::UpdateNow);
        settle(app);
        check(app.quit_requested() && ends_with_text(read(path_join(install, LAUNCHER_EXE)), SELF_TEST_NEW_MARK) &&
                  read(path_join(install, GAME_EXE)) == "new game" &&
                  read(path_join(install, "notes.txt")) == "the user's notes" &&
                  !path_is_file(app.update_plan().download) && !path_is_dir(app.update_plan().staging),
              "the folder updated: the new launcher and game, the user's file kept, no download, no staging: " +
                  app.update_message());
    }
    said.clear();
    for (int t = 0; t < 30000 && said.empty(); t += 50) {
        said = read(marker2);
        SDL_Delay(50);
    }
    check(said.find("relaunched") != std::string::npos && said.find("old gone") != std::string::npos,
          "the folder's new launcher was started and removed the .old: " + said);
    SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), SELF_TEST_GAME_ENV);
    SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), ENV_SELFTEST_CURL_DOWNLOAD);
    set_env(ENV_SELFTEST_CURL_REPLY, "offline");
}

static void test_window(const std::string &root) {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    std::string err;
    if (!gui_open(&window, &renderer, &err)) {
        check(false, err);
        return;
    }
    std::fprintf(stderr, "self-test: video driver %s, renderer %s\n", SDL_GetCurrentVideoDriver(),
                 SDL_GetRendererName(renderer));
    const std::string shots = path_join(root, "screens");
    path_make_dir(shots, nullptr);
    {
        SettingsDir location;
        location.dir = path_join(root, "ui");
        location.source = DirSource::Argument;
        App app(window, renderer, location);
        check(app.screen() == Screen::Disc, "a first run starts on the disc screen");
        pump(app, 3);

        // Ctrl+PageDown through every screen (a picture of each), back to the start; Ctrl+PageUp one back.
        const Screen first = app.screen();
        const int n = (int)Screen::Count;
        for (int i = 0; i < n; i++) {
            Screen want = (Screen)(((int)first + i) % n);
            check(app.screen() == want, std::string("Ctrl+PageDown reaches ") + screen_name(want));
            pump(app, 2);
            std::string png = path_join(shots, std::to_string((int)app.screen()) + "-" + screen_name(app.screen()) +
                                                   ".png");
            app.frame(png.c_str());
            check(path_is_file(png), "a picture of " + std::string(screen_name(app.screen())));
            push_key(window, SDL_SCANCODE_PAGEDOWN, SDL_KMOD_LCTRL, true);
            pump(app, 1);
            push_key(window, SDL_SCANCODE_PAGEDOWN, SDL_KMOD_LCTRL, false);
            pump(app, 2);
        }
        check(app.screen() == first, "Ctrl+PageDown wraps around");
        push_key(window, SDL_SCANCODE_PAGEUP, SDL_KMOD_LCTRL, true);
        pump(app, 1);
        push_key(window, SDL_SCANCODE_PAGEUP, SDL_KMOD_LCTRL, false);
        pump(app, 2);
        check(app.screen() == (Screen)(((int)first + n - 1) % n), "Ctrl+PageUp goes back");
        // PageDown without Ctrl does not change the screen.
        Screen here = app.screen();
        push_key(window, SDL_SCANCODE_PAGEDOWN, SDL_KMOD_NONE, true);
        pump(app, 1);
        push_key(window, SDL_SCANCODE_PAGEDOWN, SDL_KMOD_NONE, false);
        pump(app, 2);
        check(app.screen() == here, "PageDown alone keeps the screen");

        // A virtual gamepad: R1 next, L1 back.
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.name = "psxstack launcher self-test";
        SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        SDL_Joystick *joy = id != 0 ? SDL_OpenJoystick(id) : nullptr;
        check(joy != nullptr, std::string("a virtual gamepad: ") + SDL_GetError());
        if (joy != nullptr) {
            pump(app, 4); // the backend opens it on SDL_EVENT_GAMEPAD_ADDED
            here = app.screen();
            SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
            pump(app, 2);
            SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
            pump(app, 2);
            check(app.screen() == (Screen)(((int)here + 1) % n), "the gamepad's R1 goes to the next screen");
            SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
            pump(app, 2);
            SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
            pump(app, 2);
            check(app.screen() == here, "the gamepad's L1 goes back");
            SDL_CloseJoystick(joy);
            SDL_DetachVirtualJoystick(id);
            pump(app, 2);
        }

        // The window's close button ends the loop; nothing was changed, so nothing was written.
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        e.window.windowID = SDL_GetWindowID(window);
        SDL_PushEvent(&e);
        pump(app, 1);
        check(app.quit_requested(), "closing the window quits");
        app.flush();
        check(!path_is_file(path_join(location.dir, SETTINGS_FILE)), "an unchanged first run writes no file");
        check(app.last_error().empty(), "no error in the status bar: " + app.last_error());
    }
    test_play(window, root);
    test_mods_window(window, root);
    test_update_window(window, root);
    test_update_flow(window, root);
    gui_close(window, renderer);
}

bool self_test_run(const std::string &dir) {
    std::string err;
    // Absolute: the lookup resolves a relative --config-dir against the current directory, and the game's stand-in
    // runs in another one.
    std::string base = path_strip_slash(dir);
    if (!path_is_absolute(base)) {
        if (char *cwd = SDL_GetCurrentDirectory()) {
            base = path_join(cwd, base);
            SDL_free(cwd);
        }
    }
    const std::string root = path_join(base, "launcher-self-test");
    // The video driver for the windowed part, when SDL_VIDEO_DRIVER cannot reach the program: Wine drops that
    // variable from the Windows environment (Proton and wine-staging do), so the Windows self-test is run with
    // <PREFIX>_SELFTEST_VIDEO_DRIVER=offscreen instead (the Windows self-test under Wine, CI).
    if (const char *driver = SDL_getenv(ENV_SELFTEST_VIDEO_DRIVER)) {
        if (*driver != '\0') {
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, driver);
        }
    }
    // Never the network: every update check of the self-test asks curl's stand-in (this executable), which answers
    // "could not resolve host" unless a test gives it an answer.
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), ENV_FAKE_CURL, self_exe().c_str(), true);
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), ENV_SELFTEST_CURL_REPLY, "offline", true);
    remove_tree(root);
    if (!path_make_dir(root, &err)) {
        std::fprintf(stderr, "self-test: %s\n", err.c_str());
        return false;
    }
    test_paths();
    test_json();
    test_lookup(path_join(root, "lookup"));
    test_settings_file(path_join(root, "files"));
    test_input(path_join(root, "controls"));
    test_mods(path_join(root, "mods"));
    test_disc(path_join(root, "disc"));
    test_game(path_join(root, "game"));
    test_update_rules();
    test_update_settings(path_join(root, "update-settings"));
    test_http(path_join(root, "http"));
    self_test_update(path_join(root, "update"), &checks, &failures);
    test_window(root);
    std::fprintf(stderr, "self-test: %d of %d checks passed; pictures in %s\n", checks - failures, checks,
                 path_join(root, "screens").c_str());
    return failures == 0;
}

} // namespace psxstack
