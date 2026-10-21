#include "http.h"

#include <cstdlib>

#include "brand.h"

// The launcher's own version stamp (launcher/CMakeLists.txt, cmake/version.cmake).
extern "C" const char launcher_version[];

namespace psxstack {

// What `-w` appends after the body: a line of its own the answer cannot end with by chance.
static const char WRITE_OUT[] = "\\n@psxstack-http-code %{http_code}\\n";
static const char MARKER[] = "\n@psxstack-http-code ";

std::string http_curl_program() {
    if (const char *fake = SDL_getenv(ENV_FAKE_CURL)) {
        if (*fake != '\0') {
            return fake;
        }
    }
    return "curl";
}

std::string http_user_agent() {
    return std::string(LAUNCHER_NAME) + "/" + launcher_version;
}

// Reads `io` to its end (a pipe; it may be non-blocking).
static std::string read_all(SDL_IOStream *io) {
    std::string out;
    if (io == nullptr) {
        return out;
    }
    size_t n = 0;
    if (void *data = SDL_LoadFile_IO(io, &n, false)) {
        out.assign((const char *)data, n);
        SDL_free(data);
    }
    return out;
}

// curl's error, "curl: (N) words", from its stderr ("" when it wrote none).
static std::string curl_error(const std::string &err) {
    size_t pos = 0;
    while (pos < err.size()) {
        size_t eol = err.find('\n', pos);
        std::string line = err.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? err.size() : eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.compare(0, 7, "curl: (") == 0) {
            return line.substr(6);
        }
    }
    return "";
}

// Runs the curl program with `args`, to its end. With `slot`, the process is published there under `mutex` while it
// runs (CurlJob::cancel kills it), and a `cancel` already set stops it before it starts.
static HttpResult curl_run(const std::vector<std::string> &args, std::mutex *mutex, SDL_Process **slot,
                           const std::atomic<bool> *cancel) {
    HttpResult r;
    const std::string program = http_curl_program();
    std::vector<std::string> all = { program };
    all.insert(all.end(), args.begin(), args.end());
    all.push_back("-w");
    all.push_back(WRITE_OUT);
    std::vector<const char *> argv;
    for (const std::string &a : all) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_APP);
#ifdef SDL_PLATFORM_WINDOWS
    // CREATE_NO_WINDOW: curl.exe is a console program, and the launcher has no console to lend it.
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
    SDL_Process *p = nullptr;
    {
        std::unique_lock<std::mutex> lock;
        if (mutex != nullptr) {
            lock = std::unique_lock<std::mutex>(*mutex);
        }
        if (cancel != nullptr && *cancel) {
            SDL_DestroyProperties(props);
            r.status = HttpResult::Status::Cancelled;
            r.message = "cancelled";
            return r;
        }
        p = SDL_CreateProcessWithProperties(props);
        if (slot != nullptr) {
            *slot = p;
        }
    }
    SDL_DestroyProperties(props);
    if (p == nullptr) {
        r.status = HttpResult::Status::NoCurl;
        r.message = program + " could not be run (" + SDL_GetError() + ")";
        return r;
    }
    size_t n = 0;
    int code = 0;
    std::string out;
    if (void *data = SDL_ReadProcess(p, &n, &code)) { // stdout to its end, then the exit
        out.assign((const char *)data, n);
        SDL_free(data);
    }
    const std::string err =
        read_all((SDL_IOStream *)SDL_GetPointerProperty(SDL_GetProcessProperties(p), SDL_PROP_PROCESS_STDERR_POINTER,
                                                        nullptr));
    {
        std::unique_lock<std::mutex> lock;
        if (mutex != nullptr) {
            lock = std::unique_lock<std::mutex>(*mutex);
        }
        if (slot != nullptr) {
            *slot = nullptr;
        }
    }
    SDL_DestroyProcess(p);

    const size_t mark = out.rfind(MARKER);
    const std::string curl_err = curl_error(err);
    if (mark != std::string::npos) {
        r.http_code = std::atoi(out.c_str() + mark + SDL_strlen(MARKER));
        out.resize(mark);
    }
    if (cancel != nullptr && *cancel) {
        r.status = HttpResult::Status::Cancelled;
        r.message = "cancelled";
    } else if (mark == std::string::npos && code == 127 && curl_err.empty()) {
        r.status = HttpResult::Status::NoCurl; // a shell's "command not found" (posix_spawnp reports it itself)
        r.message = program + " could not be run (status 127)";
    } else if (!curl_err.empty()) {
        r.message = "curl: " + curl_err;
    } else if (mark == std::string::npos) {
        std::string what = err;
        while (!what.empty() && (what.back() == '\n' || what.back() == '\r')) {
            what.pop_back();
        }
        r.message = program + " gave no answer" + (what.empty() ? std::string() : ": " + what);
    } else if (r.http_code < 200 || r.http_code > 299) {
        r.message = "the server answered HTTP " + std::to_string(r.http_code);
    } else {
        r.status = HttpResult::Status::Ok;
        r.body = std::move(out);
    }
    return r;
}

