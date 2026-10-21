// The self-test of update_apply (launcher/README.md "The self-test"), called by self_test_run on fixture directories
// under its own: SHA-256 of fixture files and the digest check, the plan's answers, the AppImage swap with its
// relaunch, the Windows folder swap from a zip made with tar (on Linux too), the rollback, the cleanup, and the
// running-launcher case: a copy of this executable in a fixture folder updates its own folder (its running exe renamed
// to .old), starts the new one, which removes the .old at its start. No network.
//
// The tar that makes and unpacks the zips: $<PREFIX>_SELFTEST_TAR when set (CI's Linux job: bsdtar), else bsdtar on
// PATH (Linux; GNU tar reads no zip) or Windows' own tar.exe, else a stand-in: this executable, which writes and reads
// stored (uncompressed) zips with tar's arguments. Wine has no tar.exe, and a Unix bsdtar started from Wine is neither
// waited for nor gives its exit status, so the Windows self-test under Wine uses the stand-in.
#include "update_apply.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#ifndef SDL_PLATFORM_WINDOWS
#include <sys/stat.h> // chmod: a read-only directory for the failure paths
#endif

#include "brand.h"
#include "paths.h"
#include "selftest.h"

namespace psxstack {

static int *n_checks, *n_failures;

static void check(bool ok, const std::string &what) {
    (*n_checks)++;
    if (!ok) {
        (*n_failures)++;
        std::fprintf(stderr, "self-test: FAILED: update: %s\n", what.c_str());
    }
}

// The new build's mark: appended to a copy of this executable (trailing bytes change neither an ELF nor a PE file).
static const char NEW_MARK[] = "\npsxstack-selftest-new-build\n";
static const char CHILD_MODE[] = "update"; // self_test_fake_game's mode for self_test_update_child

static bool put(const std::string &path, const std::string &text) {
    return SDL_SaveFile(path.c_str(), text.data(), text.size());
}

static std::string get(const std::string &path) {
    std::string text, err;
    file_read(path, &text, &err);
    return text;
}

static bool ends_with(const std::string &s, const std::string &tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

static bool exists(const std::string &p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info);
}

static SDL_EnumerationResult remove_entry(void *, const char *dir, const char *name) {
    const std::string p = path_join(dir, name);
    if (path_is_dir(p)) {
        SDL_EnumerateDirectory(p.c_str(), remove_entry, nullptr);
    }
    SDL_RemovePath(p.c_str());
    return SDL_ENUM_CONTINUE;
}

static void remove_tree(const std::string &p) {
#ifndef SDL_PLATFORM_WINDOWS
    chmod(p.c_str(), 0755); // a read-only fixture left by a failed run
#endif
    if (path_is_dir(p)) {
        SDL_EnumerateDirectory(p.c_str(), remove_entry, nullptr);
    }
    SDL_RemovePath(p.c_str());
}

// Every file under `dir` with its contents ("<rel>/" for a folder): the state an "intact" check compares.
static SDL_EnumerationResult snap_entry(void *data, const char *dir, const char *name) {
    auto *m = (std::map<std::string, std::string> *)data;
    const std::string p = path_join(dir, name);
    if (path_is_dir(p)) {
        (*m)[p + "/"] = "";
        SDL_EnumerateDirectory(p.c_str(), snap_entry, data);
    } else {
        (*m)[p] = get(p);
    }
    return SDL_ENUM_CONTINUE;
}

static std::map<std::string, std::string> snapshot(const std::string &dir) {
    std::map<std::string, std::string> m;
    SDL_EnumerateDirectory(dir.c_str(), snap_entry, &m);
    return m;
}

static std::string self_exe() {
    const char *base = SDL_GetBasePath();
    return path_join(base != nullptr ? base : "", LAUNCHER_EXE);
}

// A copy of this executable at `to` (with the DLLs beside it, if a build has any), `mark` appended.
static bool copy_self(const std::string &to, const std::string &mark) {
    if (!SDL_CopyFile(self_exe().c_str(), to.c_str())) {
        return false;
    }
#ifdef SDL_PLATFORM_WINDOWS
    const char *base = SDL_GetBasePath();
    int n = 0;
    if (char **dlls = SDL_GlobDirectory(base, "*.dll", SDL_GLOB_CASEINSENSITIVE, &n)) {
        for (int i = 0; i < n; i++) {
            SDL_CopyFile(path_join(base, dlls[i]).c_str(), path_join(path_dir(to), dlls[i]).c_str());
        }
        SDL_free(dlls);
    }
#else
    chmod(to.c_str(), 0755);
#endif
    if (mark.empty()) {
        return true;
    }
    SDL_IOStream *io = SDL_IOFromFile(to.c_str(), "ab");
    if (io == nullptr) {
        return false;
    }
    const bool ok = SDL_WriteIO(io, mark.data(), mark.size()) == mark.size();
    return SDL_CloseIO(io) && ok;
}

static bool run(const std::vector<std::string> &args, const std::string &cwd, std::string *out, int *code) {
    std::vector<const char *> argv;
    for (const std::string &a : args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)argv.data());
    if (!cwd.empty()) {
        SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, cwd.c_str());
    }
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (p == nullptr) {
        *out = std::string("cannot run ") + args[0] + ": " + SDL_GetError();
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

// The zip of `top` (a folder in `dir`) at `zip`, made with tar in `dir` with relative paths, as update_apply runs it.
static bool make_zip(const std::string &tar, const std::string &dir, const std::string &top, const std::string &zip) {
    std::string out;
    int code = -1;
    const std::string tmp = path_join(dir, "made.zip");
    if (!run({ tar, "-c", "-f", "made.zip", "--format", "zip", top }, dir, &out, &code) || code != 0) {
        std::fprintf(stderr, "self-test: update: %s -c --format zip failed (%d): %s\n", tar.c_str(), code, out.c_str());
        return false;
    }
    return SDL_RenamePath(tmp.c_str(), zip.c_str());
}

static std::string digest_of(const std::string &file) {
    std::string err;
    return "sha256:" + update_sha256_file(file, &err);
}

// Waits for `path` to have text (a relaunched process writes it), up to `ms`.
static std::string wait_for(const std::string &path, int ms) {
    for (int t = 0; t < ms; t += 50) {
        std::string s = get(path);
        if (!s.empty()) {
            return s;
        }
        SDL_Delay(50);
    }
    return "";
}

static void set_env(const char *name, const std::string &value) {
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), name, value.c_str(), true);
}

