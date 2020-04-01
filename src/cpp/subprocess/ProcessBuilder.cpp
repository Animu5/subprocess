#include "ProcessBuilder.hpp"

#ifdef _WIN32

#else
#include <spawn.h>
#ifdef __APPLE__
#include <sys/wait.h>
#else
#include <wait.h>
#endif
#include <errno.h>
#include <signal.h>
#endif

#include <string.h>
#include <thread>
#include <mutex>
#include <chrono>

#include "shell_utils.hpp"
#include "environ.hpp"

extern "C" char **environ;

using std::nullptr_t;
namespace {
    struct cstring_vector {
        typedef char* value_type;
        ~cstring_vector() {
            clear();
        }
        void clear() {
            for (char* ptr : m_list) {
                if (ptr)
                    free(ptr);
            }
            m_list.clear();
        }

        void reserve(std::size_t size) {
            m_list.reserve(size);
        }
        void push_back(const std::string& str) {
            push_back(str.c_str());
        }
        void push_back(nullptr_t) {
            m_list.push_back(nullptr);
        }
        void push_back(const char* str) {
            char* copy = str? strdup(str) : nullptr;
            m_list.push_back(copy);
        }
        void push_back_owned(char* str) {
            m_list.push_back(str);
        }
        value_type& operator[](std::size_t index) {
            return m_list[index];
        }
        char** data() {
            return &m_list[0];
        }
        std::vector<char*> m_list;
    };
    double monotonic_seconds() {
        static bool needs_init = true;
        static std::chrono::steady_clock::time_point begin;
        if (needs_init) {
            begin = std::chrono::steady_clock::now();
            needs_init = true;
        }
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        std::chrono::duration<double> duration = now - begin;
        return duration.count();
    }

    class StopWatch {
    public:
        StopWatch() { start(); }

        void start() { mStart = monotonic_seconds(); }
        double seconds() const { return monotonic_seconds() - mStart; }
    private:
        double mStart;
    };

    double sleep_seconds(double seconds) {
        StopWatch watch;
        std::chrono::duration<double> duration(seconds);
        std::this_thread::sleep_for(duration);
        return watch.seconds();
    }

}
namespace subprocess {


    Popen::Popen(CommandLine command, const PopenOptions& options) {
        ProcessBuilder builder;

        builder.cin_option  = options.cin;
        builder.cout_option = options.cout;
        builder.cerr_option = options.cerr;

        builder.env = options.env;
        builder.cwd = options.cwd;

        *this = builder.run_command(command);
    }
    Popen::Popen(Popen&& other) {
        *this = std::move(other);
    }

    Popen& Popen::operator=(Popen&& other) {
        close();
        cin = other.cin;
        cout = other.cout;
        cerr = other.cerr;

        pid = other.pid;
        returncode = other.returncode;
        args = std::move(other.args);

#ifdef _WIN32
        process_info = other.process_info;
        other.process_info = {0};
#endif

        other.cin = kBadPipeValue;
        other.cout = kBadPipeValue;
        other.cerr = kBadPipeValue;
        other.pid = 0;
        other.returncode = -1000;
        return *this;
    }

    Popen::~Popen() {
        close();
    }
    void Popen::close() {
        if (cin != kBadPipeValue)
            pipe_close(cin);
        if (cout != kBadPipeValue)
            pipe_close(cout);
        if (cerr != kBadPipeValue)
            pipe_close(cerr);
        cin = cout = cerr = kBadPipeValue;

        // do this to not have zombie processes.
        if (pid)
            wait();
        pid = 0;
        returncode = kBadReturnCode;
        args.clear();
#ifdef _WIN32
        CloseHandle(process_info.hProcess);
        CloseHandle(process_info.hThread);
#endif
    }
#ifdef _WIN32
    bool Popen::poll() {
        if (returncode != kBadReturnCode)
            return returncode;
        DWORD result = WaitForSingleObject(process_info.hProcess, 0);
        if (result == WAIT_TIMEOUT) {
            return false;
        }
        DWORD exit_code;
        GetExitCodeProcess(process_info.hProcess, &exit_code);
        returncode = exit_code;
        return returncode;
    }

