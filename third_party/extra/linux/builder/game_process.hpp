#pragma once

#include <filesystem>
#include <string>
#include <sys/types.h>

namespace dqix::linux_ui {

struct GameProcessLaunch {
    pid_t pid = -1;
    bool devlog_enabled = false;
    bool fallback_log_enabled = false;
    bool used_play_script = false;
    std::filesystem::path devlog_path;
    std::filesystem::path output_log_path;
    std::string error;
};

// Launch game/play.sh when present, otherwise use the established dqix argv.
// Game stdout/stderr are captured in workspace/devlog.log, with launcher.log as
// a safe fallback. All path checks and log opening happen before fork().
GameProcessLaunch launch_game_process(const std::filesystem::path& workspace,
                                      const std::filesystem::path& game_dir,
                                      const std::filesystem::path& launcher_log,
                                      const std::string& version);

// Append one timestamped message to the workspace development log without
// following symlinks. Returns false if the log cannot be opened/written.
bool append_devlog_message(const std::filesystem::path& path, const std::string& message);

} // namespace dqix::linux_ui
