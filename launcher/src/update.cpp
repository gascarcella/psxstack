#include "update.h"

#include <cctype>

#include "json_value.h"

namespace psxstack {

// One number of a version: digits only, no sign, at most 9 of them.
static bool parse_number(const std::string &s, size_t *pos, int *out) {
    size_t start = *pos;
    int v = 0;
    while (*pos < s.size() && std::isdigit((unsigned char)s[*pos]) && *pos - start < 9) {
        v = v * 10 + (s[*pos] - '0');
        (*pos)++;
    }
    if (*pos == start || (*pos < s.size() && std::isdigit((unsigned char)s[*pos]))) {
        return false;
    }
    *out = v;
    return true;
}

bool version_parse(const std::string &text, Version *out) {
    size_t pos = !text.empty() && text[0] == 'v' ? 1 : 0;
    Version v;
    if (!parse_number(text, &pos, &v.major) || pos >= text.size() || text[pos++] != '.' ||
        !parse_number(text, &pos, &v.minor) || pos >= text.size() || text[pos++] != '.' ||
        !parse_number(text, &pos, &v.patch) || pos != text.size()) {
        return false;
    }
    *out = v;
    return true;
}

int version_compare(const Version &a, const Version &b) {
    if (a.major != b.major) {
        return a.major < b.major ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor ? -1 : 1;
    }
    return a.patch < b.patch ? -1 : a.patch > b.patch ? 1 : 0;
}

std::string version_text(const Version &v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}

std::string update_asset_name(const std::string &id, const std::string &version, bool windows) {
    return id + "-" + version + (windows ? "-windows-x86_64.zip" : "-x86_64.AppImage");
}

std::string update_asset_name(const std::string &version) {
#ifdef SDL_PLATFORM_WINDOWS
    return update_asset_name(GAME_ID, version, true);
#else
    return update_asset_name(GAME_ID, version, false);
#endif
}

std::string update_api_url(const std::string &repo) {
    return "https://api.github.com/repos/" + repo + "/releases/latest";
}

std::string update_releases_url(const std::string &repo) {
    return "https://github.com/" + repo + "/releases/latest";
}

bool update_parse_release(const std::string &json, const std::string &id, bool windows, Release *out,
                          std::string *err) {
    std::string perr;
    Json doc = Json::parse(json, &perr);
    if (!doc.is_object()) {
        *err = "the answer is not a JSON object" + (perr.empty() ? std::string() : " (" + perr + ")");
        return false;
    }
    Release r;
    const Json *tag = doc.find("tag_name");
    if (tag == nullptr || !tag->is_string()) {
        *err = "the answer has no tag_name";
        return false;
    }
    r.tag = tag->as_string();
    if (!version_parse(r.tag, &r.version)) {
        *err = "the latest release's tag, \"" + r.tag + "\", is not vX.Y.Z";
        return false;
    }
    if (const Json *v = doc.find("name")) {
        r.name = v->as_string(std::string());
    }
    if (const Json *v = doc.find("html_url")) {
        r.html_url = v->as_string(std::string());
    }
    const std::string want = update_asset_name(id, version_text(r.version), windows);
    if (const Json *assets = doc.find("assets"); assets != nullptr && assets->is_array()) {
        for (const Json &a : assets->items()) {
            const Json *name = a.find("name");
            if (name == nullptr || name->as_string(std::string()) != want) {
                continue;
            }
            r.has_asset = true;
            r.asset.name = want;
            if (const Json *v = a.find("browser_download_url")) {
                r.asset.url = v->as_string(std::string());
            }
            if (const Json *v = a.find("size"); v != nullptr && v->is_number() && v->as_number(-1) >= 0) {
                r.asset.size = (int64_t)v->as_number(-1);
            }
            if (const Json *v = a.find("digest")) {
                std::string d = v->as_string(std::string());
                bool hex = d.size() == 7 + 64 && d.compare(0, 7, "sha256:") == 0;
                for (size_t i = 7; hex && i < d.size(); i++) {
                    hex = std::isxdigit((unsigned char)d[i]) != 0;
                    d[i] = (char)std::tolower((unsigned char)d[i]);
                }
                if (hex) {
                    r.asset.sha256 = d.substr(7);
                }
            }
            break;
        }
    }
    *out = r;
    return true;
}

UpdateCheck::State UpdateCheck::evaluate(const HttpResult &r, const std::string &repo, const std::string &current,
                                         Release *release, std::string *message) {
    *release = Release();
    message->clear();
    switch (r.status) {
    case HttpResult::Status::Ok:
        break;
    case HttpResult::Status::NoCurl:
        *message = r.message;
        return State::NoCurl;
    case HttpResult::Status::Cancelled:
    case HttpResult::Status::Failed:
        if (r.http_code == 404) {
            *message = repo + " has no published release (HTTP 404)";
        } else if (r.http_code == 403 || r.http_code == 429) {
            *message = "GitHub refused the request (HTTP " + std::to_string(r.http_code) +
                       "; it allows 60 an hour from one address without an account): try again later";
        } else {
            *message = r.message;
        }
        return State::Failed;
    }
#ifdef SDL_PLATFORM_WINDOWS
    const bool windows = true;
#else
    const bool windows = false;
#endif
    if (!update_parse_release(r.body, GAME_ID, windows, release, message)) {
        return State::Failed;
    }
    Version have;
    if (!version_parse(current, &have)) {
        return State::DevBuild;
    }
    return version_compare(release->version, have) > 0 ? State::Newer : State::UpToDate;
}

void UpdateCheck::configure(const std::string &repo, const std::string &current) {
    get_.cancel();
    repo_ = repo;
    current_ = current;
    state_ = State::Idle;
    release_ = Release();
    message_.clear();
    checked_at_ = 0;
}

void UpdateCheck::start() {
    if (repo_.empty()) {
        return;
    }
    get_.start(update_api_url(repo_), { "Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28" });
    state_ = State::Checking;
}

UpdateCheck::State UpdateCheck::poll() {
    if (state_ != State::Checking || get_.poll() != HttpGet::State::Finished) {
        return state_;
    }
    state_ = evaluate(get_.result(), repo_, current_, &release_, &message_);
    if (!SDL_GetCurrentTime(&checked_at_)) {
        checked_at_ = 0;
    }
    return state_;
}

std::string UpdateCheck::summary() const {
    const std::string latest = version_text(release_.version);
    switch (state_) {
    case State::Idle:
        return "not checked yet";
    case State::Checking:
        return "checking...";
    case State::UpToDate:
        return "up to date (the latest release is " + latest + ")";
    case State::Newer:
        return latest + " is available (you have " + current_ + ")";
    case State::DevBuild:
        return "this is a development build (" + current_ + "); the latest release is " + latest;
    case State::NoCurl:
        return "curl was not found, and the check needs it: the release page has the latest";
    case State::Failed:
        return "the check failed: " + message_;
    }
    return "?";
}

std::string UpdateCheck::release_page() const {
    return release_.html_url.compare(0, 8, "https://") == 0 ? release_.html_url : update_releases_url(repo_);
}

} // namespace psxstack
