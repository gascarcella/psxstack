// The game's identity for the launcher: every name, string, hash and number of a game comes from psxstack_game_gen.h
// (tools/game_gen.py over the game's description at configure time; GAME_CONTRACT.md "The generated header"). Nothing
// of a game is written in the launcher's sources.
#pragma once

#include <cstdint>
#include <string>

#include "psxstack_game_gen.h"

namespace psxstack {

constexpr const char *GAME_ID = PSXSTACK_GAME_ID;
constexpr const char *GAME_ID_UPPER = PSXSTACK_GAME_ID_UPPER;
constexpr const char *GAME_TITLE = PSXSTACK_GAME_TITLE;
constexpr const char *GAME_ABOUT = PSXSTACK_GAME_ABOUT;
constexpr const char *GAME_DISC_HINT = PSXSTACK_GAME_DISC_HINT;
constexpr const char *GAME_WEBSITE = PSXSTACK_GAME_WEBSITE;
// The update check's repository, "owner/repo" (the description's launcher.updates.github); "" = no update check.
constexpr const char *GAME_UPDATES_GITHUB = PSXSTACK_GAME_UPDATES_GITHUB;
// The executables: the game's (`<id>`) and the launcher's own (`<id>-launcher`), with .exe on Windows.
#ifdef _WIN32
constexpr const char *GAME_EXE = PSXSTACK_GAME_ID ".exe";
constexpr const char *LAUNCHER_EXE = PSXSTACK_GAME_ID "-launcher.exe";
#else
constexpr const char *GAME_EXE = PSXSTACK_GAME_ID;
constexpr const char *LAUNCHER_EXE = PSXSTACK_GAME_ID "-launcher";
#endif
constexpr const char *LAUNCHER_NAME = PSXSTACK_GAME_ID "-launcher"; // the usage, the Copy text's header
constexpr const char *WINDOW_TITLE = PSXSTACK_GAME_TITLE " launcher";
// Environment variables: <PREFIX>_GAME (the game's executable), <PREFIX>_CONFIG_DIR (the settings directory), and
// the self-test's (launcher/README.md "The self-test").
constexpr const char *ENV_GAME = PSXSTACK_GAME_ENV_PREFIX "_GAME";
constexpr const char *ENV_CONFIG_DIR = PSXSTACK_GAME_ENV_PREFIX "_CONFIG_DIR";
constexpr const char *ENV_LAUNCHER = PSXSTACK_GAME_ENV_PREFIX "_LAUNCHER"; // set for the game it starts (the in-game menu's Quit says "to launcher")
constexpr const char *ENV_SELFTEST_FAKE_GAME = PSXSTACK_GAME_ENV_PREFIX "_LAUNCHER_FAKE_GAME";
// The program run in place of curl (http.h; the self-test's stand-in), and the stand-in's answer.
constexpr const char *ENV_FAKE_CURL = PSXSTACK_GAME_ENV_PREFIX "_LAUNCHER_FAKE_CURL";
constexpr const char *ENV_SELFTEST_CURL_REPLY = PSXSTACK_GAME_ENV_PREFIX "_SELFTEST_CURL_REPLY";
constexpr const char *ENV_SELFTEST_CURL_DOWNLOAD = PSXSTACK_GAME_ENV_PREFIX "_SELFTEST_CURL_DOWNLOAD";
// "1": the update check at start answers its prompt with Update now (a test run against the real releases;
// launcher/README.md "The self-test").
constexpr const char *ENV_UPDATE_NOW = PSXSTACK_GAME_ENV_PREFIX "_LAUNCHER_UPDATE_NOW";
constexpr const char *ENV_SELFTEST_DISC = PSXSTACK_GAME_ENV_PREFIX "_SELFTEST_DISC";
constexpr const char *ENV_SELFTEST_GAME = PSXSTACK_GAME_ENV_PREFIX "_SELFTEST_GAME";
constexpr const char *ENV_SELFTEST_VIDEO_DRIVER = PSXSTACK_GAME_ENV_PREFIX "_SELFTEST_VIDEO_DRIVER";
// The rates: the nominal one (the settings' default) and the ones the Settings screen offers.
constexpr int GAME_RATE = PSXSTACK_GAME_RATE;
constexpr int GAME_RATE_COUNT = PSXSTACK_GAME_RATE_COUNT;
inline constexpr int GAME_RATES[PSXSTACK_GAME_RATE_COUNT] = PSXSTACK_GAME_RATES;
constexpr const char *GAME_RATE_NOTE = PSXSTACK_GAME_RATE_NOTE;
// The discs the game accepts (disc.h: disc_by_sha1, disc_known, disc_labels). A disc-free game (examples/hello)
// describes none: the generated list then holds one empty entry, which the count keeps out of reach.
constexpr int GAME_DISC_COUNT = PSXSTACK_GAME_DISC_COUNT;
inline constexpr PsxstackGameDisc GAME_DISCS[PSXSTACK_GAME_DISC_COUNT > 0 ? PSXSTACK_GAME_DISC_COUNT : 1] =
    PSXSTACK_GAME_DISCS;

} // namespace psxstack