static void unset_env(const char *name) {
    SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), name);
}

// A directory made read-only for the failure paths (POSIX; skipped when the test runs as root, which writes anyway,
// and on Windows, where a folder's read-only flag does not stop writes).
static bool make_read_only(const std::string &dir, bool on) {
#ifdef SDL_PLATFORM_WINDOWS
    (void)dir;
    (void)on;
    return false;
#else
    chmod(dir.c_str(), on ? 0555 : 0755);
    if (!on) {
        return true;
    }
    const std::string probe = path_join(dir, "probe");
    if (SDL_SaveFile(probe.c_str(), "x", 1)) {
        SDL_RemovePath(probe.c_str());
        chmod(dir.c_str(), 0755);
        return false; // root
    }
    return true;
#endif
}

// ---- SHA-256 and the digest

static void test_sha256(const std::string &root) {
    std::string err;
    path_make_dir(root, nullptr);
    put(path_join(root, "abc"), "abc");
    put(path_join(root, "empty"), "");
    put(path_join(root, "million"), std::string(1000000, 'a')); // in 64 KiB reads: many chunks
    check(update_sha256_file(path_join(root, "abc"), &err) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 of \"abc\" (FIPS 180-4)");
    check(update_sha256_file(path_join(root, "empty"), &err) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256 of an empty file");
    check(update_sha256_file(path_join(root, "million"), &err) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "SHA-256 of one million 'a' (FIPS 180-4), read in chunks");
    err.clear();
    check(update_sha256_file(path_join(root, "missing"), &err).empty() && !err.empty(), "SHA-256 of a missing file");

    const std::string abc = path_join(root, "abc");
    check(update_verify(abc, "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", &err),
          "verify: the right digest: " + err);
    check(update_verify(abc, "sha256:BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", &err),
          "verify: the right digest in capitals");
    err.clear();
    check(!update_verify(abc, "sha256:ca7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", &err) &&
              err.find("damaged") != std::string::npos,
          "verify: a wrong digest is refused, naming both: " + err);
    err.clear();
    check(!update_verify(abc, "sha1:a9993e364706816aba3e25717850c26c9cd0d89d", &err) &&
              err.find("not \"sha256:\"") != std::string::npos,
          "verify: another algorithm's digest");
    err.clear();
    check(!update_verify(abc, "sha256:ba7816bf", &err) && !err.empty(), "verify: a short digest");
    err.clear();
    check(!update_verify(path_join(root, "missing"),
                         "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", &err) &&
              err.find("cannot read") != std::string::npos,
          "verify: a missing file");
}

// ---- the plan

static void test_plan(const std::string &root, const std::string &tar) {
    path_make_dir(root, nullptr);
    const std::string empty = path_join(root, "empty-dir"), bin = path_join(root, "bin");
    path_make_dir(empty, nullptr);
    path_make_dir(bin, nullptr);
    put(path_join(bin, "tool"), "");
#ifdef SDL_PLATFORM_WINDOWS
    const std::string sep = ";";
#else
    const std::string sep = ":";
#endif
    check(update_find_program("tool", empty).empty(), "PATH search: an empty directory has no tar");
    check(update_find_program("tool", empty + sep + sep + bin) == path_join(bin, "tool"),
          "PATH search: the second directory, an empty entry skipped");

    UpdateSystem sys;
    sys.kind = UpdateKind::AppImage;
    UpdatePlan p = update_plan(sys, root);
    check(!p.possible() && p.reason.find("AppImage") != std::string::npos, "plan: no $APPIMAGE, no self-update");
    sys.appimage = path_join(root, "gone.AppImage");
    p = update_plan(sys, root);
    check(!p.possible() && p.reason.find("not there") != std::string::npos, "plan: $APPIMAGE names no file");
#ifndef SDL_PLATFORM_WINDOWS
    set_env("APPIMAGE", "/x/y.AppImage");
    check(update_system().appimage == "/x/y.AppImage" && update_system().kind == UpdateKind::AppImage,
          "update_system: $APPIMAGE");
    unset_env("APPIMAGE");
    check(update_system().appimage.empty(), "update_system: without $APPIMAGE");
#else
    check(update_system().kind == UpdateKind::WindowsFolder, "update_system: the Windows folder");
#endif

    const std::string install = path_join(root, "install");
    path_make_dir(install, nullptr);
    UpdateSystem w;
    w.kind = UpdateKind::WindowsFolder;
    w.install_dir = install;
    w.tar = tar.empty() ? path_join(bin, "tool") : tar;
    p = update_plan(w, root);
    check(!p.possible() && p.reason.find(LAUNCHER_EXE) != std::string::npos, "plan: a folder without the launcher");
    put(path_join(install, LAUNCHER_EXE), "launcher");
    p = update_plan(w, root);
    check(p.possible() && p.kind == UpdateKind::WindowsFolder && p.launcher == path_join(install, LAUNCHER_EXE) &&
              path_dir(p.staging) == install && path_dir(p.download) == root && p.tar == w.tar,
          "plan: the folder, its staging inside it, the download in the given directory: " + p.reason);
    w.tar = "";
    p = update_plan(w, root);
    check(!p.possible() && p.reason.find("tar") != std::string::npos, "plan: no tar: " + p.reason);
    w.tar = path_join(empty, "tar.exe");
    check(!update_plan(w, root).possible(), "plan: a tar path that names nothing");
    w.tar = path_join(bin, "tool");
    if (make_read_only(install, true)) {
        p = update_plan(w, root);
        check(!p.possible() && p.reason.find("not writable") != std::string::npos, "plan: a read-only folder");
        sys.appimage = path_join(install, LAUNCHER_EXE);
        p = update_plan(sys, root);
        check(!p.possible() && p.reason.find("not writable") != std::string::npos,
              "plan: an AppImage in a read-only folder");
        make_read_only(install, false);
    } else {
        std::fprintf(stderr, "self-test: update: the read-only plan checks skipped (no read-only folder here)\n");
    }
}

// ---- the AppImage

static void test_appimage(const std::string &root) {
    const std::string images = path_join(root, "images"), downloads = path_join(root, "downloads");
    path_make_dir(images, nullptr);
    path_make_dir(downloads, nullptr);
    const std::string image = path_join(images, "Game-1.0-x86_64.AppImage");
    check(copy_self(image, ""), "the fake AppImage (a copy of this executable)");
    UpdateSystem sys;
    sys.kind = UpdateKind::AppImage;
    sys.appimage = image;
    UpdatePlan plan = update_plan(sys, downloads);
    check(plan.possible() && plan.target == image && plan.launcher == image && path_dir(plan.staging) == images &&
              path_dir(plan.download) == downloads,
          "plan: the AppImage, its temporary file beside it, the download in the settings directory: " + plan.reason);
    if (!plan.possible()) {
        return;
    }
    check(copy_self(plan.download, NEW_MARK), "the new AppImage downloaded");
    const std::string digest = digest_of(plan.download);
    const std::string before = get(image);

    UpdateResult r = update_apply(plan, plan.download,
                                  "sha256:0000000000000000000000000000000000000000000000000000000000000000", {}, false);
    check(!r.ok && !r.exit_now && r.message.find("nothing was changed") != std::string::npos && get(image) == before &&
              path_is_file(plan.download),
          "apply: a wrong digest changes nothing: " + r.message);
    if (make_read_only(images, true)) {
        r = update_apply(plan, plan.download, digest, {}, false);
        check(!r.ok && r.message.find("cannot write") != std::string::npos && get(image) == before &&
                  !exists(plan.staging),
              "apply: a folder that turned read-only changes nothing: " + r.message);
        make_read_only(images, false);
    }

    // The swap and the relaunch: the new image is this executable's copy, started as the self-test's stand-in.
    const std::string marker = path_join(root, "appimage-relaunched.txt");
    set_env(SELF_TEST_GAME_ENV, CHILD_MODE);
    set_env("APPIMAGE", image);
    r = update_apply(plan, plan.download, digest, { "relaunched", marker });
    check(r.ok && r.exit_now, "apply: the AppImage swapped in and relaunched: " + r.message);
    check(ends_with(get(image), NEW_MARK) && !exists(plan.staging) && !exists(plan.download),
          "apply: the image is the new one, no temporary file, the download removed");
    const std::string said = wait_for(marker, 20000);
    check(said.find("relaunched") != std::string::npos, "the new AppImage ran (it is executable): " + said);
    unset_env("APPIMAGE");
    unset_env(SELF_TEST_GAME_ENV);

    put(plan.staging, "left over");
    update_cleanup(sys);
    check(!exists(plan.staging) && path_is_file(image), "cleanup: a temporary file beside the AppImage is removed");
}

// ---- the Windows folder (also on Linux: the same file operations)

struct Package {
    std::string dir, top;
};

// <dir>/<top>/: the release's folder as the package script makes it, with `launcher` as its launcher.
static Package make_package(const std::string &dir, bool with_game) {
    Package p{ dir, std::string(GAME_ID) + "-9.9.9-windows-x86_64" };
    const std::string t = path_join(dir, p.top);
    for (const char *d : { "", "mods", "mods/a", "mods/b", "LICENSES" }) {
        path_make_dir(path_join(t, d), nullptr);
    }
    put(path_join(t, LAUNCHER_EXE), "new launcher");
    if (with_game) {
        put(path_join(t, GAME_EXE), "new game");
    }
    put(path_join(t, "README.txt"), "new readme");
    put(path_join(t, "mods/a/mod.json"), "new a");
    put(path_join(t, "mods/b/mod.json"), "new b");
    put(path_join(t, "LICENSES/SDL3.txt"), "license");
    return p;
}

static void make_install(const std::string &install) {
    for (const char *d : { "", "mods", "mods/a", "mods/user" }) {
        path_make_dir(path_join(install, d), nullptr);
    }
    put(path_join(install, LAUNCHER_EXE), "old launcher");
    put(path_join(install, GAME_EXE), "old game");
    put(path_join(install, "README.txt"), "old readme");
    put(path_join(install, "mods/a/mod.json"), "old a");
    put(path_join(install, "mods/user/mod.json"), "the user's");
    put(path_join(install, "notes.txt"), "the user's notes");
}

static void test_folder(const std::string &root, const std::string &tar) {
    const std::string install = path_join(root, "install"), downloads = path_join(root, "downloads");
    path_make_dir(downloads, nullptr);
    make_install(install);
    UpdateSystem sys;
    sys.kind = UpdateKind::WindowsFolder;
    sys.install_dir = install;
    sys.tar = tar;
    UpdatePlan plan = update_plan(sys, downloads);
    check(plan.possible(), "plan: the folder: " + plan.reason);
    if (!plan.possible()) {
        return;
    }
    const Package pkg = make_package(path_join(root, "package"), true);
    const Package nogame = make_package(path_join(root, "package-nogame"), false);
    const std::string zip = plan.download, zip_nogame = path_join(downloads, "nogame.zip");
    check(make_zip(tar, pkg.dir, pkg.top, zip) && make_zip(tar, nogame.dir, nogame.top, zip_nogame),
          "the release's zips, made with " + tar);
    const std::string digest = digest_of(zip);
    const auto before = snapshot(install);

    // Failures: each leaves the install exactly as it was, with no staging folder.
    auto intact = [&](const UpdateResult &r, const std::string &what, const std::string &words) {
        check(!r.ok && !r.exit_now && r.message.find(words) != std::string::npos && snapshot(install) == before &&
                  !exists(plan.staging),
              "apply: " + what + " changes nothing: " + r.message);
    };
    intact(update_apply(plan, zip, "sha256:" + std::string(64, '0'), {}, false), "a wrong digest", "damaged");
    const std::string junk = path_join(downloads, "junk.zip");
    put(junk, "not a zip");
    intact(update_apply(plan, junk, digest_of(junk), {}, false), "a file tar cannot unpack", "cannot unpack");
    intact(update_apply(plan, zip_nogame, digest_of(zip_nogame), {}, false), "a zip without the game",
           std::string("has no ") + LAUNCHER_EXE);
    UpdatePlan no_tar = plan;
    no_tar.tar = path_join(root, "no-such-tar");
    intact(update_apply(no_tar, zip, digest, {}, false), "a tar that is not there", "cannot unpack");
    put(path_join(install, "LICENSES"), "a file in the way");
    const auto with_file = snapshot(install);
    UpdateResult r = update_apply(plan, zip, digest, {}, false);
    check(!r.ok && r.message.find("as a file where the release has a folder") != std::string::npos &&
              snapshot(install) == with_file,
          "apply: a file where the release has a folder changes nothing: " + r.message);
    SDL_RemovePath(path_join(install, "LICENSES").c_str());
    // A failure halfway (POSIX): mods/a read-only, so its file cannot be moved after the launcher, the game and the
    // README were; every one of them is put back.
    if (make_read_only(path_join(install, "mods/a"), true)) {
        r = update_apply(plan, zip, digest, {}, false);
        make_read_only(path_join(install, "mods/a"), false);
        intact(r, "a failure halfway (put back)", "nothing was changed");
    }
    check(path_is_file(zip), "the download is kept after a failure");

    // The swap: the release's files in, the user's kept, the old launcher beside the new one as .old.
    r = update_apply(plan, zip, digest, {}, false);
    check(r.ok && !r.exit_now, "apply: the folder: " + r.message);
    check(get(path_join(install, LAUNCHER_EXE)) == "new launcher" && get(path_join(install, GAME_EXE)) == "new game" &&
              get(path_join(install, "README.txt")) == "new readme" &&
              get(path_join(install, "mods/a/mod.json")) == "new a" &&
              get(path_join(install, "mods/b/mod.json")) == "new b" &&
              get(path_join(install, "LICENSES/SDL3.txt")) == "license",
          "apply: the release's files are in place");
    check(get(path_join(install, "notes.txt")) == "the user's notes" &&
              get(path_join(install, "mods/user/mod.json")) == "the user's",
          "apply: files not in the release are kept");
    const std::string old = path_join(install, std::string(LAUNCHER_EXE) + ".old");
    check(get(old) == "old launcher" && !exists(plan.staging) && !exists(zip),
          "apply: the old launcher is .old, the staging folder and the download are gone");

    path_make_dir(path_join(plan.staging, "new"), nullptr);
    update_cleanup(sys);
    check(!exists(old) && !exists(plan.staging) && path_is_file(path_join(install, LAUNCHER_EXE)),
          "cleanup: the .old and a stale staging folder are removed");
    path_make_dir(path_join(plan.staging, "old"), nullptr);
    put(path_join(plan.staging, "old/README.txt"), "not put back");
    update_cleanup(sys);
    check(get(path_join(plan.staging, "old/README.txt")) == "not put back",
          "cleanup: a staging folder holding files a failed rollback left is kept");
    remove_tree(plan.staging);
}

// The running launcher: a copy of this executable in a fixture folder applies the release to its own folder (its
// running exe renamed to .old: the Windows rule this exists for), starts the new one and exits; the new one removes
// the .old at its start and says so.
static void test_running(const std::string &root, const std::string &tar) {
    const std::string install = path_join(root, "install"), downloads = path_join(root, "downloads");
    path_make_dir(install, nullptr);
    path_make_dir(downloads, nullptr);
    const std::string launcher = path_join(install, LAUNCHER_EXE);
    check(copy_self(launcher, ""), "the installed launcher (a copy of this executable)");
    put(path_join(install, GAME_EXE), "old game");
    put(path_join(install, "notes.txt"), "the user's notes");
    Package pkg = make_package(path_join(root, "package"), true);
    check(copy_self(path_join(path_join(pkg.dir, pkg.top), LAUNCHER_EXE), NEW_MARK), "the release's launcher");
    const std::string zip = path_join(downloads, "release.zip"), marker = path_join(root, "relaunched.txt");
    check(make_zip(tar, pkg.dir, pkg.top, zip), "the release's zip");
    std::string out;
    int code = -1;
    const bool started = run({ launcher, "apply", zip, digest_of(zip), tar, marker }, install, &out, &code);
    check(started && code == 0, "the installed launcher applied the release to its own folder (status " +
                                    std::to_string(code) + "): " + out);
    const std::string said = wait_for(marker, 30000);
    check(said.find("relaunched") != std::string::npos, "the new launcher was started: " + said);
    check(said.find("old gone") != std::string::npos, "the new launcher removed the .old at its start: " + said);
    check(ends_with(get(launcher), NEW_MARK) && get(path_join(install, GAME_EXE)) == "new game" &&
              get(path_join(install, "notes.txt")) == "the user's notes" &&
              !exists(path_join(install, std::string(GAME_ID) + "-update.tmp")) && !exists(zip),
          "the folder has the new launcher and game, the user's file, no staging folder, no download");
}

// ---- the stand-in tar: `-c -f ZIP --format zip DIR` and `-xf ZIP -C DIR`, stored entries only

static uint32_t crc32(const std::string &data) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) {
        c ^= b;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        }
    }
    return ~c;
}

