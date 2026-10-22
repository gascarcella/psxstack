#include "steam.h"

#include <algorithm>
#include <climits>
#include <cstdlib>

#include <SDL3/SDL.h>

#ifdef SDL_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#else
#include <cstdio>
#include <cstring>
#include <unistd.h>
#endif

#include "paths.h"

namespace psxstack {

// ---- The shortcut

std::string steam_quote(const std::string &path) {
    return "\"" + path + "\"";
}

static uint32_t crc32(const std::string &s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) {
        c ^= b;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        }
    }
    return ~c;
}

uint32_t steam_shortcut_appid(const std::string &exe, const std::string &name) {
    return crc32(steam_quote(exe) + name) | 0x80000000u;
}

VdfNode steam_shortcut_entry(const SteamShortcut &s) {
    // The start folder with its trailing separator, as Steam writes it.
    std::string start = path_dir(s.exe);
    if (!start.empty() && start.back() != '/' && start.back() != '\\') {
        start += s.exe.find('\\') != std::string::npos && s.exe.find('/') == std::string::npos ? '\\' : '/';
    }
    VdfNode e = VdfNode::map("");
    e.children = {
        VdfNode::int32("appid", (int32_t)steam_shortcut_appid(s.exe, s.name)),
        VdfNode::string("AppName", s.name),
        VdfNode::string("Exe", steam_quote(s.exe)),
        VdfNode::string("StartDir", steam_quote(start)),
        VdfNode::string("icon", s.icon),
        VdfNode::string("ShortcutPath", ""),
        VdfNode::string("LaunchOptions", s.launch_options),
        VdfNode::int32("IsHidden", 0),
        VdfNode::int32("AllowDesktopConfig", 1),
        VdfNode::int32("AllowOverlay", 1),
        VdfNode::int32("OpenVR", 0),
        VdfNode::int32("Devkit", 0),
        VdfNode::string("DevkitGameID", ""),
        VdfNode::int32("DevkitOverrideAppID", 0),
        VdfNode::int32("LastPlayTime", 0),
        VdfNode::string("FlatpakAppID", ""),
        VdfNode::string("sortas", ""),
        VdfNode::map("tags"),
    };
    for (size_t i = 0; i < s.tags.size(); i++) {
        e.children.back().children.push_back(VdfNode::string(std::to_string(i), s.tags[i]));
    }
    return e;
}

VdfNode *steam_shortcuts(VdfNode *root) {
    VdfNode *list = root->find("shortcuts");
    if (list == nullptr || list->type != VdfNode::Type::Map) {
        root->children.push_back(VdfNode::map("shortcuts"));
        list = &root->children.back();
    }
    return list;
}

static uint32_t entry_appid(const VdfNode &e) {
    const VdfNode *id = e.find("appid");
    return id != nullptr && id->type == VdfNode::Type::Int32 ? (uint32_t)id->as_int32() : 0;
}

static void renumber(VdfNode *list) {
    for (size_t i = 0; i < list->children.size(); i++) {
        list->children[i].key = std::to_string(i);
    }
}

uint32_t steam_shortcuts_add(VdfNode *root, const SteamShortcut &s) {
    VdfNode *list = steam_shortcuts(root);
    VdfNode e = steam_shortcut_entry(s);
    const uint32_t appid = entry_appid(e);
    auto same = std::find_if(list->children.begin(), list->children.end(),
                             [&](const VdfNode &n) { return entry_appid(n) == appid; });
    if (same != list->children.end()) {
        *same = std::move(e);
    } else {
        list->children.push_back(std::move(e));
    }
    renumber(list);
    return appid;
}

int steam_shortcuts_remove(VdfNode *root, uint32_t appid) {
    VdfNode *list = steam_shortcuts(root);
    const size_t before = list->children.size();
    list->children.erase(std::remove_if(list->children.begin(), list->children.end(),
                                        [&](const VdfNode &n) { return entry_appid(n) == appid; }),
                         list->children.end());
    renumber(list);
    return (int)(before - list->children.size());
}

const VdfNode *steam_shortcuts_find(const VdfNode &root, uint32_t appid) {
    const VdfNode *list = root.find("shortcuts");
    if (list == nullptr) {
        return nullptr;
    }
    for (const VdfNode &e : list->children) {
        if (entry_appid(e) == appid) {
            return &e;
        }
    }
    return nullptr;
}

