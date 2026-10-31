// proc - Windows implementation. See proc.h.
//
// The command runs through cmd.exe, with a console of its own that has no
// window, inside a job object. ctrl-C is sent by attaching for a moment to that
// console; the job reaches every process the command has started.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN

#include "proc.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <iterator>

namespace spawn {

namespace {

const size_t kMaxQueued = 10000;          // lines waiting for the host; more are dropped
const size_t kMaxLine = 64 * 1024;        // a longer line without newline is cut
const DWORD kPollMs = 10;
const long long kDrainAfterExitMs = 300;  // stop reading when only leftover processes hold the pipe
const DWORD kStatusControlCExit = 0xC000013A;

long long now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::wstring to_wide(const std::string& s, UINT codepage = CP_UTF8)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(codepage, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(codepage, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string to_utf8(const std::wstring& w)
{
    if (w.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Hosts expect UTF-8. Python is told to write it; what is not valid UTF-8 comes
// from a program using the console code page (dir, for example).
std::string output_to_utf8(const std::string& line)
{
    if (line.empty() ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), (int)line.size(), nullptr, 0) > 0)
        return line;
    return to_utf8(to_wide(line, CP_OEMCP));
}

std::string last_error_text()
{
    DWORD code = GetLastError();
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, (LPWSTR)&text, 0, nullptr);
    std::string s = text ? to_utf8(text) : "error " + std::to_string(code);
    if (text)
        LocalFree(text);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// cmd.exe reads a / in the first word as the start of a switch, so
// .venv/Scripts/python.exe would not run. The first word is taken as a path:
// a built-in with a switch needs its space ("dir /b", not "dir/b").
std::wstring fix_program_path(std::wstring command)
{
    size_t begin = command.find_first_not_of(L" \t");
    if (begin == std::wstring::npos)
        return command;
    size_t end = command[begin] == L'"' ? command.find(L'"', begin + 1) : command.find_first_of(L" \t", begin);
    if (end == std::wstring::npos)
        end = command.size();
    std::replace(command.begin() + begin, command.begin() + end, L'/', L'\\');
    return command;
}

// The environment of the host, plus what makes Python write UTF-8 line by line
// to a pipe. The block must be sorted.
std::wstring environment_block()
{
    std::vector<std::wstring> vars;
    LPWCH env = GetEnvironmentStringsW();
    for (LPWCH e = env; e && *e; e += wcslen(e) + 1)
        vars.push_back(e);
    if (env)
        FreeEnvironmentStringsW(env);

    auto add_if_missing = [&vars](const wchar_t* name, const wchar_t* value) {
        std::wstring prefix = std::wstring(name) + L"=";
        for (const auto& v : vars)
            if (_wcsnicmp(v.c_str(), prefix.c_str(), prefix.size()) == 0)
                return;
        vars.push_back(prefix + value);
    };
    add_if_missing(L"PYTHONUNBUFFERED", L"1");
    add_if_missing(L"PYTHONIOENCODING", L"utf-8");
    std::sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });

    std::wstring block;
    for (const auto& v : vars) {
        block += v;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

std::mutex console_mutex;
std::atomic<long long> ignore_ctrl_c_until{0};

// The ctrl-C sent to the command also reaches this process, later and on
// another thread; without this handler it would end the host. It stays
// installed, because the event may arrive after the console has been left.
BOOL WINAPI ctrl_handler(DWORD type)
{
    return type == CTRL_C_EVENT && now_ms() < ignore_ctrl_c_until ? TRUE : FALSE;
}

// False if ctrl-C could not be sent, for example when the host has a console of
// its own (a process has only one, so it could not attach to the command's).
bool send_ctrl_c(DWORD pid)
{
    std::lock_guard<std::mutex> lock(console_mutex);

    // The handler is never removed, so this module must never be unloaded.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)&ctrl_handler, &self);

    // Handlers run last-added first: this one goes before any added since.
    SetConsoleCtrlHandler(ctrl_handler, FALSE);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    ignore_ctrl_c_until = now_ms() + 2000;

    if (!AttachConsole(pid))
        return false;
    BOOL sent = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
    FreeConsole();
    return sent != FALSE;
}

}  // namespace

Proc::Proc(std::function<void()> notify) : notify_(std::move(notify)) {}

Proc::~Proc()
{
    if (running_) {
        long long t = now_ms();
        term_at_ms_ = t;
        kill_at_ms_ = send_ctrl_c(pid_) ? t + 500 : t;
    }
    if (thread_.joinable())
        thread_.join();
    close_handles();
}

void Proc::close_handles()
{
    if (process_)
        CloseHandle(process_);
    if (job_)
        CloseHandle(job_);
    process_ = nullptr;
    job_ = nullptr;
}

bool Proc::start(const std::string& command, const std::string& cwd, std::string& error)
{
    if (running_) {
        error = "already running: send stop first";
        return false;
    }
    if (thread_.joinable())
        thread_.join();  // the previous process has ended
    close_handles();

    // Everything the command starts stays in the job. The job ends with the
    // host, even if the host crashes.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        error = "cannot create job: " + last_error_text();
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);

    // stdin empty; stdout and stderr into the same pipe, so lines keep their order.
    SECURITY_ATTRIBUTES inherit = {sizeof inherit, nullptr, TRUE};
    HANDLE pipe_read = nullptr, pipe_write = nullptr;
    if (!CreatePipe(&pipe_read, &pipe_write, &inherit, 0)) {
        error = "cannot create pipe: " + last_error_text();
        CloseHandle(job);
        return false;
    }
    SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             OPEN_EXISTING, 0, nullptr);