static void le(std::string *out, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
        out->push_back((char)(v >> (8 * i)));
    }
}

static uint32_t rd(const std::string &s, size_t at, int bytes) {
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++) {
        v |= (uint32_t)(unsigned char)s[at + i] << (8 * i);
    }
    return v;
}

static SDL_EnumerationResult zip_entry(void *data, const char *dir, const char *name) {
    auto *names = (std::vector<std::string> *)data;
    const std::string p = path_join(dir, name);
    names->push_back(path_is_dir(p) ? p + "/" : p);
    if (path_is_dir(p)) {
        SDL_EnumerateDirectory(p.c_str(), zip_entry, data);
    }
    return SDL_ENUM_CONTINUE;
}

static int stand_in_zip(const std::string &zip, const std::string &top) {
    std::vector<std::string> names{ top + "/" };
    SDL_EnumerateDirectory(top.c_str(), zip_entry, &names);
    std::string out, central;
    for (std::string name : names) {
        for (char &ch : name) {
            ch = ch == '\\' ? '/' : ch;
        }
        const bool dir = name.back() == '/';
        const std::string data = dir ? "" : get(name);
        const uint32_t crc = crc32(data), offset = (uint32_t)out.size();
        std::string h;
        le(&h, 20, 2); // version needed
        le(&h, 0, 2);  // flags
        le(&h, 0, 2);  // stored
        le(&h, 0, 2);  // time
        le(&h, 0x21, 2); // 1980-01-01
        le(&h, crc, 4);
        le(&h, (uint32_t)data.size(), 4);
        le(&h, (uint32_t)data.size(), 4);
        le(&h, (uint32_t)name.size(), 2);
        le(&h, 0, 2);
        le(&out, 0x04034b50u, 4);
        out += h + name + data;
        le(&central, 0x02014b50u, 4);
        le(&central, 20, 2); // made by
        central += h;
        le(&central, 0, 2); // comment
        le(&central, 0, 2); // disk
        le(&central, 0, 2); // internal attributes
        le(&central, dir ? 0x10 : 0, 4);
        le(&central, offset, 4);
        central += name;
    }
    const uint32_t cd_at = (uint32_t)out.size();
    out += central;
    le(&out, 0x06054b50u, 4);
    le(&out, 0, 4);
    le(&out, (uint32_t)names.size(), 2);
    le(&out, (uint32_t)names.size(), 2);
    le(&out, (uint32_t)central.size(), 4);
    le(&out, cd_at, 4);
    le(&out, 0, 2);
    return put(zip, out) ? 0 : 1;
}

