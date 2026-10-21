#include "update_apply.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>

#ifndef SDL_PLATFORM_WINDOWS
#include <sys/stat.h> // chmod: SDL has no call to make a file executable
#endif

#include "brand.h"
#include "paths.h"

extern "C" {
#include "sha256.h"
}

namespace psxstack {

#ifdef SDL_PLATFORM_WINDOWS
static const char PATH_LIST_SEP = ';';
#else
static const char PATH_LIST_SEP = ':';
#endif

// The staging folder in the install folder, and the name the running launcher is renamed to.
static std::string staging_dir(const std::string &install_dir) {
    return path_join(install_dir, std::string(GAME_ID) + "-update.tmp");
}

static std::string old_launcher(const std::string &install_dir) {
    return path_join(install_dir, std::string(LAUNCHER_EXE) + ".old");
}

// The temporary file beside an AppImage: hidden, the image's name.
static std::string appimage_staging(const std::string &image) {
    return path_join(path_dir(image), "." + path_base(image) + ".update");
}

static SDL_EnumerationResult remove_entry(void *, const char *dir, const char *name) {
    const std::string p = path_join(dir, name);
    if (path_is_dir(p)) {
        SDL_EnumerateDirectory(p.c_str(), remove_entry, nullptr);
    }
    SDL_RemovePath(p.c_str());
    return SDL_ENUM_CONTINUE;
}

// `p` and everything under it; true when nothing is left.
static bool remove_tree(const std::string &p) {
    if (path_is_dir(p)) {
        SDL_EnumerateDirectory(p.c_str(), remove_entry, nullptr);
    }
    SDL_RemovePath(p.c_str());
    SDL_PathInfo info;
    return !SDL_GetPathInfo(p.c_str(), &info);
}

static bool exists(const std::string &p) {
    SDL_PathInfo info;
    return !p.empty() && SDL_GetPathInfo(p.c_str(), &info);
}

// Whether a file can be created in `dir` (a file made and removed: the only portable answer).
static bool dir_writable(const std::string &dir, std::string *err) {
    const std::string probe = path_join(dir, "." + std::string(GAME_ID) + "-write-test.tmp");
    SDL_IOStream *io = SDL_IOFromFile(probe.c_str(), "wb");
    if (io == nullptr) {
        *err = SDL_GetError();
        return false;
    }
    SDL_CloseIO(io);
    SDL_RemovePath(probe.c_str());
    return true;
}

std::string update_find_program(const std::string &name, const std::string &path_list) {
    size_t at = 0;
    while (at <= path_list.size()) {
        size_t end = path_list.find(PATH_LIST_SEP, at);
        if (end == std::string::npos) {
            end = path_list.size();
        }
        std::string dir = path_list.substr(at, end - at);
        if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') {
            dir = dir.substr(1, dir.size() - 2); // Windows allows a quoted PATH entry
        }
        if (!dir.empty()) {
            const std::string p = path_join(dir, name);
            if (path_is_file(p)) {
                return p;
            }
        }
        at = end + 1;
    }
    return "";
}

UpdateSystem update_system() {
    UpdateSystem sys;
    const char *base = SDL_GetBasePath();
    sys.install_dir = path_strip_slash(base != nullptr ? base : "");
#ifdef SDL_PLATFORM_WINDOWS
    sys.kind = UpdateKind::WindowsFolder;
    if (const char *root = SDL_getenv("SystemRoot")) {
        const std::string system_tar = path_join(path_join(root, "System32"), "tar.exe");
        if (path_is_file(system_tar)) {
            sys.tar = system_tar;
        }
    }
    if (sys.tar.empty()) {
        const char *path = SDL_getenv("PATH");
        sys.tar = update_find_program("tar.exe", path != nullptr ? path : "");
    }
#else
    sys.kind = UpdateKind::AppImage;
    if (const char *image = SDL_getenv("APPIMAGE")) {
        sys.appimage = image;
    }
#endif
    return sys;
}

