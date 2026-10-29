#include "build_job.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace dqix::linux_ui {
namespace fs = std::filesystem;
namespace {
constexpr const char* kWorkspaceMarker = ".dqix-enhanced-appimage-workspace";

bool is_within(const fs::path& child, const fs::path& parent) {
    auto c = child.begin();
    auto p = parent.begin();
    for (; p != parent.end(); ++p, ++c) {
        if (c == child.end() || *c != *p) return false;
    }
    return true;
}

fs::path canonical_or_normal(const fs::path& value, std::error_code& ec) {
    ec.clear();
    fs::path result = fs::weakly_canonical(value, ec);
    if (ec) {
        ec.clear();
        result = fs::absolute(value, ec);
        if (ec) return value.lexically_normal();
    }
    return result.lexically_normal();
}

void reject_symlink_components(const fs::path& workspace, const fs::path& relative) {
    fs::path current = workspace;
    for (const auto& component : relative) {
        current /= component;
        std::error_code ec;
        const auto status = fs::symlink_status(current, ec);
        if (ec == std::errc::no_such_file_or_directory) continue;
        if (ec) throw std::runtime_error("Cannot inspect workspace path " + current.string() + ": " + ec.message());
        if (fs::is_symlink(status))
            throw std::runtime_error("Refusing to write through a workspace symlink: " + current.string());
    }
}

bool ignored_name(const fs::path& path) {
    const std::string name = path.filename().string();
    static const char* exact[] = {
        ".git", "__pycache__", "build", "game", "extract", "obj", "gen",
        "gen7", "release", "rom", "reports", "session", "update", "bookmarks",
        "launcher-build.log", "launcher.log", "devlog.log"
    };
    for (const char* entry : exact) if (name == entry) return true;
    static const char* suffixes[] = {
        ".nds", ".sav", ".o", ".exe", ".dll", ".so", ".ppm", ".bmp",
        ".wav", ".pyc", ".ico"
    };
    for (const char* suffix : suffixes) {
        const std::string ext(suffix);
        if (name.size() >= ext.size() &&
            name.compare(name.size() - ext.size(), ext.size(), ext) == 0) return true;
    }
    return false;
}

void check_cancelled(const std::atomic<bool>& cancel) {
    if (cancel.load()) throw std::runtime_error("Build cancelled because the launcher is closing.");
}

int progress_for_line(const std::string& line, int current, std::string& label,
                      bool& indeterminate) {
    struct Stage { const char* token; int percent; const char* label; bool busy; };
    static const Stage stages[] = {
        {"== Checking the ROM", 5, "Verifying ROM", false},
        {"== Extracting the game's code", 12, "Extracting game code from ROM", false},
        {"extracted ARM9", 22, "Extraction complete", false},
        {"== Recompiling it to C", 28, "Recompiling game code (this can take a while)", true},
        {"== Compiling", 55, "Compiling runtime and game (this can take a while)", true},
        {"== Putting the game folder together", 92, "Assembling game folder", false},
        {"== Done.", 100, "Build complete", false},
    };
    for (const auto& stage : stages) {
        if (line.find(stage.token) != std::string::npos) {
            label = stage.label;
            indeterminate = stage.busy;
            return std::max(current, stage.percent);
        }
    }
    return current;
}

void stage_sources(const fs::path& source, const fs::path& workspace,
                   const std::atomic<bool>& cancel, std::size_t& copied_files,
                   std::uintmax_t& copied_bytes) {
    std::error_code ec;
    fs::create_directories(workspace, ec);
    if (ec) throw std::runtime_error("Cannot create workspace: " + ec.message());

    fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) throw std::runtime_error("Cannot enumerate bundled sources: " + ec.message());
    for (; it != end; it.increment(ec)) {
        check_cancelled(cancel);
        if (ec) {
            ec.clear();
            continue;
        }
        const fs::directory_entry entry = *it;
        const fs::path relative = entry.path().lexically_relative(source);
        if (relative.empty() || relative.native().find("..") == 0)
            throw std::runtime_error("Refusing an invalid source-relative path.");

        std::error_code type_ec;
        const auto status = entry.symlink_status(type_ec);
        if (type_ec) continue;
        // Never copy symlinks from the bundle into a writable workspace.
        if (fs::is_symlink(status)) {
            if (fs::is_directory(entry.path(), type_ec)) it.disable_recursion_pending();
            continue;
        }
        if (ignored_name(entry.path())) {
            if (fs::is_directory(status)) it.disable_recursion_pending();
            continue;
        }

        reject_symlink_components(workspace, relative);
        const fs::path destination = workspace / relative;
        if (fs::is_directory(status)) {
            fs::create_directories(destination, ec);
            if (ec) throw std::runtime_error("Cannot create " + destination.string() + ": " + ec.message());
            continue;
        }
        if (!fs::is_regular_file(status)) continue;
        fs::create_directories(destination.parent_path(), ec);
        if (ec) throw std::runtime_error("Cannot create " + destination.parent_path().string() + ": " + ec.message());
        fs::copy_file(entry.path(), destination, fs::copy_options::overwrite_existing, ec);
        if (ec) throw std::runtime_error("Cannot copy " + entry.path().string() + ": " + ec.message());
        ++copied_files;
        std::error_code size_ec;
        const auto size = fs::file_size(destination, size_ec);
        if (!size_ec) copied_bytes += size;
    }
}