bool steam_shortcuts_load(const std::string &path, VdfNode *root, std::string *err) {
    if (!path_is_file(path)) {
        *root = VdfNode::map("");
        steam_shortcuts(root);
        return true;
    }
    std::string bytes;
    if (!file_read(path, &bytes, err)) {
        return false;
    }
    if (!vdf_binary_read(bytes, root, err)) {
        *err = path + ": not a shortcuts file Steam wrote (" + *err + "); left as it is";
        return false;
    }
    return true;
}

bool steam_shortcuts_save(const std::string &path, const VdfNode &root, std::string *err) {
    const std::string backup = path + ".bak";
    if (path_is_file(path) && !path_is_file(backup)) {
        std::string old;
        if (!file_read(path, &old, err) || !file_write_atomic(backup, old, err)) {
            return false;
        }
    }
    if (!path_is_dir(path_dir(path)) && !path_make_dir(path_dir(path), err)) {
        return false;
    }
    return file_write_atomic(path, vdf_binary_write(root), err);
}

// ---- Where Steam is

int SteamInstall::default_account() const {
    for (size_t i = 0; i < accounts.size(); i++) {
        if (accounts[i].most_recent) {
            return (int)i;
        }
    }
    return accounts.empty() ? -1 : 0;
}

SteamLookup steam_lookup_linux(const std::string &home, const std::string &xdg_data) {
    SteamLookup in;
    if (home.empty()) {
        return in;
    }
    if (!xdg_data.empty()) {
        in.candidates.push_back({ path_join(xdg_data, "Steam"), "$XDG_DATA_HOME/Steam" });
    }
    in.candidates.push_back({ path_join(home, ".local/share/Steam"), "~/.local/share/Steam" });
    in.candidates.push_back({ path_join(home, ".steam/steam"), "~/.steam/steam" }); // usually a link to the one above
    in.candidates.push_back({ path_join(home, ".var/app/com.valvesoftware.Steam/.local/share/Steam"), "Flatpak" });
    return in;
}

SteamLookup steam_lookup_windows(const std::string &registry, const std::string &program_files_x86) {
    SteamLookup in;
    if (!registry.empty()) {
        in.candidates.push_back({ registry, "the registry" });
    }
    if (!program_files_x86.empty()) {
        in.candidates.push_back({ path_join(program_files_x86, "Steam"), "Program Files (x86)" });
    }
    return in;
}

SteamLookup steam_lookup_from_system() {
#ifdef SDL_PLATFORM_WINDOWS
    const char *pf = SDL_getenv("ProgramFiles(x86)");
    return steam_lookup_windows(windows_registry_string("Software\\Valve\\Steam", "SteamPath"),
                                pf != nullptr ? pf : "C:\\Program Files (x86)");
#else
    const char *home = SDL_getenv("HOME");
    const char *xdg = SDL_getenv("XDG_DATA_HOME");
    return steam_lookup_linux(home != nullptr ? home : "", xdg != nullptr ? xdg : "");
#endif
}

// The folder as one name for every way to reach it (links followed; Windows: one separator, one case).
static std::string same_dir_key(const std::string &dir) {
#ifdef SDL_PLATFORM_WINDOWS
    std::string k = path_strip_slash(dir);
    for (char &c : k) {
        c = c == '/' ? '\\' : (char)SDL_tolower((unsigned char)c);
    }
    return k;
#else
    char real[PATH_MAX];
    return realpath(dir.c_str(), real) != nullptr ? std::string(real) : path_strip_slash(dir);
#endif
}

std::vector<SteamInstall> steam_find(const SteamLookup &in) {
    std::vector<SteamInstall> found;
    std::vector<std::string> keys;
    for (const auto &c : in.candidates) {
        if (!path_is_dir(path_join(c.first, "userdata"))) {
            continue;
        }
        const std::string key = same_dir_key(c.first);
        if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
            continue;
        }
        keys.push_back(key);
        SteamInstall s;
        s.dir = path_strip_slash(c.first);
        s.where = c.second;
        s.accounts = steam_accounts(s.dir);
        found.push_back(std::move(s));
    }
    return found;
}

static SDL_EnumerationResult SDLCALL collect_ids(void *list, const char *, const char *name) {
    const std::string n = name;
    if (!n.empty() && n != "0" && n.find_first_not_of("0123456789") == std::string::npos && n.size() <= 10) {
        static_cast<std::vector<std::string> *>(list)->push_back(n);
    }
    return SDL_ENUM_CONTINUE;
}

