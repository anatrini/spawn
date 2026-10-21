// Tests for proc, without Max. Build with -DSPAWN_TESTS=ON, run ./test_proc.

#include "proc.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <string>
#include <thread>
#include <vector>

using namespace spawn;
using clk = std::chrono::steady_clock;

static int failures = 0;

static void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

struct Run {
    std::vector<Event> events;
    std::vector<double> times;  // seconds after start, for each event
    double seconds = 0;
};

// Runs `cmd`, optionally calls stop() after `stop_after` seconds, waits for the end.
static Run run(const std::string& cmd, double stop_after = -1, const std::string& cwd = "")
{
    Proc p(nullptr);
    Run r;
    std::string err;
    auto t0 = clk::now();
    auto secs = [&] { return std::chrono::duration<double>(clk::now() - t0).count(); };
    if (!p.start(cmd, cwd, err)) {
        std::printf("start failed: %s\n", err.c_str());
        return r;
    }
    bool stopped = false;
    while (true) {
        std::vector<Event> ev;
        p.drain(ev);
        for (auto& e : ev) {
            r.events.push_back(e);
            r.times.push_back(secs());
        }
        if (!ev.empty() && ev.back().kind != Event::Line)
            break;
        if (stop_after >= 0 && !stopped && secs() >= stop_after) {
            p.stop();
            stopped = true;
        }
        if (secs() > 10)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    r.seconds = secs();
    return r;
}

static std::vector<std::string> lines(const Run& r)
{
    std::vector<std::string> out;
    for (auto& e : r.events)
        if (e.kind == Event::Line)
            out.push_back(e.line);
    return out;
}

static const Event* last(const Run& r) { return r.events.empty() ? nullptr : &r.events.back(); }

int main()
{
    // Block the usual signals in this thread, as a host application may do;
    // the child must still receive them.
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGINT);
    sigaddset(&s, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &s, nullptr);

    {
        Run r = run("echo hello");
        check(lines(r) == std::vector<std::string>{"hello"}, "echo hello -> one line");
        check(last(r) && last(r)->kind == Event::Exit && last(r)->code == 0, "exit 0");
    }
    {
        Run r = run("echo a; echo b 1>&2; echo c; exit 3");
        check(lines(r) == std::vector<std::string>{"a", "b", "c"}, "stdout and stderr in order");
        check(last(r) && last(r)->kind == Event::Exit && last(r)->code == 3, "exit 3");
    }
    {
        Run r = run("/no/such/program");
        check(last(r) && last(r)->kind == Event::Exit && last(r)->code == 127, "missing program -> exit 127");
        check(!lines(r).empty(), "missing program -> error line from sh");
    }
    const char* py = std::getenv("SPAWN_TEST_PYTHON");  // e.g. the python of a conda env
    if (!py)
        std::printf("skip  python checks: set SPAWN_TEST_PYTHON to a python interpreter\n");
    if (py) {
        Run r = run(std::string(py) + " -c \"import time\nfor i in range(5):\n print(i); time.sleep(0.2)\"");
        bool spread = r.times.size() >= 5 && r.times[4] - r.times[0] > 0.6;
        check(lines(r).size() == 5 && spread, "python lines arrive as printed, not at exit");
    }
    if (py) {
        Run r = run(std::string(py) + " -c \"import time\nwhile True: time.sleep(0.1)\"", 0.5);
        check(r.seconds < 1.4, "stop ends a python loop quickly (SIGINT)");
        check(last(r) && (last(r)->kind == Event::Signal || last(r)->code != 0), "python stopped: non-zero end");
    }
    {
        Run r = run("trap '' INT TERM; while true; do sleep 0.1; done", 0.3);
        check(r.seconds > 3.0 && r.seconds < 4.0, "a command ignoring INT and TERM is killed after 3 s");
        check(last(r) && last(r)->kind == Event::Signal && last(r)->code == SIGKILL, "ended by SIGKILL");
    }
    {
        Run r = run("pwd", -1, "/tmp");
        check(!lines(r).empty() && (lines(r)[0] == "/tmp" || lines(r)[0] == "/private/tmp"), "cwd /tmp");
    }
    {
        Run r = run("sleep 5 & echo started");
        check(r.seconds < 1.0, "a background child holding the pipe does not block the end");
    }
    {
        auto t0 = clk::now();
        {
            Proc p(nullptr);
            std::string err;
            p.start("trap '' INT; sleep 30", "", err);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }  // destructor
        double secs = std::chrono::duration<double>(clk::now() - t0).count();
        check(secs < 1.5, "destructor stops a running command within ~1 s");
    }
    {
        Proc p(nullptr);
        std::string err;
        p.start("sleep 1", "", err);
        bool second = p.start("echo no", "", err);
        check(!second && !err.empty(), "second start while running is refused");
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