bool directory_empty(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return true;
    if (ec || !fs::is_directory(path, ec)) return false;
    fs::directory_iterator it(path, ec), end;
    if (ec) return false;
    // Logs generated by this launcher alone must not trigger a merge warning
    // after a failed first build.
    for (; it != end; it.increment(ec)) {
        if (ec) return false;
        const std::string name = it->path().filename().string();
        if (name == "devlog.log" || name == "launcher-build.log" || name == "launcher.log")
            continue;
        return false;
    }
    return !ec;
}

std::string local_timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (::localtime_r(&now, &local) == nullptr) return "time-unavailable";
    char buffer[40]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S%z", &local) == 0)
        return "time-unavailable";
    return buffer;
}

void append_devlog_line(const fs::path& path, const std::string& line) {
    if (path.empty()) return;
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) return;
    output << "[" << local_timestamp() << "] [build] " << line;
    if (line.empty() || line.back() != '\n') output << '\n';
}

} // namespace

BuildJob::~BuildJob() {
    cancel_and_join();
}

bool valid_rom_path(const fs::path& rom, std::string& error) {
    std::error_code ec;
    if (rom.empty()) {
        error = "Choose your own .nds ROM before building.";
        return false;
    }
    const fs::path resolved = canonical_or_normal(rom, ec);
    if (ec || !fs::is_regular_file(resolved, ec) || ec) {
        error = "ROM file does not exist or is not a regular file.";
        return false;
    }
    std::string extension = resolved.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (extension != ".nds") {
        error = "The selected ROM must have the .nds extension.";
        return false;
    }
    if (::access(resolved.c_str(), R_OK) != 0) {
        error = "The selected ROM is not readable.";
        return false;
    }
    return true;
}

bool validate_workspace_path(const fs::path& source_root, const fs::path& workspace,
                             std::string& error) {
    std::error_code ec;
    const fs::path source = canonical_or_normal(source_root, ec);
    if (ec || !fs::is_directory(source, ec) || ec) {
        error = "The bundled project source directory is missing.";
        return false;
    }
    if (workspace.empty()) {
        error = "Choose a workspace directory.";
        return false;
    }
    const fs::path target = canonical_or_normal(workspace, ec);
    if (ec || target.empty()) {
        error = "The workspace path cannot be resolved.";
        return false;
    }
    const fs::path root = fs::path("/").lexically_normal();
    if (target == root) {
        error = "The filesystem root cannot be used as a workspace.";
        return false;
    }
    const char* home_env = std::getenv("HOME");
    if (home_env && *home_env) {
        const fs::path home = canonical_or_normal(home_env, ec);
        if (!ec && target == home) {
            error = "Choose a dedicated workspace folder, not your home directory itself.";
            return false;
        }
    }
    if (target == source || is_within(target, source) || is_within(source, target)) {
        error = "The workspace must be separate from the bundled source tree.";
        return false;
    }
    if (fs::exists(target, ec) && (!fs::is_directory(target, ec) || ec)) {
        error = "The workspace path exists but is not a directory.";
        return false;
    }
    fs::path writable_probe = target;
    if (!fs::exists(writable_probe, ec)) {
        ec.clear();
        writable_probe = target.parent_path();
        while (!writable_probe.empty() && !fs::exists(writable_probe, ec)) {
            if (ec) { ec.clear(); }
            const fs::path parent = writable_probe.parent_path();
            if (parent == writable_probe) break;
            writable_probe = parent;
        }
    }
    if (writable_probe.empty()) writable_probe = ".";
    ec.clear();
    if (!fs::exists(writable_probe, ec) || ec ||
        !fs::is_directory(writable_probe, ec) || ec ||
        ::access(writable_probe.c_str(), W_OK | X_OK) != 0) {
        error = "The workspace or its nearest existing parent directory is not writable.";
        return false;
    }
    return true;
}

