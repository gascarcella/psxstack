// Applying a downloaded launcher release (docs/LAUNCHER.md "Applying one"): the plan (whether this install can update
// itself, where things go), the SHA-256 check against the release asset's digest, the swap (the AppImage renamed over
// the running one; the Windows folder's files moved in, the running launcher renamed to .old), the relaunch, and the
// cleanup at the next start. No network and no UI here: the caller downloads to `UpdatePlan::download` and asks.
// Every failure leaves the old install as it was and names what failed.
#pragma once

#include <string>
#include <vector>

namespace psxstack {

enum class UpdateKind {
    None,          // no self-update: the caller offers the release page instead
    AppImage,      // Linux: the running image ($APPIMAGE) is replaced by the new one
    WindowsFolder, // Windows: the install folder's files are replaced from the release's zip
};

// The running launcher's situation. update_system() reads this process's; the self-test makes its own.
struct UpdateSystem {
    UpdateKind kind = UpdateKind::None; // AppImage on Linux, WindowsFolder on Windows
    std::string appimage;               // AppImage: $APPIMAGE ("" when unset)
    std::string install_dir;            // WindowsFolder: the launcher's own directory
    std::string tar;                    // WindowsFolder: the tar that extracts the zip ("" when none was found)
};
// %SystemRoot%\System32\tar.exe first (Windows 10 1803+'s bsdtar; a GNU tar elsewhere on PATH cannot read a zip),
// then tar.exe on PATH.
UpdateSystem update_system();

// What an update would do here, computed before anything is downloaded so the UI can say whether it is possible.
struct UpdatePlan {
    UpdateKind kind = UpdateKind::None; // None: not possible here, `reason` says why
    std::string reason;                 // for the UI, when not possible
    std::string target;   // AppImage: the image replaced; WindowsFolder: the install folder
    std::string launcher; // what is started afterwards (the image; <install>/<id>-launcher.exe)
    std::string staging;  // AppImage: the temporary file beside the image; WindowsFolder: the staging folder inside
                          // the install folder (the same volume, so every move is a rename)
    std::string download; // where the caller downloads the asset to: in `download_dir` (the settings directory)
    std::string tar;      // WindowsFolder: the tar that extracts the zip
    bool possible() const { return kind != UpdateKind::None; }
};
// AppImage: $APPIMAGE set, naming an existing file in a writable directory. WindowsFolder: the launcher's directory
// holds <id>-launcher.exe and is writable, and a tar was found.
UpdatePlan update_plan(const UpdateSystem &sys, const std::string &download_dir);

// The file's SHA-256 as 64 lowercase hex digits, read in chunks; "" with `err` set when it cannot be read.
std::string update_sha256_file(const std::string &file, std::string *err);
// `file` against `digest`, "sha256:<64 hex digits>" (GitHub's release asset `digest`; either case). False with `err`
// set when the digest is malformed, the file unreadable, or the two differ.
bool update_verify(const std::string &file, const std::string &digest, std::string *err);

struct UpdateResult {
    bool ok = false;       // the new release is in place (false: the old install is as it was, `message` says why)
    bool exit_now = false; // the new launcher was started: the caller saves its settings and exits at once
    std::string message;   // what failed, or what was done
};
// Verifies `file` against `digest` first, then swaps it in per `plan` and removes `file`. `args`: the relaunched
// launcher's arguments (the caller's argv without argv[0], e.g. its --config-dir); `relaunch` false: the files only.
UpdateResult update_apply(const UpdatePlan &plan, const std::string &file, const std::string &digest,
                          const std::vector<std::string> &args, bool relaunch = true);

// At start: removes what an update left beside the running launcher (<id>-launcher.exe.old, retried briefly while the
// old launcher exits; a staging folder; an AppImage's temporary file). main() calls it once.
void update_cleanup_at_start();
void update_cleanup(const UpdateSystem &sys); // the same for a given situation (the self-test)

// `name` in the directories of `path_list` (PATH's form: ';' between them on Windows, ':' elsewhere); "" when absent.
std::string update_find_program(const std::string &name, const std::string &path_list);

// The self-test of this module (selftest_update.cpp), on fixture directories under `dir`; adds to the counts.
void self_test_update(const std::string &dir, int *checks, int *failures);
// The self-test's child processes: the launcher's copy that applies an update to its own folder and relaunches, and
// the relaunched one (self_test_fake_game's mode "update").
int self_test_update_child(int argc, char **argv);

} // namespace psxstack