std::vector<SteamAccount> steam_accounts(const std::string &steam_dir) {
    std::vector<std::string> ids;
    SDL_EnumerateDirectory(path_join(steam_dir, "userdata").c_str(), collect_ids, &ids);
    std::sort(ids.begin(), ids.end(), [](const std::string &a, const std::string &b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    // config/loginusers.vdf: users { "<SteamID64>" { "AccountName", "PersonaName", "MostRecent" } }; an account's
    // 32-bit id is its SteamID64 less the individual-account base.
    VdfNode users;
    std::string text, err;
    const bool named = file_read(path_join(steam_dir, "config/loginusers.vdf"), &text, &err) &&
                       vdf_text_read(text, &users, &err);
    const VdfNode *list = named ? users.find("users") : nullptr;
    std::vector<SteamAccount> accounts;
    for (const std::string &id : ids) {
        SteamAccount a;
        a.id = id;
        if (list != nullptr) {
            const std::string id64 = std::to_string(76561197960265728ull + std::strtoull(id.c_str(), nullptr, 10));
            if (const VdfNode *u = list->find(id64)) {
                a.name = u->get_string("PersonaName", u->get_string("AccountName"));
                a.most_recent = u->get_string("MostRecent") == "1";
            }
        }
        accounts.push_back(a);
    }
    return accounts;
}

std::string steam_shortcuts_path(const std::string &steam_dir, const std::string &account) {
    return path_join(path_join(path_join(steam_dir, "userdata"), account), "config/shortcuts.vdf");
}

// ---- This system

#ifdef SDL_PLATFORM_WINDOWS
static std::string utf8(const wchar_t *w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(n > 0 ? n : 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    out.resize(n > 0 ? (size_t)n - 1 : 0);
    return out;
}
#endif

bool process_running(const std::string &name) {
#ifdef SDL_PLATFORM_WINDOWS
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        found = SDL_strcasecmp(utf8(pe.szExeFile).c_str(), name.c_str()) == 0;
    }
    CloseHandle(snap);
    return found;
#else
    struct Find {
        std::string comm;
        bool found = false;
    } f;
    f.comm = name.substr(0, 15);
    SDL_EnumerateDirectory(
        "/proc",
        [](void *data, const char *dir, const char *pid) -> SDL_EnumerationResult {
            Find *f = static_cast<Find *>(data);
            if (pid[0] < '0' || pid[0] > '9') {
                return SDL_ENUM_CONTINUE;
            }
            char line[64] = "";
            if (FILE *c = std::fopen(path_join(path_join(dir, pid), "comm").c_str(), "r")) {
                if (std::fgets(line, sizeof(line), c) != nullptr) {
                    line[std::strcspn(line, "\n")] = '\0';
                }
                std::fclose(c);
            }
            f->found = f->comm == line;
            return f->found ? SDL_ENUM_SUCCESS : SDL_ENUM_CONTINUE;
        },
        &f);
    return f.found;
#endif
}

bool steam_running() {
#ifdef SDL_PLATFORM_WINDOWS
    return process_running("steam.exe");
#else
    return process_running("steam");
#endif
}

std::string steam_launcher_path() {
#ifdef SDL_PLATFORM_WINDOWS
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return n > 0 && n < sizeof(buf) / sizeof(buf[0]) ? utf8(buf) : "";
#else
    if (const char *image = SDL_getenv("APPIMAGE"); image != nullptr && *image != '\0') {
        return image;
    }
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, (size_t)n) : "";
#endif
}

std::string windows_registry_string(const std::string &subkey, const std::string &value) {
#ifdef SDL_PLATFORM_WINDOWS
    auto wide = [](const std::string &s) {
        std::wstring w(s.size() + 1, L'\0');
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], (int)w.size());
        w.resize(n > 0 ? (size_t)n - 1 : 0);
        return w;
    };
    const std::wstring k = wide(subkey), v = wide(value);
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, k.c_str(), v.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        bytes == 0) {
        return "";
    }
    std::wstring w(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, k.c_str(), v.c_str(), RRF_RT_REG_SZ, nullptr, &w[0], &bytes) !=
        ERROR_SUCCESS) {
        return "";
    }
    return utf8(w.c_str());
#else
    (void)subkey;
    (void)value;
    return "";
#endif
}

} // namespace psxstack
