// The self-test of the Steam shortcut's files (steam.h, vdf.h; launcher/README.md "The self-test"), called by
// self_test_run on fixture folders under its own: the binary VDF read and written back byte for byte
// (tests/fixtures/steam/shortcuts.vdf, written by make_fixtures.py, an encoder of its own), the launcher's shortcut
// added and removed against the bytes that script expects (shortcuts_added.vdf), damaged files refused, the save's
// backup, the text VDF of loginusers.vdf, and Steam's folders and accounts found in a fixture home. It never looks at
// this system's Steam: steam_lookup_from_system is not called. On Windows, the registry reader is checked on a value
// of its own, written under HKEY_CURRENT_USER\Software\<id>-launcher-self-test and removed.
#include "steam.h"

#include <cstdio>
#include <string>

#include <SDL3/SDL.h>

#ifdef SDL_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h> // symlink: ~/.steam/steam as a link to ~/.local/share/Steam
#endif

#include "brand.h"
#include "paths.h"
#include "selftest_fixtures.h"

namespace psxstack {

static int *n_checks, *n_failures;

static void check(bool ok, const std::string &what) {
    (*n_checks)++;
    if (!ok) {
        (*n_failures)++;
        std::fprintf(stderr, "self-test: FAILED: steam: %s\n", what.c_str());
    }
}

static std::string bytes(const unsigned char *data, unsigned size) {
    return std::string(reinterpret_cast<const char *>(data), size);
}

static bool put(const std::string &path, const std::string &data) {
    std::string err;
    return path_make_dir(path_dir(path), &err) && SDL_SaveFile(path.c_str(), data.data(), data.size());
}

static std::string get(const std::string &path) {
    std::string text, err;
    file_read(path, &text, &err);
    return text;
}

// The shortcut make_fixtures.py's ADDED is: the launcher's entry for this executable and title.
static const char ADDED_EXE[] = "/home/player/Games/a-game.AppImage";
static const char ADDED_NAME[] = "A Game";
static const uint32_t ADDED_APPID = 0xEEEDDD90u; // zlib.crc32(b'"/home/player/Games/a-game.AppImage"A Game') | 1 << 31

static void test_binary() {
    const std::string fixture = bytes(STEAM_SHORTCUTS_VDF, STEAM_SHORTCUTS_VDF_SIZE);
    const std::string added = bytes(STEAM_SHORTCUTS_ADDED_VDF, STEAM_SHORTCUTS_ADDED_VDF_SIZE);
    VdfNode root;
    std::string err;
    check(vdf_binary_read(fixture, &root, &err), "the fixture shortcuts.vdf reads: " + err);
    check(vdf_binary_write(root) == fixture, "shortcuts.vdf is written back byte for byte");
    const VdfNode *list = root.find("shortcuts");
    check(list != nullptr && list->children.size() == 2 && list->children[0].key == "0" &&
              list->children[1].key == "1",
          "two shortcuts, numbered 0 and 1");
    if (list == nullptr || list->children.size() != 2) {
        return;
    }
    const VdfNode emu = list->children[0], jeu = list->children[1]; // copies: the list changes below
    const uint32_t emu_appid = (uint32_t)emu.find("appid")->as_int32();
    const VdfNode *tags = emu.find("tags");
    check(emu.get_string("AppName") == "An Emulator" && emu.get_string("exe") == "\"/opt/emulator/bin/emulator\"" &&
              emu.get_string("LaunchOptions") == "--fullscreen \"%command%\"" && tags != nullptr &&
              tags->children.size() == 2 && tags->get_string("1") == "Favorites" &&
              emu.find("LastPlayTime")->as_int32() == 1760000000,
          "the first shortcut's fields (keys found case-insensitively)");
    check(jeu.get_string("AppName") == "Jeu \xC3\xA0 essayer" && jeu.find("AllowOverlay")->as_int32() == 0,
          "the second shortcut's UTF-8 name and its overlay off");
    check(steam_shortcut_appid("/opt/emulator/bin/emulator", "An Emulator") == emu_appid &&
              steam_shortcut_appid(ADDED_EXE, ADDED_NAME) == ADDED_APPID,
          "the appid is CRC-32 of the quoted exe and the name, top bit set (as zlib computes it)");

    // The launcher's shortcut added: the file make_fixtures.py expects, byte for byte; again: replaced, not doubled;
    // removed: the fixture again; removed once more: nothing.
    SteamShortcut s;
    s.exe = ADDED_EXE;
    s.name = ADDED_NAME;
    check(steam_shortcuts_add(&root, s) == ADDED_APPID && vdf_binary_write(root) == added,
          "adding the shortcut gives shortcuts_added.vdf byte for byte (Steam's fields, order and defaults)");
    steam_shortcuts_add(&root, s);
    check(vdf_binary_write(root) == added, "adding it again replaces it: one entry per appid");
    check(steam_shortcuts_find(root, ADDED_APPID) != nullptr && steam_shortcuts_find(root, 0x80000001u) == nullptr,
          "the shortcut is found by its appid");
    check(steam_shortcuts_remove(&root, ADDED_APPID) == 1 && vdf_binary_write(root) == fixture,
          "removing it gives the fixture back byte for byte");
    check(steam_shortcuts_remove(&root, ADDED_APPID) == 0 && vdf_binary_write(root) == fixture,
          "removing it again removes nothing");
    // Another entry removed: the rest numbered again, each unchanged.
    VdfNode two;
    vdf_binary_read(added, &two, &err);
    VdfNode jeu_alone = two.find("shortcuts")->children[1];
    check(steam_shortcuts_remove(&two, emu_appid) == 1, "the first shortcut removed");
    const VdfNode *left = two.find("shortcuts");
    jeu_alone.key = "0";
    VdfNode one = VdfNode::map(""), expect = VdfNode::map("");
    one.children.push_back(left->children[0]);
    expect.children.push_back(jeu_alone);
    check(left->children.size() == 2 && left->children[0].key == "0" && left->children[1].key == "1" &&
              vdf_binary_write(one) == vdf_binary_write(expect),
          "the others are numbered 0 and 1 again, their fields unchanged");

    // An empty file as Steam writes one; damaged ones refused.
    VdfNode empty = VdfNode::map("");
    steam_shortcuts(&empty);
    check(vdf_binary_write(empty) == std::string("\0shortcuts\0\x08\x08", 13), "no shortcuts: Steam's empty file");
    std::string bad = fixture;
    bad.pop_back();
    check(!vdf_binary_read(bad, &root, &err) && err.find("byte") != std::string::npos, "a truncated file: " + err);
    bad = fixture + "x";
    check(!vdf_binary_read(bad, &root, &err), "bytes after the end: " + err);
    bad = fixture;
    bad[11] = '\x05'; // the first entry's type byte: a wide string, which this reader does not keep
    check(!vdf_binary_read(bad, &root, &err) && err.find("type") != std::string::npos, "an unknown type: " + err);
    check(!vdf_binary_read("", &root, &err), "an empty file: " + err);
    // The other number types are kept as their bytes.
    const std::string numbers("\x03" "f\0" "\x00\x00\x80\x3F" "\x07" "u\0" "\x01\x02\x03\x04\x05\x06\x07\x08"
                              "\x0A" "i\0" "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF" "\x08",
                              30);
    check(vdf_binary_read(numbers, &root, &err) && vdf_binary_write(root) == numbers &&
              root.find("u")->raw == 0x0807060504030201ull,
          "float32, uint64 and int64 values round-trip: " + err);
}

static void test_files(const std::string &dir) {
    const std::string fixture = bytes(STEAM_SHORTCUTS_VDF, STEAM_SHORTCUTS_VDF_SIZE);
    const std::string path = path_join(dir, "userdata/100/config/shortcuts.vdf");
    VdfNode root;
    std::string err;
    // No file yet (an account that never had a shortcut): an empty list; saved: no backup of nothing.
    check(steam_shortcuts_load(path, &root, &err) && steam_shortcuts(&root)->children.empty(),
          "a missing shortcuts.vdf is an empty one: " + err);
    SteamShortcut s;
    s.exe = ADDED_EXE;
    s.name = ADDED_NAME;
    steam_shortcuts_add(&root, s);
    check(steam_shortcuts_save(path, root, &err) && !path_is_file(path + ".bak"),
          "saving creates the file and its folder, with no backup: " + err);
    // An existing file: backed up once, as it was.
    check(put(path, fixture), "the fixture written");
    check(steam_shortcuts_load(path, &root, &err), "the fixture loads: " + err);
    steam_shortcuts_add(&root, s);
    check(steam_shortcuts_save(path, root, &err) && get(path + ".bak") == fixture &&
              get(path) == bytes(STEAM_SHORTCUTS_ADDED_VDF, STEAM_SHORTCUTS_ADDED_VDF_SIZE),
          "the first save keeps shortcuts.vdf.bak as it was: " + err);
    steam_shortcuts_remove(&root, ADDED_APPID);
    check(steam_shortcuts_save(path, root, &err) && get(path) == fixture &&
              get(path + ".bak") == fixture && !path_is_file(path + ".tmp"),
          "a later save leaves the backup alone: " + err);
    // A file that does not read is refused and left as it is.
    check(put(path, "not a vdf"), "a damaged file written");
    check(!steam_shortcuts_load(path, &root, &err) && err.find("left as it is") != std::string::npos &&
              get(path) == "not a vdf",
          "a damaged shortcuts.vdf is refused, untouched: " + err);
}

static void test_text() {
    VdfNode root;
    std::string err;
    const std::string text = bytes(STEAM_LOGINUSERS_VDF, STEAM_LOGINUSERS_VDF_SIZE);
    check(vdf_text_read(text, &root, &err), "loginusers.vdf reads: " + err);
    const VdfNode *users = root.find("users");
    check(users != nullptr && users->children.size() == 3 &&
              users->find("76561198000000222")->get_string("PersonaName") == "Second \"Quoted\" Player",
          "three users; an escaped quote");
    check(vdf_text_read("\xEF\xBB\xBF// c\nkey { \"k\" \"v\\tw\" // trailing\n bare word }", &root, &err) &&
              root.find("KEY") != nullptr && root.find("key")->get_string("k") == "v\tw" &&
              root.find("key")->get_string("bare") == "word",
          "a byte order mark, comments, bare words, escapes: " + err);
    check(!vdf_text_read("\"users\" { \"a\" \"b\"", &root, &err) && err.find("line 1") != std::string::npos,
          "a map without its closing brace: " + err);
    check(!vdf_text_read("\"a\" \"b\" }", &root, &err), "a closing brace too many: " + err);
    check(!vdf_text_read("\"a\" \"b", &root, &err), "a string without its quote: " + err);
}

static void test_find(const std::string &dir) {
    std::string err;
    const std::string home = path_join(dir, "home");
    const std::string steam = path_join(home, ".local/share/Steam");
    // Steam: two accounts named in loginusers.vdf (the second the most recent), one not, a third named there without
    // a userdata folder; "0" and non-numbers are not accounts.
    check(put(path_join(steam, "config/loginusers.vdf"), bytes(STEAM_LOGINUSERS_VDF, STEAM_LOGINUSERS_VDF_SIZE)) &&
              put(steam_shortcuts_path(steam, "39734383"), bytes(STEAM_SHORTCUTS_VDF, STEAM_SHORTCUTS_VDF_SIZE)) &&
              path_make_dir(path_join(steam, "userdata/39734494/config"), &err) &&
              path_make_dir(path_join(steam, "userdata/12345"), &err) &&
              path_make_dir(path_join(steam, "userdata/0"), &err) &&
              path_make_dir(path_join(steam, "userdata/anonymous"), &err),
          "the fixture Steam folder: " + err);
    // Flatpak's, with an account and no loginusers.vdf; a folder without userdata/ is not Steam.
    const std::string flatpak = path_join(home, ".var/app/com.valvesoftware.Steam/.local/share/Steam");
    check(path_make_dir(path_join(flatpak, "userdata/777"), &err) &&
              path_make_dir(path_join(dir, "xdg/Steam/config"), &err),
          "the fixture Flatpak folder: " + err);
#ifndef SDL_PLATFORM_WINDOWS
    check(path_make_dir(path_join(home, ".steam"), &err) &&
              symlink("../.local/share/Steam", path_join(home, ".steam/steam").c_str()) == 0,
          "~/.steam/steam a link to ~/.local/share/Steam");
#endif
    const SteamLookup lookup = steam_lookup_linux(home, path_join(dir, "xdg"));
    bool inside = true;
    for (const auto &c : lookup.candidates) {
        inside &= c.first.compare(0, dir.size(), dir) == 0;
    }
    check(lookup.candidates.size() == 4 && inside, "Linux's four places, all in the fixture home");
    const std::vector<SteamInstall> found = steam_find(lookup);
    check(found.size() == 2 && found[0].dir == steam && found[0].where == "~/.local/share/Steam" &&
              found[1].where == "Flatpak",
          "two Steam folders: ~/.local/share/Steam (its link ~/.steam/steam once) and Flatpak's");
    if (found.size() == 2) {
        const std::vector<SteamAccount> &a = found[0].accounts;
        check(a.size() == 3 && a[0].id == "12345" && a[0].name.empty() && a[1].id == "39734383" &&
                  a[1].name == "First Player" && !a[1].most_recent && a[2].id == "39734494" && a[2].most_recent,
              "the accounts in id order, named from loginusers.vdf, the most recent marked");
        check(found[0].default_account() == 2, "the most recent account is offered first");
        check(found[1].accounts.size() == 1 && found[1].accounts[0].name.empty() &&
                  found[1].default_account() == 0,
              "no loginusers.vdf: the account unnamed, the first offered");
    }
    check(steam_find(steam_lookup_linux("", "")).empty(), "no home: nothing to look in");
    // Windows' order: the registry's folder, then Program Files (x86)'s; the same folder twice counted once.
    const SteamLookup win = steam_lookup_windows(steam, path_join(home, ".local/share"));
    const std::vector<SteamInstall> w = steam_find(win);
    check(win.candidates.size() == 2 && w.size() == 1 && w[0].where == "the registry",
          "Windows: the registry's folder, Program Files' the same one");
    check(steam_find(steam_lookup_windows("", path_join(dir, "nowhere"))).empty(), "Windows: nothing found");
}

static void test_registry() {
#ifdef SDL_PLATFORM_WINDOWS
    const std::string key = std::string("Software\\") + GAME_ID + "-launcher-self-test";
    const std::wstring wkey(key.begin(), key.end());
    const wchar_t value[] = L"C:/Jeux/St\u00e9am";
    const LSTATUS set = RegSetKeyValueW(HKEY_CURRENT_USER, wkey.c_str(), L"SteamPath", REG_SZ, value, sizeof(value));
    check(set == ERROR_SUCCESS, "the registry: a value of the self-test's own written");
    check(windows_registry_string(key, "SteamPath") == "C:/Jeux/St\xC3\xA9" "am", "the registry's value read, as UTF-8");
    check(windows_registry_string(key, "Missing").empty(), "a missing value: empty");
    RegDeleteTreeW(HKEY_CURRENT_USER, wkey.c_str());
    check(windows_registry_string(key, "SteamPath").empty(), "the self-test's key removed");
#else
    check(windows_registry_string("Software\\Valve\\Steam", "SteamPath").empty(), "no registry off Windows");
#endif
}

void self_test_steam(const std::string &dir, int *checks, int *failures) {
    n_checks = checks;
    n_failures = failures;
    test_binary();
    test_files(path_join(dir, "files"));
    test_text();
    test_find(path_join(dir, "find"));
    test_registry();
}

} // namespace psxstack
