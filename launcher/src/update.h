// The update check (docs/LAUNCHER.md "Updates"): the latest release of the game's GitHub repository (the description's
// launcher.updates.github, brand.h GAME_UPDATES_GITHUB) against the launcher's own version stamp: one GET of
// https://api.github.com/repos/<repo>/releases/latest through curl (http.h) on a worker thread, the answer read with
// the runtime's JSON reader (json_value.h). Only a stamp that is exactly X.Y.Z is offered an update; a development
// build (dev-..., X.Y.Z-N-g..., -dirty) is told the latest version and nothing more.
#pragma once

#include <cstdint>
#include <string>

#include <SDL3/SDL.h>

#include "brand.h"
#include "http.h"

// The launcher's own version stamp (launcher/CMakeLists.txt, cmake/version.cmake): "0.4.1", "dev-...", ...
extern "C" const char launcher_version[];

namespace psxstack {

struct Version {
    int major = 0, minor = 0, patch = 0;
};
// "X.Y.Z" or "vX.Y.Z" and nothing else (no "-3-gabc", no "-dirty", no "dev-"): true, with the three numbers.
bool version_parse(const std::string &text, Version *out);
int version_compare(const Version &a, const Version &b); // < 0, 0, > 0
std::string version_text(const Version &v);              // "X.Y.Z"

// The release packages' names (GAME_CONTRACT.md "Releases"), `version` without its v: `<id>-<version>-x86_64.AppImage`
// on Linux, `<id>-<version>-windows-x86_64.zip` on Windows. The one-argument form: this game, this platform.
std::string update_asset_name(const std::string &id, const std::string &version, bool windows);
std::string update_asset_name(const std::string &version);

std::string update_api_url(const std::string &repo);      // https://api.github.com/repos/<repo>/releases/latest
std::string update_releases_url(const std::string &repo); // https://github.com/<repo>/releases/latest (in a browser)

// A release's package (the self-update, issue #77, downloads and verifies it).
struct ReleaseAsset {
    std::string name;
    std::string url;     // browser_download_url
    int64_t size = -1;
    std::string sha256;  // the asset's `digest` ("sha256:<hex>") as 64 lower-case hex digits; "" when GitHub gave none
};

struct Release {
    std::string tag;      // tag_name: "v0.4.1"
    Version version;      // from the tag
    std::string name;     // the release's title ("" when none)
    std::string html_url; // its page
    bool has_asset = false;
    ReleaseAsset asset;   // this platform's package (update_asset_name), when has_asset
};

// GitHub's releases/latest answer: the tag (vX.Y.Z), the page and the package named update_asset_name(id, the tag's
// version, windows). False with why in `err` (not JSON, no tag, a tag that is not a version).
bool update_parse_release(const std::string &json, const std::string &id, bool windows, Release *out,
                          std::string *err);

class UpdateCheck {
public:
    enum class State {
        Idle,     // not checked yet
        Checking, // the request is running
        UpToDate, // the latest release is not newer than this launcher
        Newer,    // the latest release is newer: release()
        DevBuild, // this launcher's stamp is not X.Y.Z: release() is only reported
        NoCurl,   // curl could not be run: message()
        Failed,   // the request or its answer failed: message()
    };

    // What the outcome of a request is, for a launcher stamped `current` (the check's own logic, also the self-test's).
    static State evaluate(const HttpResult &r, const std::string &repo, const std::string &current, Release *release,
                          std::string *message);

    // The repository ("" = no update check: start() does nothing) and the stamp compared. The defaults: the
    // description's repository and this build's stamp.
    void configure(const std::string &repo, const std::string &current);
    bool available() const { return !repo_.empty(); }
    // Starts the request on a thread (a check running is restarted).
    void start();
    // Once a frame: takes the answer when it has come. Returns the state.
    State poll();
    State state() const { return state_; }
    bool checking() const { return state_ == State::Checking; }
    const std::string &repo() const { return repo_; }
    const std::string &current() const { return current_; }
    const Release &release() const { return release_; } // UpToDate, Newer, DevBuild
    const std::string &message() const { return message_; } // NoCurl, Failed
    SDL_Time checked_at() const { return checked_at_; } // when the last answer came (0: never)
    // The state in words, for the Settings screen ("0.5.0 is available (you have 0.4.1)").
    std::string summary() const;
    // The page to open: the release's, else the repository's latest release.
    std::string release_page() const;

private:
    std::string repo_ = GAME_UPDATES_GITHUB, current_ = launcher_version;
    State state_ = State::Idle;
    HttpGet get_;
    Release release_;
    std::string message_;
    SDL_Time checked_at_ = 0;
};

} // namespace psxstack
