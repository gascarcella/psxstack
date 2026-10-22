#include "steam_section.h"

#include <cstdio>

#include "imgui.h"

#include "paths.h"
#include "style.h"
#include "widgets.h"

namespace psxstack {

void SteamSection::configure(const SteamOptions &options, SettingsFile *settings, std::function<void()> save) {
    options_ = options;
    settings_ = settings;
    save_ = std::move(save);
}

void SteamSection::say(const std::string &text, bool ok) {
    message_ = text;
    message_ok_ = ok;
    std::fprintf(stderr, "launcher: steam: %s\n", text.c_str());
}

bool SteamSection::moved() const {
    const SteamRecord &r = settings_->values.steam;
    return r.added() && r.exe != options_.launcher_path();
}

bool SteamSection::steam_closed(const char *what) {
    if (!options_.running()) {
        return true;
    }
    say(std::string("Steam is running. Quit Steam first (Steam > Exit; on the Steam Deck, from the desktop), then "
                    "press ") +
            what + " again: Steam keeps its shortcuts in memory and writes them back when it exits, which would undo "
                   "the change.",
        false);
    return false;
}

static std::string account_label(const SteamAccount &a) {
    return a.name.empty() ? "account " + a.id : a.name + " (" + a.id + ")";
}

void SteamSection::begin_add() {
    const SteamLookup in = options_.lookup();
    installs_ = steam_find(in);
    looked_in_.clear();
    for (const auto &c : in.candidates) {
        looked_in_ += (looked_in_.empty() ? "" : ", ") + c.first;
    }
    choosing_ = true;
    message_.clear();
    install_ = account_ = -1;
    // The first folder with an account, its most recent account.
    for (size_t i = 0; i < installs_.size() && install_ < 0; i++) {
        if (!installs_[i].accounts.empty()) {
            choose((int)i, installs_[i].default_account());
        }
    }
    if (installs_.empty()) {
        say("Steam was not found" + (looked_in_.empty() ? std::string(".") : " (looked in " + looked_in_ + ").") +
                " Install Steam and log in once, then try again.",
            false);
    } else if (install_ < 0) {
        say("No account has logged in to Steam here yet (" + installs_[0].dir + "). Log in once, then try again.",
            false);
    }
}

void SteamSection::choose(int install, int account) {
    if (install < 0 || install >= (int)installs_.size()) {
        return;
    }
    install_ = install;
    const int n = (int)installs_[install].accounts.size();
    account_ = account >= 0 && account < n ? account : installs_[install].default_account();
}

void SteamSection::cancel() {
    choosing_ = false;
    installs_.clear();
    install_ = account_ = -1;
    message_.clear();
}

bool SteamSection::write_shortcut(const std::string &dir, const std::string &account,
                                  const std::string &account_name, const SteamRecord *replace, const char *verb,
                                  std::string *note) {
    const std::string not_done = std::string("Not ") + verb + ": ";
    // The account's folder must be there: the launcher never makes one for an account Steam no longer has.
    if (!path_is_dir(path_join(path_join(dir, "userdata"), account))) {
        say(not_done + "Steam's folder for the account is gone (" + path_join(path_join(dir, "userdata"), account) +
                ").",
            false);
        return false;
    }
    const std::string path = steam_shortcuts_path(dir, account);
    VdfNode root;
    std::string err;
    if (!steam_shortcuts_load(path, &root, &err)) {
        say(not_done + err, false);
        return false;
    }
    SteamShortcut s;
    s.name = options_.name;
    s.exe = options_.launcher_path();
    if (s.exe.empty()) {
        say(not_done + "the launcher's own path could not be found.", false);
        return false;
    }
    // The icon picture, when the game has one, is also the shortcut's icon.
    const std::string grid = steam_grid_dir(dir, account);
    for (const SteamArt &a : options_.art) {
        if (a.suffix == "_icon.png") {
            s.icon = path_join(grid, std::to_string(steam_shortcut_appid(s.exe, s.name)) + a.suffix);
        }
    }
    if (replace != nullptr) {
        steam_shortcuts_remove(&root, replace->appid);
    }
    const uint32_t appid = steam_shortcuts_add(&root, s);
    if (!steam_shortcuts_save(path, root, &err)) {
        say(not_done + err, false);
        return false;
    }
    std::vector<std::string> written;
    note->clear();
    if (!steam_art_write(grid, appid, options_.art, &written, &err)) {
        *note = " Its pictures could not all be written (" + err + "): Steam shows its own for those.";
    }
    if (replace != nullptr) {
        steam_art_remove(steam_grid_dir(replace->dir, replace->account), replace->art);
    }
    SteamRecord &r = settings_->values.steam;
    r.dir = dir;
    r.account = account;
    r.account_name = account_name;
    r.appid = appid;
    r.exe = s.exe;
    r.name = s.name;
    r.art = written;
    save_();
    return true;
}

bool SteamSection::add() {
    if (install_ < 0 || account_ < 0 || !steam_closed("Add to Steam")) {
        return false;
    }
    const std::string dir = installs_[install_].dir;
    const SteamAccount a = installs_[install_].accounts[account_]; // a copy: the list is cleared below
    std::string note;
    if (!write_shortcut(dir, a.id, a.name, nullptr, "added", &note)) {
        return false;
    }
    choosing_ = false;
    installs_.clear();
    say("Added to Steam for " + account_label(a) +
            ". Start Steam: the game is in the library (with the non-Steam games), and starts this launcher." + note,
        true);
    return true;
}

bool SteamSection::remove_shortcut(int *removed) {
    const SteamRecord &r = settings_->values.steam;
    const std::string path = steam_shortcuts_path(r.dir, r.account);
    *removed = 0;
    if (!path_is_file(path)) {
        return true; // Steam's folder or the account is gone: nothing to take out
    }
    VdfNode root;
    std::string err;
    if (!steam_shortcuts_load(path, &root, &err)) {
        say("Not removed: " + err, false);
        return false;
    }
    *removed = steam_shortcuts_remove(&root, r.appid);
    if (*removed > 0 && !steam_shortcuts_save(path, root, &err)) {
        say("Not removed: " + err, false);
        return false;
    }
    return true;
}

bool SteamSection::remove() {
    SteamRecord &r = settings_->values.steam;
    if (!r.added() || !steam_closed("Remove from Steam")) {
        return false;
    }
    int removed = 0;
    if (!remove_shortcut(&removed)) {
        return false;
    }
    steam_art_remove(steam_grid_dir(r.dir, r.account), r.art);
    r = SteamRecord();
    save_();
    say(removed > 0 ? "Removed from Steam."
                    : "It was no longer in Steam's shortcuts (removed in Steam, or the account is gone); forgotten here.",
        true);
    return true;
}

bool SteamSection::update_shortcut() {
    SteamRecord &r = settings_->values.steam;
    if (!r.added() || !steam_closed("Update shortcut")) {
        return false;
    }
    const SteamRecord old = r;
    std::string note;
    if (!write_shortcut(old.dir, old.account, old.account_name, &old, "updated", &note)) {
        return false;
    }
    say("The shortcut now starts " + r.exe + "." + note, true);
    return true;
}

void SteamSection::draw(float label_w) {
    const SteamRecord &r = settings_->values.steam;
    section_title("Steam");
    ImGui::BeginDisabled(!settings_->writable());
    ImGui::TextDisabled("Library");
    ImGui::SameLine(label_w);
    ImGui::PushTextWrapPos(0);
    if (r.added()) {
        ImGui::Text("In Steam, for %s", r.account_name.empty() ? ("account " + r.account).c_str()
                                                               : (r.account_name + " (" + r.account + ")").c_str());
        ImGui::Indent(label_w);
        text_fit_left("", r.dir);
        if (moved()) {
            ImGui::TextColored(rgb(ACCENT), "The launcher has moved since it was added (the shortcut starts %s).",
                               r.exe.c_str());
            if (ImGui::Button("Update shortcut")) {
                update_shortcut();
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Remove from Steam")) {
            remove();
        }
        ImGui::Unindent(label_w);
    } else if (!choosing_) {
        ImGui::TextUnformatted("Not in Steam");
        ImGui::Indent(label_w);
        ImGui::TextDisabled("Adds the game to your Steam library as a non-Steam game, for Steam's Game Mode, controller "
                            "settings and overlay. Steam must be closed.");
        if (ImGui::Button("Add to Steam")) {
            begin_add();
        }
        ImGui::Unindent(label_w);
    } else {
        ImGui::TextUnformatted("Add to Steam");
        ImGui::Indent(label_w);
        if (installs_.size() > 1) {
            const SteamInstall *cur = install_ >= 0 ? &installs_[install_] : nullptr;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##steam_dir", cur != nullptr ? (cur->where + ": " + cur->dir).c_str() : "")) {
                for (size_t i = 0; i < installs_.size(); i++) {
                    const SteamInstall &in = installs_[i];
                    ImGui::BeginDisabled(in.accounts.empty());
                    if (ImGui::Selectable((in.where + ": " + in.dir).c_str(), (int)i == install_)) {
                        choose((int)i, -1);
                    }
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
        } else if (install_ >= 0) {
            text_fit_left("", installs_[install_].dir);
        }
        if (install_ >= 0) {
            const std::vector<SteamAccount> &accounts = installs_[install_].accounts;
            if (accounts.size() > 1) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##steam_account", account_label(accounts[account_]).c_str())) {
                    for (size_t i = 0; i < accounts.size(); i++) {
                        if (ImGui::Selectable(account_label(accounts[i]).c_str(), (int)i == account_)) {
                            choose(install_, (int)i);
                        }
                    }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::Text("For %s", account_label(accounts[account_]).c_str());
            }
        }
        if (install_ >= 0 && account_ >= 0) {
            if (ImGui::Button("Add")) {
                add();
            }
            ImGui::SameLine();
        }
        if (ImGui::Button(install_ >= 0 ? "Cancel" : "Close")) {
            cancel();
        }
        ImGui::Unindent(label_w);
    }
    if (!message_.empty()) {
        ImGui::Indent(label_w);
        ImGui::TextColored(message_ok_ ? ImGui::GetStyle().Colors[ImGuiCol_Text] : rgb(ACCENT), "%s",
                           message_.c_str());
        ImGui::Unindent(label_w);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndDisabled();
}

} // namespace psxstack
