// proc - run one command line in a child process and read its output.
//
// Independent of Max and Pd. The command runs through /bin/sh -c in its own
// process group, so that stop() also reaches the processes it starts.
// A reader thread collects the output (stdout and stderr together, line by
// line) and the exit status; the host takes them with drain(), on its own
// main thread.

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace spawn {

struct Event {
    enum Kind { Line, Exit, Signal };
    Kind kind;
    std::string line;  // Line
    int code = 0;      // Exit: exit status; Signal: signal number
};

class Proc {
public:
    // notify is called from the reader thread whenever new events are queued.
    explicit Proc(std::function<void()> notify);
    ~Proc();  // stops the process and waits for the reader thread

    Proc(const Proc&) = delete;
    Proc& operator=(const Proc&) = delete;

    // Starts `command` in folder `cwd` (empty: inherit). Returns false and
    // sets `error` if a process is already running or the start fails.
    bool start(const std::string& command, const std::string& cwd, std::string& error);

    // Interrupt (SIGINT, like ctrl-C), then SIGTERM after 1 s, SIGKILL after 3 s.
    void stop();

    bool running() const { return running_; }

    // Moves the queued events into `out` (thread safe).
    void drain(std::vector<Event>& out);

private:
    void reader(int fd);
    void push(Event e);

    std::function<void()> notify_;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<Event> events_;
    std::atomic<bool> running_{false};
    std::atomic<int> pid_{0};
    std::atomic<long long> term_at_ms_{0};  // 0: not scheduled
    std::atomic<long long> kill_at_ms_{0};
};

}  // namespace spawn
