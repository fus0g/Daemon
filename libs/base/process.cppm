// daemon.process — spawning child processes with non-blocking pipes (build tools, programs, clangd).
module;
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>
export module daemon.base.process;
import std;

namespace fs = std::filesystem;

export namespace ide {

struct ProcessOptions {
    std::vector<std::string> argv;
    std::string cwd;
    bool pipeStdin = false;       // give the child a pipe on stdin (otherwise /dev/null)
    bool mergeStderr = true;      // stderr shares the stdout pipe (otherwise stderr is discarded)
    std::vector<std::pair<std::string, std::string>> env;   // variables to set (the rest is inherited)
    bool usePty = false;          // run on a pseudo-terminal: the program behaves as in a terminal (prompts, line-buffered
                                  // output, echoed input) and stdin / stdout / stderr are all the terminal
};

// Finds an executable by name on $PATH (or checks a path with a slash).
std::optional<std::string> findExecutable(const std::string& name);

class Process {
public:
    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process() {
        if (pid_ > 0 && !exited_) {
            terminate(true);
            int st = 0;
            ::waitpid(pid_, &st, 0);
        }
        closeFds();
    }

    std::expected<void, std::string> start(const ProcessOptions& o);

    // A pseudo-terminal without a child: the caller hands `slavePath` to another program (e.g. gdb's inferior-tty) and
    // reads / writes the master side here. The slave end is kept closed on our side so EOF arrives when the user is done.
    std::expected<std::string, std::string> openPtyOnly();
    void releasePtySlave() { if (ptySlave_ >= 0) { ::close(ptySlave_); ptySlave_ = -1; } }

    int outFd() const { return outFd_; }
    // Pseudo-terminal size (the program gets SIGWINCH).
    void resizePty(int cols, int rows) {
        if (outFd_ < 0) return;
        winsize ws{static_cast<unsigned short>(rows), static_cast<unsigned short>(cols), 0, 0};
        ::ioctl(outFd_, TIOCSWINSZ, &ws);
    }
    bool started() const { return pid_ > 0; }
    pid_t pid() const { return pid_; }

    // Reads whatever is available. Returns the byte count, 0 at end of stream, -1 if nothing is ready.
    long read(std::string& out);

    bool write(std::string_view data);

    void closeStdin() {
        if (inFd_ >= 0) { ::close(inFd_); inFd_ = -1; }
    }
    void closeOutput() {
        if (outFd_ >= 0) { ::close(outFd_); outFd_ = -1; }
    }

    // Signals the child's whole process group.
    void terminate(bool force = false) {
        if (pid_ > 0 && !exited_) ::kill(-pid_, force ? SIGKILL : SIGTERM);
    }

    // Exit status if the process has ended (128 + signal when killed), otherwise nullopt.
    std::optional<int> tryWait();

    bool running() { return started() && !tryWait().has_value(); }

    struct Result {
        int exitCode = -1;
        std::string output;
        bool timedOut = false;
        std::string error;  // spawn failure
    };

    // Runs a command to completion and captures its output (used for short, synchronous jobs / tests).
    static Result runCapture(const ProcessOptions& opts, int timeoutMs = 60000);

private:
    void closeFds() {
        releasePtySlave();
        closeStdin();
        closeOutput();
    }

    pid_t pid_ = -1;
    int outFd_ = -1, inFd_ = -1, ptySlave_ = -1;
    bool exited_ = true;
    int status_ = 0;
};

}  // namespace ide