UpdatePlan update_plan(const UpdateSystem &sys, const std::string &download_dir) {
    UpdatePlan plan;
    std::string err;
    if (sys.kind == UpdateKind::AppImage) {
        if (sys.appimage.empty()) {
            plan.reason = "This launcher does not run from an AppImage (a development or unpacked build), so it cannot "
                          "update itself: get the new release from its page.";
            return plan;
        }
        if (!path_is_file(sys.appimage)) {
            plan.reason = "The AppImage this launcher runs from (" + sys.appimage + ") is not there any more.";
            return plan;
        }
        const std::string dir = path_dir(sys.appimage);
        if (!dir_writable(dir, &err)) {
            plan.reason =
                "The AppImage's folder (" + dir + ") is not writable, so the launcher cannot replace it: " + err;
            return plan;
        }
        plan.target = plan.launcher = sys.appimage;
        plan.staging = appimage_staging(sys.appimage);
        plan.download = path_join(download_dir, std::string(GAME_ID) + "-update.AppImage");
    } else if (sys.kind == UpdateKind::WindowsFolder) {
        const std::string launcher = path_join(sys.install_dir, LAUNCHER_EXE);
        if (!path_is_file(launcher)) {
            plan.reason = "The launcher's folder (" + sys.install_dir + ") has no " + LAUNCHER_EXE +
                          ", so it is not a release's folder: get the new release from its page.";
            return plan;
        }
        if (!dir_writable(sys.install_dir, &err)) {
            plan.reason = "The launcher's folder (" + sys.install_dir +
                          ") is not writable, so the launcher cannot update it (move the folder somewhere of yours, "
                          "or get the new release from its page): " + err;
            return plan;
        }
        if (sys.tar.empty() || !path_is_file(sys.tar)) {
            plan.reason = "Windows' tar.exe (Windows 10 1803 or later) was not found, so the release's zip cannot be "
                          "unpacked: get the new release from its page.";
            return plan;
        }
        plan.target = sys.install_dir;
        plan.launcher = launcher;
        plan.staging = staging_dir(sys.install_dir);
        plan.download = path_join(download_dir, std::string(GAME_ID) + "-update.zip");
        plan.tar = sys.tar;
    } else {
        plan.reason = "This launcher cannot update itself here: get the new release from its page.";
        return plan;
    }
    plan.kind = sys.kind;
    return plan;
}

std::string update_sha256_file(const std::string &file, std::string *err) {
    SDL_IOStream *io = SDL_IOFromFile(file.c_str(), "rb");
    if (io == nullptr) {
        *err = "cannot read " + file + ": " + SDL_GetError();
        return "";
    }
    PortSha256 c;
    port_sha256_init(&c);
    std::vector<unsigned char> buf(1 << 16);
    size_t n;
    while ((n = SDL_ReadIO(io, buf.data(), buf.size())) > 0) {
        port_sha256_update(&c, buf.data(), n);
    }
    const bool failed = SDL_GetIOStatus(io) == SDL_IO_STATUS_ERROR;
    SDL_CloseIO(io);
    if (failed) {
        *err = "cannot read " + file + ": " + SDL_GetError();
        return "";
    }
    uint8_t digest[32];
    char hex[65];
    port_sha256_final(&c, digest);
    port_sha256_hex(digest, hex);
    return hex;
}

bool update_verify(const std::string &file, const std::string &digest, std::string *err) {
    std::string want;
    if (digest.compare(0, 7, "sha256:") == 0) {
        want = digest.substr(7);
    }
    bool hex = want.size() == 64;
    for (char &ch : want) {
        ch = (char)SDL_tolower((unsigned char)ch);
        hex &= (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    }
    if (!hex) {
        *err = "the release's digest \"" + digest + "\" is not \"sha256:\" and 64 hex digits";
        return false;
    }
    const std::string got = update_sha256_file(file, err);
    if (got.empty()) {
        return false;
    }
    if (got != want) {
        *err = "the download " + file + " is damaged or not the release: its SHA-256 is " + got + ", the release's " +
               want;
        return false;
    }
    return true;
}

// Runs `args` in `cwd` to its end; its output (stdout and stderr) in `out`.
static bool run_to_end(const std::vector<std::string> &args, const std::string &cwd, std::string *out, int *code,
                       std::string *err) {
    std::vector<const char *> argv;
    for (const std::string &a : args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)argv.data());
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, cwd.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (p == nullptr) {
        *err = "cannot run " + args[0] + ": " + SDL_GetError();
        return false;
    }
    size_t n = 0;
    void *data = SDL_ReadProcess(p, &n, code);
    if (data != nullptr) {
        out->assign((const char *)data, n);
        SDL_free(data);
    }
    SDL_DestroyProcess(p);
    return true;
}