// A GET's arguments: fail on an HTTP error, silent but for errors, follow redirects; 10 s; at most 8 MiB. Every run's
// arguments start with -fsSL: that is how the launcher, started as curl's stand-in by the self-test, knows (main.cpp).
static std::vector<std::string> get_args(const std::string &url, const std::vector<std::string> &headers) {
    std::vector<std::string> args = { "-fsSL", "--max-time", "10", "--max-filesize", "8388608", "-A",
                                      http_user_agent() };
    for (const std::string &h : headers) {
        args.push_back("-H");
        args.push_back(h);
    }
    args.push_back(url);
    return args;
}

HttpResult http_get(const std::string &url, const std::vector<std::string> &headers) {
    return curl_run(get_args(url, headers), nullptr, nullptr, nullptr);
}

// ---- the worker thread

CurlJob::~CurlJob() {
    stop();
}

void CurlJob::stop() {
    cancel();
    join();
    if (state_ == State::Running) {
        state_ = State::Finished;
    }
}

void CurlJob::start(const std::vector<std::string> &args) {
    stop();
    args_ = args;
    cancel_ = false;
    done_ = false;
    result_ = HttpResult();
    state_ = State::Running;
    thread_ = SDL_CreateThread(thread_main, "curl", this);
    if (thread_ == nullptr) {
        result_.message = std::string("cannot start a thread: ") + SDL_GetError();
        state_ = State::Finished;
    }
}

void CurlJob::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancel_ = true;
    if (proc_ != nullptr) {
        SDL_KillProcess(proc_, true);
    }
}

void CurlJob::join() {
    if (thread_ != nullptr) {
        SDL_WaitThread(thread_, nullptr);
        thread_ = nullptr;
    }
}

CurlJob::State CurlJob::poll() {
    if (state_ == State::Running && done_) {
        join();
        state_ = State::Finished;
    }
    return state_;
}

int CurlJob::thread_main(void *self) {
    CurlJob *job = static_cast<CurlJob *>(self);
    HttpResult r = curl_run(job->args_, &job->mutex_, &job->proc_, &job->cancel_);
    job->result_ = std::move(r);
    job->done_ = true;
    return 0;
}

void HttpGet::start(const std::string &url, const std::vector<std::string> &headers) {
    job_.start(get_args(url, headers));
}

// ---- downloads

HttpDownload::~HttpDownload() {
    if (job_.state() == State::Running) {
        job_.stop();
        if (job_.result().status != HttpResult::Status::Ok || (expected_ > 0 && received() != expected_)) {
            SDL_RemovePath(path_.c_str());
        }
    }
}

void HttpDownload::start(const std::string &url, const std::string &path, int64_t expected_size) {
    job_.stop(); // a download still going: its process ended before its file is replaced
    path_ = path;
    expected_ = expected_size > 0 ? expected_size : 0;
    SDL_RemovePath(path.c_str());
    std::vector<std::string> args = { "-fsSL", "--connect-timeout", "10", "--speed-limit", "1024", "--speed-time",
                                      "30", "-A", http_user_agent() };
    if (expected_ > 0) {
        args.push_back("--max-filesize");
        args.push_back(std::to_string(expected_ + (1 << 20)));
    }
    args.push_back("-o");
    args.push_back(path);
    args.push_back(url);
    job_.start(args);
}

int64_t HttpDownload::received() const {
    SDL_PathInfo info;
    if (path_.empty() || !SDL_GetPathInfo(path_.c_str(), &info) || info.type != SDL_PATHTYPE_FILE) {
        return 0;
    }
    return (int64_t)info.size;
}

float HttpDownload::progress() const {
    if (expected_ <= 0) {
        return 0.0f;
    }
    const double p = (double)received() / (double)expected_;
    return p < 0 ? 0.0f : p > 1 ? 1.0f : (float)p;
}

HttpDownload::State HttpDownload::poll() {
    const State before = job_.state();
    const State now = job_.poll();
    if (before == State::Running && now == State::Finished) {
        HttpResult &r = job_.result();
        const int64_t got = received();
        if (r.status == HttpResult::Status::Ok && expected_ > 0 && got != expected_) {
            r.status = HttpResult::Status::Failed;
            r.message = "the download has " + std::to_string(got) + " bytes, not " + std::to_string(expected_);
        }
        if (r.status != HttpResult::Status::Ok) {
            SDL_RemovePath(path_.c_str());
        }
    }
    return now;
}

} // namespace psxstack
