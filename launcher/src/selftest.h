// The launcher's self-test (like the port's --input-test): no disc, no display needed (SDL's offscreen driver), so
// CI runs it. It checks the path helpers, the JSON writer, the settings directory's lookup order, the settings
// file's round trips (unknown members kept, a broken file kept aside, a newer schema never written), then opens the
// window and walks every screen with injected key events and a virtual gamepad, saving each screen as a PNG in
// <dir>/screens/.
#pragma once

#include <string>

#include "brand.h"

namespace psxstack {

// `dir`: a scratch directory the test may fill (created when missing). True when every check passed.
bool self_test_run(const std::string &dir);

// The game's stand-in: the self-test starts this executable with SELF_TEST_GAME_ENV set to a mode, so the launch
// path (game_probe, GameRun) is tested without the game or a disc. main() calls it first.
constexpr const char *SELF_TEST_GAME_ENV = ENV_SELFTEST_FAKE_GAME; // <PREFIX>_LAUNCHER_FAKE_GAME
int self_test_fake_game(const char *mode, int argc, char **argv);

// curl's stand-in (http.h): the self-test points $<PREFIX>_LAUNCHER_FAKE_CURL at this executable and sets
// $<PREFIX>_SELFTEST_CURL_REPLY to what it answers: `[slow:]FILE` (the file as the body, or copied to -o's path; slow:
// in small pieces with pauses), `offline` (curl's "could not resolve host"), `http:N` (curl -f's error for status N).
// It honours -w's %{http_code} as curl does. A download (-o) is answered with $<PREFIX>_SELFTEST_CURL_DOWNLOAD instead
// when that is set (the release's JSON for the check, the package for Update now). main() calls it first, when the
// arguments start with -fsSL (http.cpp's always do).
int self_test_fake_curl(const char *reply, int argc, char **argv);

// selftest_update.cpp's fixtures, for the Update now flow in the window (selftest.cpp): the tar that makes and unpacks
// the zips (bsdtar, Windows' tar.exe or the stand-in), a copy of this executable (with `mark` appended: a new build),
// a zip of the folder `top` in `dir` made with that tar, and the new build's mark. The stand-in tar and the relaunched
// copy are this executable with SELF_TEST_GAME_ENV set to SELF_TEST_UPDATE_MODE.
std::string self_test_tar();
bool self_test_copy_self(const std::string &to, const std::string &mark);
bool self_test_make_zip(const std::string &tar, const std::string &dir, const std::string &top, const std::string &zip);
extern const char SELF_TEST_NEW_MARK[];
constexpr const char *SELF_TEST_UPDATE_MODE = "update";

} // namespace psxstack
