#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/types.h>
#include <filesystem>

namespace dqix::linux_ui {

struct BuildRequest {
    std::filesystem::path source_root;
    std::filesystem::path workspace;
    std::filesystem::path rom;
    bool allow_merge_unmarked = false;
};

struct BuildEvent {
    enum class Kind { Log, Progress, Status, Succeeded, Failed };
    Kind kind = Kind::Log;
    std::string text;
    int percent = 0;
    bool indeterminate = false;
};

class BuildJob {
public:
    BuildJob() = default;
    ~BuildJob();
    BuildJob(const BuildJob&) = delete;
    BuildJob& operator=(const BuildJob&) = delete;

    bool start(BuildRequest request, std::string& error);
    void cancel_and_join();
    void wait();
    bool running() const noexcept { return running_.load(); }
    std::vector<BuildEvent> drain_events();

private:
    void run(BuildRequest request);
    void publish(BuildEvent event);
    void log_line(const std::string& line);

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<pid_t> child_pid_{-1};
    std::mutex events_mutex_;
    std::deque<BuildEvent> events_;
    std::filesystem::path log_path_;
    std::filesystem::path devlog_path_;
};

bool valid_rom_path(const std::filesystem::path& rom, std::string& error);
bool validate_workspace_path(const std::filesystem::path& source_root,
                             const std::filesystem::path& workspace,
                             std::string& error);
bool workspace_needs_merge_confirmation(const std::filesystem::path& workspace);
bool game_directory_ready(const std::filesystem::path& game_dir);

} // namespace dqix::linux_ui