static int stand_in_unzip(const std::string &zip, const std::string &dest) {
    const std::string s = get(zip);
    size_t at = 0;
    int entries = 0;
    while (at + 30 <= s.size() && rd(s, at, 4) == 0x04034b50u) {
        const uint32_t flags = rd(s, at + 6, 2), method = rd(s, at + 8, 2), crc = rd(s, at + 14, 4);
        const uint32_t size = rd(s, at + 18, 4), nlen = rd(s, at + 26, 2), elen = rd(s, at + 28, 2);
        const size_t data_at = at + 30 + nlen + elen;
        if (method != 0 || (flags & 8) != 0 || data_at + size > s.size()) {
            std::fprintf(stderr, "stand-in tar: %s: not a stored zip entry\n", zip.c_str());
            return 1;
        }
        const std::string name = s.substr(at + 30, nlen), data = s.substr(data_at, size);
        if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos || crc32(data) != crc) {
            std::fprintf(stderr, "stand-in tar: %s: a bad entry\n", zip.c_str());
            return 1;
        }
        const std::string p = path_join(dest, name);
        if (name.back() == '/') {
            path_make_dir(p, nullptr);
        } else if (!path_make_dir(path_dir(p), nullptr) || !put(p, data)) {
            std::fprintf(stderr, "stand-in tar: cannot write %s\n", p.c_str());
            return 1;
        }
#ifndef SDL_PLATFORM_WINDOWS
        chmod(p.c_str(), 0755); // the stand-in keeps no modes: every file executable (the launcher among them)
#endif
        entries++;
        at = data_at + size;
    }
    if (entries == 0) {
        std::fprintf(stderr, "stand-in tar: Error opening archive: %s is not a zip\n", zip.c_str());
        return 1;
    }
    return 0;
}