bool workspace_needs_merge_confirmation(const fs::path& workspace) {
    std::error_code ec;
    if (!fs::exists(workspace, ec) || ec || !fs::is_directory(workspace, ec) || ec) return false;
    if (fs::exists(workspace / kWorkspaceMarker, ec) && !ec) return false;
    return !directory_empty(workspace);
}

bool game_directory_ready(const fs::path& game_dir) {
    std::error_code ec;
    const fs::path executable = game_dir / "dqix";
    return fs::is_regular_file(executable, ec) && !ec &&
           ::access(executable.c_str(), X_OK) == 0 &&
           fs::is_regular_file(game_dir / "data" / "dqix.nds", ec) && !ec &&
           fs::is_directory(game_dir / "data" / "code", ec) && !ec;
}

bool BuildJob::start(BuildRequest request, std::string& error) {
    if (running_.load()) {
        error = "A build is already running.";
        return false;
    }
    if (worker_.joinable()) worker_.join();
    if (!valid_rom_path(request.rom, error)) return false;
    if (!validate_workspace_path(request.source_root, request.workspace, error)) return false;
    if (workspace_needs_merge_confirmation(request.workspace) && !request.allow_merge_unmarked) {
        error = "This workspace is not empty and has no builder marker; confirm merging source files before building.";
        return false;
    }
    const fs::path script = request.workspace / "third_party" / "extra" / "linux" / "src" / "tools" / "build-game.sh";
    (void)script;
    cancel_.store(false);
    running_.store(true);
    try {
        worker_ = std::thread(&BuildJob::run, this, std::move(request));
    } catch (const std::exception& e) {
        running_.store(false);
        error = std::string("Cannot start build worker: ") + e.what();
        return false;
    }
    return true;
}

void BuildJob::wait() {
    if (worker_.joinable()) worker_.join();
}

void BuildJob::cancel_and_join() {
    cancel_.store(true);
    const pid_t pid = child_pid_.load();
    if (pid > 0) {
        (void)::kill(-pid, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        (void)::kill(-pid, SIGKILL);
    }
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}

void BuildJob::publish(BuildEvent event) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    constexpr std::size_t kMaxEvents = 4096;
    if (events_.size() >= kMaxEvents) events_.pop_front();
    events_.push_back(std::move(event));
}