// The new launcher, detached: it outlives this one.
static bool relaunch(const std::string &exe, const std::vector<std::string> &args, std::string *err) {
    std::vector<const char *> argv{ exe.c_str() };
    for (const std::string &a : args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)argv.data());
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (p == nullptr) {
        *err = "cannot start " + exe + ": " + SDL_GetError();
        return false;
    }
    SDL_DestroyProcess(p); // does not end it
    return true;
}

static std::string last_part(const std::string &text, size_t n = 600) {
    std::string s = text.size() > n ? "..." + text.substr(text.size() - n) : text;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

static void finish(UpdateResult *r, const UpdatePlan &plan, const std::string &file,
                   const std::vector<std::string> &args, bool do_relaunch, const std::string &done) {
    r->ok = true;
    SDL_RemovePath(file.c_str());
    if (!do_relaunch) {
        r->message = done;
        return;
    }
    std::string err;
    if (relaunch(plan.launcher, args, &err)) {
        r->exit_now = true;
        r->message = done + "; the new launcher was started";
    } else {
        r->message = done + ", but the new launcher could not be started (" + err + "): start it again";
    }
}

static UpdateResult apply_appimage(const UpdatePlan &plan, const std::string &file,
                                   const std::vector<std::string> &args, bool do_relaunch) {
    UpdateResult r;
    const std::string &tmp = plan.staging;
    SDL_RemovePath(tmp.c_str());
    // Copied, not renamed: the download is in the settings directory, maybe on another filesystem.
    if (!SDL_CopyFile(file.c_str(), tmp.c_str())) {
        r.message = "cannot write the new AppImage beside the old one (" + tmp + "): " + SDL_GetError() +
                    "; nothing was changed";
        SDL_RemovePath(tmp.c_str());
        return r;
    }
#ifndef SDL_PLATFORM_WINDOWS
    if (chmod(tmp.c_str(), 0755) != 0) {
        r.message = "cannot make " + tmp + " executable: " + std::strerror(errno) + "; nothing was changed";
        SDL_RemovePath(tmp.c_str());
        return r;
    }
#endif
    // One rename on the same filesystem: the old image or the new one, never half of either. The running launcher
    // keeps the old image open (its mount) until it exits.
    if (!SDL_RenamePath(tmp.c_str(), plan.target.c_str())) {
        r.message = "cannot replace " + plan.target + ": " + SDL_GetError() + "; nothing was changed";
        SDL_RemovePath(tmp.c_str());
        return r;
    }
    finish(&r, plan, file, args, do_relaunch, "the AppImage " + plan.target + " was replaced");
    return r;
}

struct PackageEntry {
    std::string rel; // with '/'
    bool dir;
};

struct ListContext {
    std::string root;
    std::string prefix;
    std::vector<PackageEntry> *out;
};

static SDL_EnumerationResult list_entry(void *data, const char *dir, const char *name) {
    ListContext *c = (ListContext *)data;
    const std::string p = path_join(dir, name);
    const std::string rel = c->prefix.empty() ? std::string(name) : c->prefix + "/" + name;
    if (path_is_dir(p)) {
        c->out->push_back({ rel, true });
        ListContext sub{ c->root, rel, c->out };
        SDL_EnumerateDirectory(p.c_str(), list_entry, &sub);
    } else {
        c->out->push_back({ rel, false });
    }
    return SDL_ENUM_CONTINUE;
}

static std::vector<PackageEntry> list_tree(const std::string &root) {
    std::vector<PackageEntry> out;
    ListContext c{ root, "", &out };
    SDL_EnumerateDirectory(root.c_str(), list_entry, &c);
    // Directories before what is in them, and a stable order.
    std::sort(out.begin(), out.end(), [](const PackageEntry &a, const PackageEntry &b) { return a.rel < b.rel; });
    return out;
}

// The release's folder in the extracted zip: the launcher at the top, or in the zip's one top folder
// (<id>-<version>-windows-x86_64/).
static std::string package_root(const std::string &extracted) {
    if (path_is_file(path_join(extracted, LAUNCHER_EXE))) {
        return extracted;
    }
    std::vector<PackageEntry> top;
    ListContext c{ extracted, "", &top };
    SDL_EnumerateDirectory(extracted.c_str(), [](void *data, const char *dir, const char *name) {
        ListContext *ctx = (ListContext *)data;
        ctx->out->push_back({ name, path_is_dir(path_join(dir, name)) });
        return SDL_ENUM_CONTINUE;
    }, &c);
    if (top.size() == 1 && top[0].dir && path_is_file(path_join(path_join(extracted, top[0].rel), LAUNCHER_EXE))) {
        return path_join(extracted, top[0].rel);
    }
    return "";
}

// A staging folder whose old/ holds files: what a failed rollback left, never removed by the launcher.
static bool holds_unrestored(const std::string &staging) {
    for (const PackageEntry &e : list_tree(path_join(staging, "old"))) {
        if (!e.dir) {
            return true;
        }
    }
    return false;
}

static UpdateResult apply_folder(const UpdatePlan &plan, const std::string &file,
                                 const std::vector<std::string> &args, bool do_relaunch) {
    UpdateResult r;
    const std::string &install = plan.target, &staging = plan.staging;
    const std::string extracted = path_join(staging, "new"), backup = path_join(staging, "old");
    std::string err;
    auto fail = [&](const std::string &what) {
        r.message = what + "; nothing was changed";
        remove_tree(staging);
        return r;
    };
    if (holds_unrestored(staging)) {
        r.message = "a previous update left files it could not put back in " + staging + "; nothing was changed";
        return r;
    }
    if (!remove_tree(staging)) {
        r.message = "cannot remove the previous update's staging folder " + staging + "; nothing was changed";
        return r;
    }
    if (!path_make_dir(extracted, &err) || !path_make_dir(backup, &err)) {
        return fail("cannot create the staging folder: " + err);
    }
    // The zip is copied into the staging folder and tar runs there with relative paths only: no drive letter or
    // separator for a tar to misread (a GNU-style one takes "C:" for a remote host).
    if (!SDL_CopyFile(file.c_str(), path_join(staging, "package.zip").c_str())) {
        return fail("cannot copy the download into " + staging + ": " + SDL_GetError());
    }
    std::string out;
    int code = -1;
    if (!run_to_end({ plan.tar, "-xf", "package.zip", "-C", "new" }, staging, &out, &code, &err)) {
        return fail("cannot unpack the release: " + err);
    }
    if (code != 0) {
        return fail("cannot unpack the release (" + plan.tar + " -xf, status " + std::to_string(code) + "): " +
                    last_part(out));
    }
    const std::string root = package_root(extracted);
    if (root.empty() || !path_is_file(path_join(root, GAME_EXE))) {
        return fail(std::string("the release's zip has no ") + LAUNCHER_EXE + " and " + GAME_EXE +
                    " (at its top or in its one folder)");
    }
    const std::vector<PackageEntry> entries = list_tree(root);
    // Before anything moves: a file where the package has a folder (or the reverse), and a previous update's .old
    // still there (the old launcher still running).
    for (const PackageEntry &e : entries) {
        const std::string t = path_join(install, e.rel);
        if (exists(t) && path_is_dir(t) != e.dir) {
            return fail("the install has " + t + (e.dir ? " as a file where the release has a folder"
                                                        : " as a folder where the release has a file"));
        }
    }
    const std::string old = old_launcher(install);
    if (exists(old) && !SDL_RemovePath(old.c_str())) {
        return fail("cannot remove " + old + " (a previous update's launcher, still running?): " + SDL_GetError());
    }

    // The moves, each a rename on the install's volume, journaled so a failure puts everything back.
    struct Step {
        std::string from, to; // renamed from -> to; `to` empty: a folder made at `from`
    };
    std::vector<Step> done;
    auto undo = [&]() {
        bool clean = true;
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            clean &= it->to.empty() ? SDL_RemovePath(it->from.c_str())
                                    : SDL_RenamePath(it->to.c_str(), it->from.c_str());
        }
        return clean;
    };
    auto rename_step = [&](const std::string &from, const std::string &to) {
        if (!SDL_RenamePath(from.c_str(), to.c_str())) {
            err = "cannot move " + from + " to " + to + ": " + SDL_GetError();
            return false;
        }
        done.push_back({ from, to });
        return true;
    };
    bool ok = true;
    for (const PackageEntry &e : entries) {
        const std::string t = path_join(install, e.rel), src = path_join(root, e.rel);
        if (e.dir) {
            if (!exists(t)) {
                if (!SDL_CreateDirectory(t.c_str())) {
                    err = "cannot create " + t + ": " + SDL_GetError();
                    ok = false;
                    break;
                }
                done.push_back({ t, "" });
            }
            continue;
        }
        if (exists(t)) {
            // The running launcher cannot be overwritten but can be renamed: beside itself as .old (removed at the
            // next start). Every other replaced file goes to the staging folder's old/ until the end.
            std::string aside = e.rel == LAUNCHER_EXE ? old : path_join(backup, e.rel);
            if (e.rel != LAUNCHER_EXE && !path_make_dir(path_dir(aside), &err)) {
                ok = false;
                break;
            }
            if (!rename_step(t, aside)) {
                ok = false;
                break;
            }
        }
        if (!rename_step(src, t)) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        if (undo()) {
            r.message = err + "; nothing was changed";
            remove_tree(staging);
        } else { // the staging folder keeps what could not be put back
            r.message = err + "; putting the old files back failed too (what is missing is in " + staging +
                        "): the install may be damaged, get the release from its page";
        }
        return r;
    }
    remove_tree(staging); // the replaced files; the running launcher's .old stays until the next start
    finish(&r, plan, file, args, do_relaunch, "the install folder " + install + " was updated");
    return r;
}