int self_test_update_child(int argc, char **argv) {
    if (argc == 7 && std::strcmp(argv[1], "-c") == 0 && std::strcmp(argv[2], "-f") == 0) { // the stand-in tar
        return stand_in_zip(argv[3], argv[6]);
    }
    if (argc == 5 && std::strcmp(argv[1], "-xf") == 0 && std::strcmp(argv[3], "-C") == 0) {
        return stand_in_unzip(argv[2], argv[4]);
    }
    if (argc == 3 && std::strcmp(argv[1], "relaunched") == 0) { // the new launcher: what main() does at start
        update_cleanup_at_start();
        const char *base = SDL_GetBasePath();
        const std::string old = path_join(base != nullptr ? base : "", std::string(LAUNCHER_EXE) + ".old");
        return put(argv[2], std::string("relaunched\n") + (exists(old) ? "old present\n" : "old gone\n")) ? 0 : 1;
    }
    if (argc == 6 && std::strcmp(argv[1], "apply") == 0) { // the installed launcher: apply, relaunch
        UpdateSystem sys;
        sys.kind = UpdateKind::WindowsFolder;
        const char *base = SDL_GetBasePath();
        sys.install_dir = path_strip_slash(base != nullptr ? base : "");
        sys.tar = argv[4];
        UpdatePlan plan = update_plan(sys, path_dir(argv[2]));
        UpdateResult r = update_apply(plan, argv[2], argv[3], { "relaunched", argv[5] });
        std::printf("%s\n", r.message.c_str());
        return r.ok && r.exit_now ? 0 : 1;
    }
    std::fprintf(stderr, "self-test: update: unknown child arguments\n");
    return 64;
}

