// The disc check (DECISIONS "Launcher and mods"): the launcher's own .cue reader
// (the same rules as the runtime's disc.c: the first FILE line names the BIN, relative to the cue's directory) and the
// runtime's SHA-1 (runtime/sha1.c) over the whole BIN, on a worker thread. The game checks the disc again itself: the
// launcher's check is there to tell the user early, and to keep a wrong file out of the settings.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <SDL3/SDL.h>

#include "brand.h"

namespace psxstack {

// The accepted discs (brand.h GAME_DISCS: the game's description). disc_by_sha1: the disc with that SHA-1, or null.
// disc_known: a stored SHA-1 and BIN size name one of them (the Verified status without hashing again).
// disc_labels: the labels joined ("the unpatched European disc (SLES-03936)"; "A or B"), for the messages.
const PsxstackGameDisc *disc_by_sha1(const std::string &sha1);
bool disc_known(const std::string &sha1, int64_t size);
std::string disc_labels();

// The BIN behind `path`: the .cue's first FILE (resolved against the cue's directory), or `path` itself. False with
// the reason in `err`.
bool disc_bin_path(const std::string &path, std::string *bin, std::string *err);
// The BIN's size, or -1.
int64_t disc_file_size(const std::string &path);

class DiscCheck {
public:
    enum class State { Idle, Running, Passed, Failed, Cancelled };

    ~DiscCheck();
    // Starts hashing `path` (.cue or .bin) on a thread; a check already running is cancelled first.
    void start(const std::string &path);
    void cancel();
    // Joins the thread once it has finished; call once a frame. Returns the state.
    State poll();

    State state() const { return state_; }
    const std::string &path() const { return path_; } // the path the check was started with
    float progress() const;                           // 0..1 while running
    std::string sha1() const;                         // when Passed or Failed (a hash was computed)
    std::string message() const;                     // when Failed: why

private:
    static int thread_main(void *self);
    void run();
    void join();

    SDL_Thread *thread_ = nullptr;
    State state_ = State::Idle;
    std::string path_;
    std::atomic<bool> cancel_{ false };
    std::atomic<bool> done_{ false };
    std::atomic<uint64_t> read_{ 0 }, total_{ 1 };
    mutable std::mutex mutex_; // guards the two strings below (written by the thread)
    std::string sha1_, message_;
    bool passed_ = false;
};

} // namespace psxstack