UpdateResult update_apply(const UpdatePlan &plan, const std::string &file, const std::string &digest,
                          const std::vector<std::string> &args, bool do_relaunch) {
    UpdateResult r;
    if (!plan.possible()) {
        r.message = plan.reason;
        return r;
    }
    std::string err;
    if (!update_verify(file, digest, &err)) {
        r.message = err + "; nothing was changed";
        return r;
    }
    return plan.kind == UpdateKind::AppImage ? apply_appimage(plan, file, args, do_relaunch)
                                             : apply_folder(plan, file, args, do_relaunch);
}

void update_cleanup(const UpdateSystem &sys) {
    if (!sys.appimage.empty()) {
        SDL_RemovePath(appimage_staging(sys.appimage).c_str());
    }
    if (sys.install_dir.empty()) {
        return;
    }
    const std::string old = old_launcher(sys.install_dir);
    // The old launcher may still be exiting (it starts this one, then quits): a few tries over about a second.
    for (int i = 0; i < 20 && exists(old) && !SDL_RemovePath(old.c_str()); i++) {
        SDL_Delay(50);
    }
    if (exists(old)) {
        std::fprintf(stderr, "launcher: cannot remove %s: %s\n", old.c_str(), SDL_GetError());
    }
    // A staging folder whose old/ still holds files is what a failed rollback left: kept for the player (the message
    // named it).
    const std::string staging = staging_dir(sys.install_dir);
    if (holds_unrestored(staging)) {
        std::fprintf(stderr, "launcher: %s holds files a failed update could not put back; kept\n", staging.c_str());
        return;
    }
    remove_tree(staging);
}

void update_cleanup_at_start() {
    update_cleanup(update_system());
}

} // namespace psxstack