    // No handles of the host except these two.
    HANDLE handles[] = {nul, pipe_write};
    SIZE_T list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
    std::vector<char> list_memory(list_size);
    auto list = (LPPROC_THREAD_ATTRIBUTE_LIST)list_memory.data();
    InitializeProcThreadAttributeList(list, 1, 0, &list_size);
    UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles,
                              nullptr, nullptr);

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = pipe_write;
    si.StartupInfo.hStdError = pipe_write;
    si.lpAttributeList = list;

    // From the system folder, never a cmd.exe found in the folder of the patch.
    wchar_t system_dir[MAX_PATH] = {};
    GetSystemDirectoryW(system_dir, MAX_PATH);
    std::wstring cmd_exe = std::wstring(system_dir) + L"\\cmd.exe";
    // /d: no AutoRun scripts. /s /c "...": cmd.exe removes the outer quotes only.
    std::wstring line = L"\"" + cmd_exe + L"\" /d /s /c \"" + fix_program_path(to_wide(command)) + L"\"";

    std::wstring dir = to_wide(cwd);
    std::replace(dir.begin(), dir.end(), L'/', L'\\');
    std::wstring env = environment_block();

    // Suspended until it is in the job, so that nothing it starts can escape.
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(cmd_exe.c_str(), &line[0], nullptr, nullptr, TRUE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                 EXTENDED_STARTUPINFO_PRESENT,
                             &env[0], dir.empty() ? nullptr : dir.c_str(), &si.StartupInfo, &pi);
    if (!ok)
        error = "cannot start cmd.exe: " + last_error_text();

    DeleteProcThreadAttributeList(list);
    CloseHandle(pipe_write);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    if (!ok) {
        CloseHandle(pipe_read);
        CloseHandle(job);
        return false;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    process_ = pi.hProcess;
    job_ = job;
    pid_ = pi.dwProcessId;
    term_at_ms_ = 0;
    kill_at_ms_ = 0;
    running_ = true;
    thread_ = std::thread(&Proc::reader, this, (void*)pipe_read);
    return true;
}

void Proc::stop()
{
    if (!running_ || term_at_ms_ != 0)
        return;
    long long t = now_ms();
    term_at_ms_ = t;
    kill_at_ms_ = send_ctrl_c(pid_) ? t + 3000 : t;
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

void Proc::reader(void* pipe_handle)
{
    HANDLE pipe = pipe_handle;
    std::string buf;
    bool exited = false, kill_sent = false;
    long long exited_at = 0;

    auto emit_lines = [&] {
        size_t start = 0, nl;
        while ((nl = buf.find('\n', start)) != std::string::npos) {
            size_t end = (nl > start && buf[nl - 1] == '\r') ? nl - 1 : nl;
            push({Event::Line, output_to_utf8(buf.substr(start, end - start))});
            start = nl + 1;
        }
        buf.erase(0, start);
        if (buf.size() > kMaxLine) {
            push({Event::Line, output_to_utf8(buf)});
            buf.clear();
        }
    };
    auto close_pipe = [&] {
        CloseHandle(pipe);
        pipe = nullptr;
    };

    while (true) {
        bool got_data = false;
        DWORD available = 0;
        if (pipe && !PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            close_pipe();  // every writer has gone
        if (pipe && available > 0) {
            char chunk[4096];
            DWORD got = 0;
            if (ReadFile(pipe, chunk, std::min<DWORD>(available, sizeof chunk), &got, nullptr) && got > 0) {
                buf.append(chunk, got);
                emit_lines();
                got_data = true;
            }
            else {
                close_pipe();
            }
        }

        long long t = now_ms();
        if (!exited && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
            exited = true;
            exited_at = t;
            if (term_at_ms_ != 0)
                TerminateJobObject(job_, 1);  // stop reaches what the command left behind
        }
        if (!exited && !kill_sent && kill_at_ms_ != 0 && t >= kill_at_ms_) {
            TerminateJobObject(job_, 1);
            kill_sent = true;
        }
        if (exited && (!pipe || t - exited_at > kDrainAfterExitMs))
            break;
        if (!got_data)
            Sleep(kPollMs);
    }

    if (!buf.empty())
        push({Event::Line, output_to_utf8(buf)});
    if (pipe)
        close_pipe();

    // As on macOS, what a command leaves running by itself ("start notepad")
    // goes on running: closing the job later must not end it.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof limits);

    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    running_ = false;
    if (kill_sent)
        push({Event::Signal, "", 9});
    else if (code == kStatusControlCExit)
        push({Event::Signal, "", 2});
    else
        push({Event::Exit, "", (int)code});
}

}  // namespace spawn
