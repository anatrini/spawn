// proc - POSIX implementation (macOS). See proc.h.

#include "proc.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace spawn {

namespace {

const size_t kMaxQueued = 10000;          // lines waiting for the host; more are dropped
const size_t kMaxLine = 64 * 1024;        // a longer line without newline is cut
const int kPollMs = 50;
const long long kDrainAfterExitMs = 300;  // stop reading when only leftover processes hold the pipe

long long now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

Proc::Proc(std::function<void()> notify) : notify_(std::move(notify)) {}

Proc::~Proc()
{
    if (running_) {
        ::killpg(pid_, SIGTERM);
        kill_at_ms_ = now_ms() + 500;
    }
    if (thread_.joinable())
        thread_.join();
}

bool Proc::start(const std::string& command, const std::string& cwd, std::string& error)
{
    if (running_) {
        error = "already running: send stop first";
        return false;
    }
    if (thread_.joinable())
        thread_.join();  // the previous process has ended

    int fd[2];
    if (::pipe(fd) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    ::fcntl(fd[0], F_SETFD, FD_CLOEXEC);

    // stdin empty; stdout and stderr into the same pipe, so lines keep their order.
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 2);
    if (!cwd.empty())
        posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());

    // Own process group, so that stop() reaches the whole command; default
    // signal mask and handlers, since the host may block or catch signals;
    // no file descriptors of the host except 0, 1, 2.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                                        POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_CLOEXEC_DEFAULT);

    // Python writes to a pipe in blocks unless told otherwise; lines should
    // arrive when they are printed.
    std::vector<char*> env;
    bool has_unbuffered = false;
    for (char** e = environ; *e; ++e) {
        if (std::strncmp(*e, "PYTHONUNBUFFERED=", 17) == 0)
            has_unbuffered = true;
        env.push_back(*e);
    }
    char unbuffered[] = "PYTHONUNBUFFERED=1";
    if (!has_unbuffered)
        env.push_back(unbuffered);
    env.push_back(nullptr);

    char sh[] = "/bin/sh", dash_c[] = "-c";
    std::string cmd = command;
    char* argv[] = {sh, dash_c, cmd.data(), nullptr};

    pid_t pid = 0;
    int rc = ::posix_spawn(&pid, "/bin/sh", &fa, &attr, argv, env.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    ::close(fd[1]);
    if (rc != 0) {
        error = std::string("cannot start /bin/sh: ") + std::strerror(rc);
        ::close(fd[0]);
        return false;
    }

    pid_ = pid;
    term_at_ms_ = 0;
    kill_at_ms_ = 0;
    running_ = true;
    thread_ = std::thread(&Proc::reader, this, fd[0]);
    return true;
}

void Proc::stop()
{
    if (!running_ || term_at_ms_ != 0)
        return;
    ::killpg(pid_, SIGINT);
    long long t = now_ms();
    term_at_ms_ = t + 1000;
    kill_at_ms_ = t + 3000;
}

void Proc::drain(std::vector<Event>& out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    out.insert(out.end(), std::make_move_iterator(events_.begin()),
               std::make_move_iterator(events_.end()));
    events_.clear();
}

void Proc::push(Event e)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (e.kind == Event::Line && events_.size() >= kMaxQueued)
            return;
        events_.push_back(std::move(e));
    }
    if (notify_)
        notify_();
}

void Proc::reader(int fd)
{
    const pid_t pid = pid_;
    std::string buf;
    int status = 0;
    bool exited = false, term_sent = false, kill_sent = false;
    long long exited_at = 0;

    auto emit_lines = [&] {
        size_t start = 0, nl;
        while ((nl = buf.find('\n', start)) != std::string::npos) {
            size_t end = (nl > start && buf[nl - 1] == '\r') ? nl - 1 : nl;
            push({Event::Line, buf.substr(start, end - start)});
            start = nl + 1;
        }
        buf.erase(0, start);
        if (buf.size() > kMaxLine) {
            push({Event::Line, buf});
            buf.clear();
        }
    };

    while (true) {
        if (fd >= 0) {
            struct pollfd p = {fd, POLLIN, 0};
            if (::poll(&p, 1, kPollMs) > 0) {
                char chunk[4096];
                ssize_t got = ::read(fd, chunk, sizeof chunk);
                if (got > 0) {
                    buf.append(chunk, (size_t)got);
                    emit_lines();
                }
                else if (got == 0 || errno != EINTR) {
                    ::close(fd);
                    fd = -1;
                }
            }
        }
        else {
            ::usleep(kPollMs * 1000);
        }

        long long t = now_ms();
        if (!exited && ::waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
            exited_at = t;
        }
        if (!exited) {
            if (!term_sent && term_at_ms_ != 0 && t >= term_at_ms_) {
                ::killpg(pid, SIGTERM);
                term_sent = true;
            }
            if (!kill_sent && kill_at_ms_ != 0 && t >= kill_at_ms_) {
                ::killpg(pid, SIGKILL);
                kill_sent = true;
            }
        }
        if (exited && (fd < 0 || t - exited_at > kDrainAfterExitMs))
            break;
    }

    if (!buf.empty())
        push({Event::Line, buf});
    if (fd >= 0)
        ::close(fd);
    running_ = false;
    if (WIFSIGNALED(status))
        push({Event::Signal, "", WTERMSIG(status)});
    else
        push({Event::Exit, "", WEXITSTATUS(status)});
}

}  // namespace spawn