    int Popen::wait(double timeout) {
        if (returncode != kBadReturnCode)
            return returncode;
        DWORD ms = timeout < 0? INFINITE : timeout*1000.0;
        DWORD result = WaitForSingleObject(process_info.hProcess, ms);
        if (result == WAIT_TIMEOUT) {
            throw TimeoutExpired("no time");
        }
        DWORD exit_code;
        GetExitCodeProcess(process_info.hProcess, &exit_code);
        returncode = exit_code;
        return returncode;
    }

    bool Popen::send_signal(int signum) {
        if (returncode != kBadReturnCode)
            return false;
        if (signum == PSIGKILL) {
            return TerminateProcess(process_info.hProcess, 1);
        } else if (signum == PSIGINT) {
            // can I use pid for processgroupid?
            return GenerateConsoleCtrlEvent(CTRL_C_EVENT, pid);
        } else if (signum == PSIGTERM) {
            return GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
        }
        return GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
    }
#else
    bool Popen::poll() {
        if (returncode != kBadReturnCode)
            return true;
        int exit_code;
        auto child = waitpid(pid, &exit_code, WNOHANG);
        if (child == 0)
            return false;
        if (child > 0)
            returncode = exit_code;
        return child > 0;
    }
    int Popen::wait(double timeout) {
        if (returncode != kBadReturnCode)
            return returncode;
        if (timeout < 0) {
            int exit_code;
            while (true) {
                pid_t child = waitpid(pid, &exit_code,0);
                if (child == -1 && errno == EINTR) {
                    continue;
                }
                if (child == -1) {
                    // TODO: throw oserror(errno)
                }
                break;
            }
            returncode = exit_code;
            return returncode;
        }
        StopWatch watch;

        while (watch.seconds() < timeout) {
            if (poll())
                return returncode;
            sleep_seconds(0.00001);
        }
        throw TimeoutExpired("no time");
    }

    bool Popen::send_signal(int signum) {
        if (returncode != kBadReturnCode)
            return false;
        return ::kill(pid, signum) == 0;
    }
#endif
    bool Popen::terminate() {
        return send_signal(PSIGTERM);
    }
    bool Popen::kill() {
        return send_signal(PSIGKILL);
    }










    std::string ProcessBuilder::windows_command() {
        return this->command[0];
    }

