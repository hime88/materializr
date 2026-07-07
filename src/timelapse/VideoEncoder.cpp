#include "VideoEncoder.h"
#include "platform_defs.h"

#include <cstdio>
#include <cstring>

#if !defined(_WIN32) && !defined(MZ_MOBILE)
#define MZ_HAVE_FFMPEG_PIPE 1
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace materializr {

#if defined(MZ_HAVE_FFMPEG_PIPE)

namespace {

// Locate ffmpeg on PATH without invoking a shell.
std::string findFfmpeg() {
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return {};
    std::string path(pathEnv);
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        if (end > start) {
            std::string cand = path.substr(start, end - start) + "/ffmpeg";
            if (access(cand.c_str(), X_OK) == 0) return cand;
        }
        start = end + 1;
    }
    return {};
}

} // namespace

VideoEncoder::~VideoEncoder() {
    if (m_pipe >= 0) close(m_pipe);
    if (m_pid > 0) {
        int status = 0;
        waitpid(pid_t(m_pid), &status, 0);
    }
}

bool VideoEncoder::available() { return !findFfmpeg().empty(); }

bool VideoEncoder::begin(const std::string& path, int width, int height,
                         int fps, bool fragmented) {
    if (m_pipe >= 0 || width <= 0 || height <= 0) return false;
    const std::string ffmpeg = findFfmpeg();
    if (ffmpeg.empty()) return false;

    // A dying ffmpeg must surface as a write() error, not kill the app.
    std::signal(SIGPIPE, SIG_IGN);

    int fds[2];
    if (pipe(fds) != 0) return false;

    char size[32], rate[16];
    std::snprintf(size, sizeof(size), "%dx%d", width, height);
    std::snprintf(rate, sizeof(rate), "%d", fps);
    // Fragmented segments survive a crash (readable up to the last fragment);
    // one-shot exports keep the streamable faststart layout.
    const char* movflags =
        fragmented ? "+frag_keyframe+empty_moov" : "+faststart";
    // yuv420p requires even dimensions; the scale filter floors odd sizes.
    const char* argv[] = {
        ffmpeg.c_str(), "-y",           "-loglevel", "error",
        "-f",           "rawvideo",     "-pix_fmt",  "rgba",
        "-s",           size,           "-r",        rate,
        "-i",           "-",
        "-vf",          "scale=trunc(iw/2)*2:trunc(ih/2)*2",
        "-c:v",         "libx264",      "-preset",   "veryfast",
        "-crf",         "20",           "-pix_fmt",  "yuv420p",
        "-movflags",    movflags,       path.c_str(), nullptr};

    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        // Child: pipe → stdin, quiet stdout, exec ffmpeg (argv array, no shell).
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        close(fds[1]);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execv(ffmpeg.c_str(), const_cast<char* const*>(argv));
        _exit(127);
    }
    close(fds[0]);
    m_pipe = fds[1];
    m_pid = pid;
    m_frameBytes = size_t(width) * height * 4;
    return true;
}

bool VideoEncoder::addFrame(const uint8_t* rgba) {
    if (m_pipe < 0 || !rgba) return false;
    size_t off = 0;
    while (off < m_frameBytes) {
        const ssize_t n = write(m_pipe, rgba + off, m_frameBytes - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += size_t(n);
    }
    return true;
}

bool VideoEncoder::end() {
    if (m_pipe < 0) return false;
    close(m_pipe);
    m_pipe = -1;
    int status = 0;
    waitpid(pid_t(m_pid), &status, 0);
    m_pid = -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

namespace {

// Spawn ffmpeg with the given arguments (no stdin pipe) and wait. argv-array
// exec, no shell — same posture as the streaming path above.
bool runFfmpeg(const std::vector<std::string>& args) {
    const std::string ffmpeg = findFfmpeg();
    if (ffmpeg.empty()) return false;
    std::vector<const char*> argv;
    argv.push_back(ffmpeg.c_str());
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execv(ffmpeg.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

bool VideoEncoder::concatSegments(const std::vector<std::string>& segments,
                                  const std::string& outPath, int totalFrames,
                                  int condenseSeconds, std::string* err) {
    if (segments.empty()) {
        if (err) *err = "Nothing recorded yet.";
        return false;
    }
    // ffmpeg concat demuxer wants a list file: file '<path>' per line.
    const std::string listPath = outPath + ".list.txt";
    {
        FILE* fp = std::fopen(listPath.c_str(), "w");
        if (!fp) {
            if (err) *err = "Couldn't write " + listPath;
            return false;
        }
        for (const auto& s : segments)
            std::fprintf(fp, "file '%s'\n", s.c_str());
        std::fclose(fp);
    }
    std::vector<std::string> args = {"-y",    "-loglevel", "error",
                                     "-f",    "concat",    "-safe",
                                     "0",     "-i",        listPath};
    const int targetFrames = condenseSeconds * 30;
    if (condenseSeconds > 0 && totalFrames > targetFrames) {
        char setpts[64];
        std::snprintf(setpts, sizeof(setpts), "setpts=%.6f*PTS",
                      double(targetFrames) / double(totalFrames));
        const std::vector<std::string> tail = {
            "-vf",  setpts,      "-r",        "30",       "-c:v",
            "libx264", "-preset", "veryfast", "-crf",     "20",
            "-pix_fmt", "yuv420p", "-movflags", "+faststart", outPath};
        args.insert(args.end(), tail.begin(), tail.end());
    } else {
        const std::vector<std::string> tail = {"-c", "copy", "-movflags",
                                               "+faststart", outPath};
        args.insert(args.end(), tail.begin(), tail.end());
    }
    const bool ok = runFfmpeg(args);
    std::remove(listPath.c_str());
    if (!ok && err) *err = "ffmpeg failed to assemble the video.";
    return ok;
}

#elif !defined(MZ_IOS) && !defined(__ANDROID__)
// Stubs: Windows only (iOS: ios_videowriter.mm, Android: android_videowriter.cpp)

VideoEncoder::~VideoEncoder() = default;
bool VideoEncoder::available() { return false; }
bool VideoEncoder::begin(const std::string&, int, int, int, bool) {
    return false;
}
bool VideoEncoder::addFrame(const uint8_t*) { return false; }
bool VideoEncoder::end() { return false; }
bool VideoEncoder::concatSegments(const std::vector<std::string>&,
                                  const std::string&, int, int,
                                  std::string* err) {
    if (err) *err = "Video export is not available on this platform.";
    return false;
}

#endif

} // namespace materializr