std::vector<BuildEvent> BuildJob::drain_events() {
    std::vector<BuildEvent> result;
    std::lock_guard<std::mutex> lock(events_mutex_);
    result.reserve(events_.size());
    while (!events_.empty()) {
        result.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    return result;
}

void BuildJob::log_line(const std::string& line) {
    publish({BuildEvent::Kind::Log, line, 0, false});
    const std::string with_newline = line.empty() || line.back() != '\n' ? line + "\n" : line;
    if (!log_path_.empty()) {
        std::ofstream output(log_path_, std::ios::binary | std::ios::app);
        if (output) output << with_newline;
    }
    append_devlog_line(devlog_path_, line);
}

void BuildJob::run(BuildRequest request) {
    int current_progress = 1;
    bool succeeded = false;
    std::string failure;
    try {
        std::error_code ec;
        const fs::path source = canonical_or_normal(request.source_root, ec);
        if (ec) throw std::runtime_error("Cannot resolve bundled source directory.");
        request.workspace = canonical_or_normal(request.workspace, ec);
        if (ec) throw std::runtime_error("Cannot resolve workspace path.");
        request.rom = canonical_or_normal(request.rom, ec);
        if (ec) throw std::runtime_error("Cannot resolve ROM path.");
        log_path_ = request.workspace / "launcher-build.log";
        devlog_path_ = request.workspace / "devlog.log";
        fs::create_directories(request.workspace, ec);
        if (ec) throw std::runtime_error("Cannot initialize workspace: " + ec.message());
        reject_symlink_components(request.workspace, fs::path("launcher-build.log"));
        reject_symlink_components(request.workspace, fs::path("devlog.log"));
        reject_symlink_components(request.workspace, fs::path(kWorkspaceMarker));
        log_line("[builder] DQIX Enhanced native SDL2 build started");
        log_line("[builder] Workspace: " + request.workspace.string());
        log_line("[builder] ROM selected: " + request.rom.filename().string());
        publish({BuildEvent::Kind::Status, "Preparing project sources in workspace…", 2, true});
        std::size_t copied_files = 0;
        std::uintmax_t copied_bytes = 0;
        const auto copy_start = std::chrono::steady_clock::now();
        stage_sources(source, request.workspace, cancel_, copied_files, copied_bytes);
        const auto copy_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - copy_start).count();
        log_line("[timing] workspace_source_copy wall_seconds=" + std::to_string(copy_seconds) +
                 " copied_files=" + std::to_string(copied_files) +
                 " copied_bytes=" + std::to_string(copied_bytes));
        std::ofstream marker(request.workspace / kWorkspaceMarker, std::ios::binary | std::ios::trunc);
        if (!marker) throw std::runtime_error("Cannot write the workspace marker.");
        marker << "Managed by DQIX Enhanced native SDL2 launcher.\n"
                  "The workspace may contain a user-supplied ROM, generated game code and saves.\n";
        marker.close();
        if (!marker) throw std::runtime_error("Failed while writing the workspace marker.");

        const fs::path script = request.workspace / "third_party" / "extra" / "linux" / "src" / "tools" / "build-game.sh";
        if (!fs::is_regular_file(script, ec) || ec)
            throw std::runtime_error("Build script not found: " + script.string());
        if (::access(script.c_str(), R_OK) != 0)
            throw std::runtime_error("Build script is not readable: " + script.string());
        check_cancelled(cancel_);
        publish({BuildEvent::Kind::Progress, "Sources ready — verifying ROM", current_progress, false});
        log_line("[builder] Starting the patch-derived Linux build entry point; code generation and compilation may take time.");

        int pipe_fds[2]{};
        if (::pipe(pipe_fds) != 0)
            throw std::runtime_error(std::string("Cannot create build output pipe: ") + std::strerror(errno));
        const pid_t pid = ::fork();
        if (pid < 0) {
            const std::string err = std::strerror(errno);
            ::close(pipe_fds[0]); ::close(pipe_fds[1]);
            throw std::runtime_error("Cannot start build process: " + err);
        }
        if (pid == 0) {
            (void)::setpgid(0, 0);
            (void)::close(pipe_fds[0]);
            (void)::dup2(pipe_fds[1], STDOUT_FILENO);
            (void)::dup2(pipe_fds[1], STDERR_FILENO);
            (void)::close(pipe_fds[1]);
            if (::chdir(request.workspace.c_str()) != 0) _exit(126);
            ::execl("/bin/bash", "bash", script.c_str(), request.rom.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        (void)::setpgid(pid, pid);
        child_pid_.store(pid);
        if (cancel_.load()) (void)::kill(-pid, SIGTERM);
        ::close(pipe_fds[1]);
        FILE* stream = ::fdopen(pipe_fds[0], "r");
        if (!stream) {
            ::close(pipe_fds[0]);
            (void)::kill(-pid, SIGTERM);
            int ignored = 0; (void)::waitpid(pid, &ignored, 0);
            child_pid_.store(-1);
            throw std::runtime_error("Cannot read build process output.");
        }
        char* buffer = nullptr;
        std::size_t capacity = 0;
        ssize_t count = 0;
        while ((count = ::getline(&buffer, &capacity, stream)) >= 0) {
            if (cancel_.load()) (void)::kill(-pid, SIGTERM);
            std::string line(buffer, static_cast<std::size_t>(count));
            log_line(line);
            std::string label;
            bool busy = false;
            const int next = progress_for_line(line, current_progress, label, busy);
            if (!label.empty()) {
                current_progress = next;
                publish({BuildEvent::Kind::Progress, label, current_progress, busy});
            }
        }
        std::free(buffer);
        (void)::fclose(stream);
        int status = 0;
        pid_t waited;
        do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
        child_pid_.store(-1);
        if (cancel_.load()) throw std::runtime_error("Build cancelled because the launcher is closing.");
        if (waited < 0) throw std::runtime_error(std::string("Cannot collect build exit status: ") + std::strerror(errno));
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            if (!game_directory_ready(request.workspace / "game")) {
                throw std::runtime_error("Build command succeeded, but the game folder is not ready (expected dqix, data/dqix.nds and data/code/).");
            }
            succeeded = true;
            publish({BuildEvent::Kind::Progress, "Build complete", 100, false});
            log_line("[builder] Build completed successfully.");
        } else {
            std::string status_text = WIFEXITED(status)
                ? "exit code " + std::to_string(WEXITSTATUS(status))
                : WIFSIGNALED(status) ? "signal " + std::to_string(WTERMSIG(status))
                                      : "unknown process status";
            throw std::runtime_error("Build command failed with " + status_text + ". See launcher-build.log.");
        }
    } catch (const std::exception& e) {
        failure = e.what();
    }
    if (succeeded) {
        publish({BuildEvent::Kind::Succeeded, "Build complete. Game is ready to launch.", 100, false});
    } else {
        if (!failure.empty()) {
            log_line("[builder] ERROR: " + failure);
            publish({BuildEvent::Kind::Failed, failure, current_progress, false});
        } else {
            publish({BuildEvent::Kind::Failed, "Build failed for an unknown reason.", current_progress, false});
        }
    }
    running_.store(false);
}

} // namespace dqix::linux_ui
