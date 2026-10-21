// HTTP through the system's curl, run as a child process (SDL_CreateProcess, as the game's probe; DECISIONS "Updates:
// GitHub releases, curl as a process"): one code path on Linux and Windows (Windows 10 1803 and later ship curl.exe),
// the OS's TLS and proxies, no HTTP library linked in. A GET to a string, and a download to a file with its progress
// (the file's size against the expected one) and a cancel (the process killed). $<PREFIX>_LAUNCHER_FAKE_CURL names
// another program to run in curl's place (the self-test's stand-in, so it never touches the network).
//
// The outcome is read from curl's own output, never from its exit status: `-w` appends the HTTP status after the
// body, and an error is curl's "curl: (N) ..." line on stderr. On Windows the process is created in the background (no
// console window flashing up over the launcher), and SDL then gives no exit status.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

namespace psxstack {

struct HttpResult {
    enum class Status {
        Ok,        // a 2xx answer (a download: the whole file, of the expected size)
        NoCurl,    // curl could not be run (not installed, not on PATH)
        Failed,    // curl ran and failed, or the server said no: `message`
        Cancelled, // cancel()
    } status = Status::Failed;
    int http_code = 0;   // the HTTP status curl reported (0: no answer)
    std::string body;    // a GET's answer
    std::string message; // when not Ok: why, in words (curl's own error when it gave one)
};

// The program run as curl: $<PREFIX>_LAUNCHER_FAKE_CURL when set, else "curl" (looked up on PATH).
std::string http_curl_program();
// "<id>-launcher/<version>": the User-Agent of every request (GitHub's API requires one).
std::string http_user_agent();

// A GET to a string: `curl -fsSL --max-time 10 --max-filesize 8 MiB -A <user agent> [-H header]... URL`. Blocking: run
// it on a worker thread (HttpGet does).
HttpResult http_get(const std::string &url, const std::vector<std::string> &headers = {});

// One curl run on a worker thread (HttpGet, HttpDownload). cancel() kills the process; the destructor cancels and
// joins.
class CurlJob {
public:
    enum class State { Idle, Running, Finished };

    ~CurlJob();
    // Starts curl with `args` (after the program's name) on a thread; a run still going is cancelled first.
    void start(const std::vector<std::string> &args);
    void cancel();
    // cancel(), then waits for the thread: the process has ended when it returns.
    void stop();
    // Joins the thread once it has finished; call once a frame. Returns the state.
    State poll();
    State state() const { return state_; }
    // When Finished.
    const HttpResult &result() const { return result_; }
    HttpResult &result() { return result_; }

private:
    static int thread_main(void *self);
    void join();

    SDL_Thread *thread_ = nullptr;
    State state_ = State::Idle;
    std::vector<std::string> args_;
    std::atomic<bool> done_{ false }, cancel_{ false };
    std::mutex mutex_;              // guards proc_ (the thread sets and clears it, cancel() kills it)
    SDL_Process *proc_ = nullptr;
    HttpResult result_;             // written by the thread until done_
};

// http_get on a worker thread.
class HttpGet {
public:
    using State = CurlJob::State;
    void start(const std::string &url, const std::vector<std::string> &headers = {});
    void cancel() { job_.cancel(); }
    State poll() { return job_.poll(); }
    State state() const { return job_.state(); }
    const HttpResult &result() const { return job_.result(); } // when Finished

private:
    CurlJob job_;
};

// A download to `path` (replaced) on a worker thread: `curl -fsSL --connect-timeout 10` (and a stalled transfer, under
// 1 KiB/s for 30 s, given up) `--max-filesize <expected + 1 MiB> -o PATH URL`. When it ends otherwise than Ok the
// partial file is removed.
class HttpDownload {
public:
    using State = CurlJob::State;
    ~HttpDownload(); // a download still going is cancelled, and its partial file removed
    // `expected_size` > 0: the size the file must have (a different one is Failed), and the progress's 100%.
    void start(const std::string &url, const std::string &path, int64_t expected_size);
    void cancel() { job_.cancel(); }
    // Once a frame; when the run has just ended, checks the file (its size) and removes a partial one.
    State poll();
    State state() const { return job_.state(); }
    const std::string &path() const { return path_; }
    int64_t expected_size() const { return expected_; }
    int64_t received() const; // the file's size so far (0 before it exists)
    float progress() const;   // 0..1 (0 without an expected size)
    const HttpResult &result() const { return job_.result(); } // when Finished

private:
    CurlJob job_;
    std::string path_;
    int64_t expected_ = 0;
};

} // namespace psxstack