void self_test_update(const std::string &dir, int *checks, int *failures) {
    n_checks = checks;
    n_failures = failures;
    remove_tree(dir);
    path_make_dir(dir, nullptr);
    std::string tar;
    const std::string tar_env = std::string(PSXSTACK_GAME_ENV_PREFIX) + "_SELFTEST_TAR";
    if (const char *t = SDL_getenv(tar_env.c_str())) {
        tar = t;
    } else {
#ifdef SDL_PLATFORM_WINDOWS
        tar = update_system().tar;
#else
        const char *path = SDL_getenv("PATH");
        tar = update_find_program("bsdtar", path != nullptr ? path : "");
#endif
    }
    if (tar.empty()) {
        tar = self_exe();
        std::fprintf(stderr, "self-test: update: no bsdtar or tar.exe: the zips are the stand-in's (stored)\n");
    }
    test_sha256(path_join(dir, "sha256"));
    test_plan(path_join(dir, "plan"), tar);
    test_appimage(path_join(dir, "appimage"));
    set_env(SELF_TEST_GAME_ENV, CHILD_MODE); // the stand-in tar, the installed launcher and the relaunched one
    test_folder(path_join(dir, "folder"), tar);
    test_running(path_join(dir, "running"), tar);
    unset_env(SELF_TEST_GAME_ENV);
}

} // namespace psxstack