    std::string ProcessBuilder::windows_args() {
        return this->windows_args(this->command);
    }
    std::string ProcessBuilder::windows_args(const CommandLine& command) {
        std::string args;
        for(unsigned int i = 0; i < command.size(); ++i) {
            if (i > 0)
                args += ' ';
            args += escape_shell_arg(command[i]);
        }
        return args;
    }

#ifdef _WIN32

#else
    Popen ProcessBuilder::run_command(const CommandLine& command) {
        if (command.empty()) {
            throw std::invalid_argument("command should not be empty");
        }
        std::string program = find_program(command[0]);
        if(program.empty()) {
            throw CommandNotFoundError("command not found " + command[0]);
        }

        Popen process;
        PipePair cin_pair;
        PipePair cout_pair;
        PipePair cerr_pair;

        posix_spawn_file_actions_t action;

        posix_spawn_file_actions_init(&action);
        if (cin_option == PipeOption::close)
            posix_spawn_file_actions_addclose(&action, kStdInValue);
        else if (cin_option == PipeOption::specific) {
            posix_spawn_file_actions_adddup2(&action, this->cin_pipe, kStdInValue);
            posix_spawn_file_actions_addclose(&action, this->cin_pipe);
        } else if (cin_option == PipeOption::pipe) {
            cin_pair = pipe_create();
            posix_spawn_file_actions_addclose(&action, cin_pair.output);
            posix_spawn_file_actions_adddup2(&action, cin_pair.input, kStdInValue);
            posix_spawn_file_actions_addclose(&action, cin_pair.input);
            process.cin = cin_pair.output;
        }


        if (cout_option == PipeOption::close)
            posix_spawn_file_actions_addclose(&action, kStdOutValue);
        else if (cout_option == PipeOption::pipe) {
            cout_pair = pipe_create();
            posix_spawn_file_actions_addclose(&action, cout_pair.input);
            posix_spawn_file_actions_adddup2(&action, cout_pair.output, kStdOutValue);
            posix_spawn_file_actions_addclose(&action, cout_pair.output);
            process.cout = cout_pair.input;
        } else if (cout_option == PipeOption::cerr) {
            // we have to wait until stderr is setup first
        } else if (cout_option == PipeOption::specific) {
            posix_spawn_file_actions_adddup2(&action, this->cout_pipe, kStdOutValue);
            posix_spawn_file_actions_addclose(&action, this->cout_pipe);
        }

        if (cerr_option == PipeOption::close)
            posix_spawn_file_actions_addclose(&action, kStdErrValue);
        else if (cerr_option == PipeOption::pipe) {
            cerr_pair = pipe_create();
            posix_spawn_file_actions_addclose(&action, cerr_pair.input);
            posix_spawn_file_actions_adddup2(&action, cerr_pair.output, kStdErrValue);
            posix_spawn_file_actions_addclose(&action, cerr_pair.output);
            process.cerr = cerr_pair.input;
        } else if (cerr_option == PipeOption::cout) {
            posix_spawn_file_actions_adddup2(&action, kStdOutValue, kStdErrValue);
        } else if (cerr_option == PipeOption::specific) {
            posix_spawn_file_actions_adddup2(&action, this->cerr_pipe, kStdErrValue);
            posix_spawn_file_actions_addclose(&action, this->cerr_pipe);
        }

        if (cout_option == PipeOption::cerr) {
            posix_spawn_file_actions_adddup2(&action, kStdErrValue, kStdOutValue);
        }
        pid_t pid;
        cstring_vector args;
        args.reserve(command.size()+1);
        args.push_back(program);
        for(size_t i = 1; i < command.size(); ++i) {
            args.push_back(command[i]);
        }
        args.push_back(nullptr);
        char** env = environ;
        cstring_vector env_store;
        if (!this->env.empty()) {
            for (auto& pair : this->env) {
                std::string line = pair.first + "=" + pair.second;
                env_store.push_back(line);
            }
            env_store.push_back(nullptr);
            env = &env_store[0];
        }
        {
            /*  I should have gone with vfork() :(
                TODO: reimplement with vfork for thread safety.

                this locking solution should work in practice just fine.
            */
            static std::mutex mutex;
            std::unique_lock<std::mutex> lock(mutex);
            CwdGuard cwdGuard;
            if (!this->cwd.empty())
                subprocess::setcwd(this->cwd);
            if(posix_spawn(&pid, args[0], &action, NULL, &args[0], env) != 0)
                throw SpawnError("posix_spawn failed with error: " + std::string(strerror(errno)));
        }
        args.clear();
        env_store.clear();
        if (cin_pair)
            cin_pair.close_input();
        if (cout_pair)
            cout_pair.close_output();
        if (cerr_pair)
            cerr_pair.close_output();
        cin_pair.disown();
        cout_pair.disown();
        cerr_pair.disown();
        process.pid = pid;
        process.args = CommandLine(command.begin()+1, command.end());
        return process;
    }
#endif


    CompletedProcess run(CommandLine command, RunOptions options) {
        Popen popen(command, options);
        CompletedProcess completed;
        std::thread cout_thread;
        std::thread cerr_thread;
        if (popen.cout != kBadPipeValue) {
            cout_thread = std::thread([&]() {
                try {
                    completed.cout = pipe_read_all(popen.cout);
                } catch (...) {
                }
                pipe_close(popen.cout);
                popen.cout = kBadPipeValue;
            });
        }
        if (popen.cerr != kBadPipeValue) {
            cerr_thread = std::thread([&]() {
                try {
                    completed.cerr = pipe_read_all(popen.cerr);
                } catch (...) {
                }
                pipe_close(popen.cerr);
                popen.cerr = kBadPipeValue;
            });
        }

        if (cout_thread.joinable()) {
            cout_thread.join();
        }
        if (cerr_thread.joinable()) {
            cerr_thread.join();
        }

        popen.wait();
        completed.returncode = popen.returncode;
        completed.args = CommandLine(command.begin()+1, command.end());
        if (options.check) {
            CalledProcessError error("failed to execute " + command[0]);
            error.cmd           = command;
            error.returncode    = completed.returncode;
            error.cout          = std::move(completed.cout);
            error.cerr          = std::move(completed.cerr);
            throw error;
        }
        return completed;
    }

    CompletedProcess capture(CommandLine command, PopenOptions options) {
        return subprocess::run(command, options);
    }

}