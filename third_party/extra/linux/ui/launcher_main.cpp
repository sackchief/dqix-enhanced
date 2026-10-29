// Minimal Linux-only launcher using the project's existing SDL2 stack.
// No ImGui, SDL3, Vulkan, SDL_ttf or other UI library is required.
#include "../settings/settings_model.hpp"
#include "../builder/build_job.hpp"
#include "../builder/game_process.hpp"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#include <string>
#include <string_view>
#include <system_error>
#include <cstdarg>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using dqix::linux_ui::IniDocument;
namespace {
constexpr int kWidth = 1280;
constexpr int kHeight = 800;
constexpr int kUiScale = 2;
enum class Page { Game, Graphics, Display, Audio, Controls, Mods, Wireless, System };
enum class BrowserKind { None, Rom, Workspace };
struct BrowserEntry { fs::path path; bool directory = false; };
constexpr std::size_t kBrowserVisibleRows = 9;
constexpr std::array<const char*, 8> kPageNames = {
    "GAME", "GRAPHICS", "SCREEN / DISPLAY", "AUDIO / MUSIC",
    "CONTROLS", "MODS", "WIRELESS / ONLINE", "SYSTEM"
};
struct LauncherState {
    fs::path game_dir, settings_path, log_path, devlog_path;
    fs::path source_root, workspace, rom_path;
    std::string version = "unknown";
    bool integrated_builder = false;
    bool confirm_merge = false;
    bool build_in_progress = false;
    bool build_indeterminate = false;
    bool build_failed = false;
    int build_percent = 0;
    std::string build_status = "Choose your ROM and workspace to build.";
    std::vector<std::string> build_lines;
    std::size_t build_log_offset = 0;
    std::unique_ptr<dqix::linux_ui::BuildJob> build_job = std::make_unique<dqix::linux_ui::BuildJob>();
    BrowserKind browser_kind = BrowserKind::None;
    fs::path browser_dir;
    std::vector<BrowserEntry> browser_entries;
    std::size_t browser_scroll = 0;
    std::size_t browser_selection = 0;
    bool path_editing = false;
    std::string path_edit_buffer;
    IniDocument settings;
    Page page = Page::Game;
    std::string message = "Choose a built game folder with --game-dir PATH.";
    bool message_is_error = false, settings_load_ok = true;
    pid_t game_pid = -1;
    std::string child_result;
    int focus_index = 0, previous_button_count = 1, button_count = 0;
};
bool g_mouse_click = false;
int g_mouse_x = 0, g_mouse_y = 0;
bool g_activate = false;

void log_message(const fs::path& path, const char* format, ...) {
    char buffer[2048]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    std::fprintf(stderr, "%s", buffer);
    std::fflush(stderr);
    if (!path.empty()) {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        if (file) file << buffer;
    }
}
fs::path absolute_path(const fs::path& path) {
    std::error_code ec;
    const fs::path result = fs::absolute(path, ec);
    return (ec ? path : result).lexically_normal();
}
std::string setting_value(const LauncherState& state, std::string_view key, std::string_view fallback) {
    return state.settings.get(key).value_or(std::string(fallback));
}
bool boolean_setting(const LauncherState& state, std::string_view key, bool fallback = false) {
    const std::string value = setting_value(state, key, fallback ? "1" : "0");
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    return end != value.c_str() && *end == '\0' && parsed != 0;
}
int integer_setting(const LauncherState& state, std::string_view key, int fallback, int minimum, int maximum) {
    const std::string value = setting_value(state, key, std::to_string(fallback));
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < minimum || parsed > maximum) return fallback;
    return static_cast<int>(parsed);
}
bool load_settings(LauncherState& state) {
    state.settings_path = state.game_dir / "settings.ini";
    state.settings = IniDocument::parse("");
    state.settings_load_ok = true;
    std::error_code ec;
    const bool exists = fs::exists(state.settings_path, ec);
    if (ec) {
        state.message = "Cannot inspect settings.ini: " + ec.message();
        state.message_is_error = true; state.settings_load_ok = false; return false;
    }
    if (!exists) {
        state.message = "No settings.ini found; defaults are shown until you save a change.";
        state.message_is_error = false; return true;
    }
    std::string error;
    if (!state.settings.load_file(state.settings_path, error)) {
        state.message = error; state.message_is_error = true; state.settings_load_ok = false;
        log_message(state.log_path, "[settings] %s\n", error.c_str()); return false;
    }
    state.message = "Loaded settings.ini. Saved values may still depend on runtime capabilities.";
    state.message_is_error = false;
    log_message(state.log_path, "[settings] loaded %s\n", state.settings_path.c_str());
    return true;
}
bool commit_setting(LauncherState& state, std::string_view key, std::string_view value) {
    if (!state.settings_load_ok) {
        state.message = "Settings cannot be edited because settings.ini could not be read.";
        state.message_is_error = true; return false;
    }
    const IniDocument previous = state.settings;
    std::string error;
    if (!state.settings.set_validated(key, value, error)) {
        state.message = error; state.message_is_error = true; return false;
    }
    std::error_code ec;
    fs::create_directories(state.game_dir, ec);
    if (ec || !state.settings.save_atomic(state.settings_path, error)) {
        state.settings = previous;
        state.message = ec ? "Cannot create game directory: " + ec.message() : error;
        state.message_is_error = true;
        log_message(state.log_path, "[settings] save failed: %s\n", state.message.c_str());
        return false;
    }
    state.message = "Saved " + std::string(key) + ". Runtime support is checked when the game starts.";
    state.message_is_error = false;
    log_message(state.log_path, "[settings] saved %s to %s\n", std::string(key).c_str(), state.settings_path.c_str());
    return true;
}
bool path_has_file(const fs::path& path) {
    std::error_code ec; return fs::is_regular_file(path, ec);
}
bool game_ready(const LauncherState& state) {
    const fs::path executable = state.game_dir / "dqix";
    std::error_code ec;
    return path_has_file(executable) && ::access(executable.c_str(), X_OK) == 0 &&
           path_has_file(state.game_dir / "data" / "dqix.nds") &&
           fs::is_directory(state.game_dir / "data" / "code", ec);
}
std::string short_path(const fs::path& path, std::size_t maximum = 72) {
    const std::string value = path.string();
    if (value.size() <= maximum) return value;
    return "..." + value.substr(value.size() - maximum + 3);
}
void launch_game(LauncherState& state) {
    if (state.game_pid > 0) {
        state.message = "The game is already running (PID " + std::to_string(state.game_pid) + ").";
        state.message_is_error = false; return;
    }
    if (!game_ready(state)) {
        state.message = "Game not ready: expected dqix, data/dqix.nds and data/code/.";
        state.message_is_error = true; return;
    }
    if (state.devlog_path.empty()) {
        const fs::path root = state.workspace.empty() ? state.game_dir : state.workspace;
        state.devlog_path = root / "devlog.log";
    }

    const auto launched = dqix::linux_ui::launch_game_process(
        state.workspace, state.game_dir, state.log_path, state.version);
    if (!launched.output_log_path.empty()) state.devlog_path = launched.output_log_path;
    if (launched.pid <= 0) {
        state.message = launched.error.empty() ? "Could not start game." : launched.error;
        state.message_is_error = true;
        log_message(state.log_path, "[launch] %s\n", state.message.c_str());
        return;
    }

    state.game_pid = launched.pid;
    state.child_result.clear();
    if (launched.devlog_enabled) {
        state.message = "Game started (PID " + std::to_string(launched.pid) +
                        "). Runtime output goes to workspace/devlog.log.";
        state.message_is_error = false;
    } else if (launched.fallback_log_enabled) {
        state.message = "Game started (PID " + std::to_string(launched.pid) +
                        "). devlog.log unavailable; output goes to launcher.log.";
        state.message_is_error = true;
    } else {
        state.message = "Game started (PID " + std::to_string(launched.pid) +
                        "), but neither devlog.log nor launcher.log could be opened.";
        state.message_is_error = true;
    }
    log_message(state.log_path, "[launch] started game pid=%ld from %s\n",
                static_cast<long>(launched.pid), state.game_dir.c_str());
}

void update_child_status(LauncherState& state) {
    if (state.game_pid <= 0) return;
    int status = 0;
    const pid_t result = ::waitpid(state.game_pid, &status, WNOHANG);
    if (result == 0) return;
    if (result < 0) {
        if (errno == EINTR) return;
        state.message = "Could not inspect game process: " + std::string(std::strerror(errno));
        state.message_is_error = true; state.game_pid = -1; return;
    }
    if (WIFEXITED(status)) state.child_result = "Game exited with code " + std::to_string(WEXITSTATUS(status)) + ".";
    else if (WIFSIGNALED(status)) state.child_result = "Game stopped by signal " + std::to_string(WTERMSIG(status)) + ".";
    else state.child_result = "Game process ended.";
    state.game_pid = -1; state.message = state.child_result; state.message_is_error = false;
    log_message(state.log_path, "[launch] %s\n", state.child_result.c_str());
    (void)dqix::linux_ui::append_devlog_message(state.devlog_path, "[game] " + state.child_result);
}


bool is_nds_path(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext == ".nds";
}
void refresh_browser_entries(LauncherState& state) {
    state.browser_entries.clear();
    std::error_code ec;
    if (!fs::is_directory(state.browser_dir, ec) || ec) {
        state.message = "Cannot open folder: " + state.browser_dir.string();
        state.message_is_error = true;
        return;
    }
    fs::directory_iterator it(state.browser_dir, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) {
        state.message = "Cannot list folder: " + ec.message();
        state.message_is_error = true;
        return;
    }
    constexpr std::size_t kMaxScannedEntries = 10000;
    constexpr std::size_t kMaxVisibleEntries = 5000;
    std::size_t scanned = 0;
    for (; it != end && scanned < kMaxScannedEntries &&
           state.browser_entries.size() < kMaxVisibleEntries; it.increment(ec), ++scanned) {
        if (ec) { ec.clear(); continue; }
        std::error_code type_ec;
        const auto status = it->symlink_status(type_ec);
        if (type_ec || fs::is_symlink(status)) continue;
        const bool directory = fs::is_directory(status);
        if (state.browser_kind == BrowserKind::Rom && !directory && !is_nds_path(it->path())) continue;
        if (!directory && !fs::is_regular_file(status)) continue;
        state.browser_entries.push_back({it->path(), directory});
    }
    std::sort(state.browser_entries.begin(), state.browser_entries.end(),
        [](const BrowserEntry& a, const BrowserEntry& b) {
            if (a.directory != b.directory) return a.directory > b.directory;
            return a.path.filename().native() < b.path.filename().native();
        });
    state.browser_scroll = 0;
    state.browser_selection = 0;
    state.message = (scanned >= kMaxScannedEntries || state.browser_entries.size() >= kMaxVisibleEntries)
        ? "Directory listing is capped at 10000 scanned / 5000 visible entries."
        : "Select a folder or .nds file.";
    state.message_is_error = false;
}
void close_browser(LauncherState& state) {
    if (state.path_editing) SDL_StopTextInput();
    state.path_editing = false;
    state.browser_kind = BrowserKind::None;
    state.browser_entries.clear();
}
void open_browser(LauncherState& state, BrowserKind kind) {
    state.browser_kind = kind;
    state.path_editing = false;
    fs::path start;
    std::error_code ec;
    if (kind == BrowserKind::Rom && !state.rom_path.empty())
        start = state.rom_path.parent_path();
    else if (kind == BrowserKind::Workspace && !state.workspace.empty() &&
             fs::is_directory(state.workspace, ec))
        start = state.workspace;
    else if (kind == BrowserKind::Workspace && !state.workspace.empty())
        start = state.workspace.parent_path();
    if (start.empty()) {
        const char* home = std::getenv("HOME");
        start = home && *home ? fs::path(home) : fs::current_path(ec);
    }
    if (start.empty()) start = "/";
    while (!start.empty() && !fs::is_directory(start, ec)) {
        ec.clear();
        const fs::path parent = start.parent_path();
        if (parent == start) break;
        start = parent;
    }
    if (start.empty()) start = "/";
    state.browser_dir = absolute_path(start);
    refresh_browser_entries(state);
}
void choose_workspace(LauncherState& state, const fs::path& path) {
    state.workspace = absolute_path(path);
    state.devlog_path = state.workspace / "devlog.log";
    state.game_dir = state.workspace / "game";
    state.settings_path = state.game_dir / "settings.ini";
    state.log_path = state.game_dir / "launcher.log";
    close_browser(state);
    (void)load_settings(state);
    state.message = "Workspace selected: " + state.workspace.string();
    state.message_is_error = false;
}
void activate_browser_entry(LauncherState& state, std::size_t index) {
    if (index >= state.browser_entries.size()) return;
    const BrowserEntry entry = state.browser_entries[index];
    if (entry.directory) {
        state.browser_dir = entry.path;
        refresh_browser_entries(state);
        return;
    }
    if (state.browser_kind == BrowserKind::Rom && is_nds_path(entry.path)) {
        state.rom_path = absolute_path(entry.path);
        close_browser(state);
        state.message = "ROM selected: " + state.rom_path.filename().string();
        state.message_is_error = false;
    }
}
bool commit_browser_path(LauncherState& state) {
    std::error_code ec;
    fs::path typed(state.path_edit_buffer);
    if (typed.is_relative()) typed = state.browser_dir / typed;
    fs::path chosen = absolute_path(typed);
    if (state.browser_kind == BrowserKind::Rom && is_nds_path(chosen) &&
        fs::is_regular_file(chosen, ec) && !ec && ::access(chosen.c_str(), R_OK) == 0) {
        state.rom_path = chosen;
        close_browser(state);
        state.message = "ROM selected: " + state.rom_path.filename().string();
        state.message_is_error = false;
        return true;
    }
    ec.clear();
    if (fs::is_directory(chosen, ec) && !ec) {
        state.browser_dir = chosen;
        state.path_editing = false;
        SDL_StopTextInput();
        refresh_browser_entries(state);
        return true;
    }
    if (state.browser_kind == BrowserKind::Workspace && !chosen.empty() &&
        chosen != fs::path("/") && chosen != absolute_path(std::getenv("HOME") ? std::getenv("HOME") : "/")) {
        const fs::path parent = chosen.parent_path();
        ec.clear();
        if (!parent.empty() && fs::is_directory(parent, ec) && !ec) {
            state.workspace = chosen;
            state.game_dir = state.workspace / "game";
            state.path_editing = false;
            SDL_StopTextInput();
            close_browser(state);
            (void)load_settings(state);
            state.message = "Workspace path selected. It will be created when you build.";
            state.message_is_error = false;
            return true;
        }
    }
    state.message = "That path is not a readable .nds ROM or an accessible folder.";
    state.message_is_error = true;
    return false;
}
void begin_build(LauncherState& state, bool allow_merge) {
    if (!state.integrated_builder || state.source_root.empty()) {
        state.message = "Build setup is unavailable: the bundled source root was not supplied.";
        state.message_is_error = true;
        return;
    }
    std::string error;
    if (!dqix::linux_ui::valid_rom_path(state.rom_path, error)) {
        state.message = error; state.message_is_error = true; return;
    }
    if (!dqix::linux_ui::validate_workspace_path(state.source_root, state.workspace, error)) {
        state.message = error; state.message_is_error = true; return;
    }
    if (dqix::linux_ui::workspace_needs_merge_confirmation(state.workspace) && !allow_merge) {
        state.confirm_merge = true;
        state.message = "Review the merge warning, then confirm to refresh matching project source files.";
        state.message_is_error = true;
        return;
    }
    dqix::linux_ui::BuildRequest request;
    request.source_root = state.source_root;
    request.workspace = state.workspace;
    request.rom = state.rom_path;
    request.allow_merge_unmarked = allow_merge;
    if (!state.build_job) state.build_job = std::make_unique<dqix::linux_ui::BuildJob>();
    if (!state.build_job->start(std::move(request), error)) {
        state.message = error;
        state.message_is_error = true;
        return;
    }
    state.confirm_merge = false;
    state.build_in_progress = true;
    state.build_failed = false;
    state.build_indeterminate = false;
    state.build_percent = 1;
    state.build_status = "Preparing workspace…";
    state.build_lines.clear();
    state.game_dir = state.workspace / "game";
    state.settings_path = state.game_dir / "settings.ini";
    state.log_path = state.game_dir / "launcher.log";
    state.devlog_path = state.workspace / "devlog.log";
    state.message = "Build started. The launcher will remain responsive; see Build output below.";
    state.message_is_error = false;
}
void poll_build_events(LauncherState& state) {
    if (!state.build_job) return;
    for (auto& event : state.build_job->drain_events()) {
        if (event.kind == dqix::linux_ui::BuildEvent::Kind::Log) {
            std::string line = event.text;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (!line.empty()) {
                state.build_lines.push_back(std::move(line));
                if (state.build_lines.size() > 500) state.build_lines.erase(state.build_lines.begin());
            }
        } else if (event.kind == dqix::linux_ui::BuildEvent::Kind::Status ||
                   event.kind == dqix::linux_ui::BuildEvent::Kind::Progress) {
            state.build_status = event.text;
            state.build_percent = std::max(state.build_percent, event.percent);
            state.build_indeterminate = event.indeterminate;
            state.message = event.text;
            state.message_is_error = false;
        } else if (event.kind == dqix::linux_ui::BuildEvent::Kind::Succeeded) {
            state.build_in_progress = false;
            state.build_failed = false;
            state.build_indeterminate = false;
            state.build_percent = 100;
            state.build_status = event.text;
            state.game_dir = state.workspace / "game";
            state.settings_path = state.game_dir / "settings.ini";
            state.log_path = state.game_dir / "launcher.log";
            state.devlog_path = state.workspace / "devlog.log";
            (void)load_settings(state);
            state.message = "Build succeeded. Play Now is ready.";
            state.message_is_error = false;
        } else if (event.kind == dqix::linux_ui::BuildEvent::Kind::Failed) {
            state.build_in_progress = false;
            state.build_failed = true;
            state.build_indeterminate = false;
            state.build_status = event.text;
            state.message = event.text;
            state.message_is_error = true;
        }
    }
}

// Compact 5x7 bitmap font: no external text or UI dependency.
const char* glyph_bits(char c) {
    static constexpr const char* blank = "00000""00000""00000""00000""00000""00000""00000";
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(c)))) {
    case 'A': return "01110""10001""10001""11111""10001""10001""10001";
    case 'B': return "11110""10001""10001""11110""10001""10001""11110";
    case 'C': return "01111""10000""10000""10000""10000""10000""01111";
    case 'D': return "11110""10001""10001""10001""10001""10001""11110";
    case 'E': return "11111""10000""10000""11110""10000""10000""11111";
    case 'F': return "11111""10000""10000""11110""10000""10000""10000";
    case 'G': return "01111""10000""10000""10111""10001""10001""01111";
    case 'H': return "10001""10001""10001""11111""10001""10001""10001";
    case 'I': return "11111""00100""00100""00100""00100""00100""11111";
    case 'J': return "00111""00010""00010""00010""10010""10010""01100";
    case 'K': return "10001""10010""10100""11000""10100""10010""10001";
    case 'L': return "10000""10000""10000""10000""10000""10000""11111";
    case 'M': return "10001""11011""10101""10101""10001""10001""10001";
    case 'N': return "10001""11001""10101""10011""10001""10001""10001";
    case 'O': return "01110""10001""10001""10001""10001""10001""01110";
    case 'P': return "11110""10001""10001""11110""10000""10000""10000";
    case 'Q': return "01110""10001""10001""10001""10101""10010""01101";
    case 'R': return "11110""10001""10001""11110""10100""10010""10001";
    case 'S': return "01111""10000""10000""01110""00001""00001""11110";
    case 'T': return "11111""00100""00100""00100""00100""00100""00100";
    case 'U': return "10001""10001""10001""10001""10001""10001""01110";
    case 'V': return "10001""10001""10001""10001""10001""01010""00100";
    case 'W': return "10001""10001""10001""10101""10101""10101""01010";
    case 'X': return "10001""10001""01010""00100""01010""10001""10001";
    case 'Y': return "10001""10001""01010""00100""00100""00100""00100";
    case 'Z': return "11111""00001""00010""00100""01000""10000""11111";
    case '0': return "01110""10001""10011""10101""11001""10001""01110";
    case '1': return "00100""01100""00100""00100""00100""00100""01110";
    case '2': return "01110""10001""00001""00010""00100""01000""11111";
    case '3': return "11110""00001""00001""01110""00001""00001""11110";
    case '4': return "00010""00110""01010""10010""11111""00010""00010";
    case '5': return "11111""10000""10000""11110""00001""00001""11110";
    case '6': return "01110""10000""10000""11110""10001""10001""01110";
    case '7': return "11111""00001""00010""00100""01000""01000""01000";
    case '8': return "01110""10001""10001""01110""10001""10001""01110";
    case '9': return "01110""10001""10001""01111""00001""00001""01110";
    case '-': return "00000""00000""00000""11111""00000""00000""00000";
    case '_': return "00000""00000""00000""00000""00000""00000""11111";
    case '.': return "00000""00000""00000""00000""00000""00110""00110";
    case ':': return "00000""00110""00110""00000""00110""00110""00000";
    case '/': return "00001""00010""00010""00100""01000""01000""10000";
    case '\\': return "10000""01000""01000""00100""00010""00010""00001";
    case '+': return "00000""00100""00100""11111""00100""00100""00000";
    case '%': return "11001""11010""00100""01000""10110""00110""00000";
    case '=': return "00000""11111""00000""11111""00000""00000""00000";
    case '>': return "10000""01000""00100""00010""00100""01000""10000";
    case '(': return "00010""00100""01000""01000""01000""00100""00010";
    case ')': return "01000""00100""00010""00010""00010""00100""01000";
    case '?': return "01110""10001""00001""00010""00100""00000""00100";
    case ',': return "00000""00000""00000""00000""00110""00100""01000";
    case '!': return "00100""00100""00100""00100""00100""00000""00100";
    case '\'': return "00100""00100""00000""00000""00000""00000""00000";
    default: return blank;
    }
}
void set_color(SDL_Renderer* renderer, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}
void fill_rect(SDL_Renderer* renderer, int x, int y, int w, int h, SDL_Color color) {
    SDL_Rect rect{x, y, w, h}; set_color(renderer, color); SDL_RenderFillRect(renderer, &rect);
}
void draw_text(SDL_Renderer* renderer, int x, int y, std::string_view text,
               SDL_Color color = SDL_Color{220,228,240,255}, int scale = kUiScale,
               std::size_t maximum_chars = 120) {
    set_color(renderer, color);
    const std::size_t limit = std::min(text.size(), maximum_chars);
    int cursor_x = x;
    for (std::size_t i = 0; i < limit; ++i) {
        const char* bits = glyph_bits(text[i]);
        for (int row = 0; row < 7; ++row)
            for (int column = 0; column < 5; ++column)
                if (bits[row * 5 + column] == '1') {
                    SDL_Rect pixel{cursor_x + column * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(renderer, &pixel);
                }
        cursor_x += 6 * scale;
        if (cursor_x >= kWidth - 12) break;
    }
}
bool draw_button(SDL_Renderer* renderer, LauncherState& state, int x, int y, int w, int h,
                 const std::string& label, bool disabled = false) {
    const int index = state.button_count++;
    const bool focused = index == state.focus_index;
    const SDL_Rect bounds{x,y,w,h};
    const bool inside = g_mouse_x >= x && g_mouse_x < x+w && g_mouse_y >= y && g_mouse_y < y+h;
    const bool clicked = g_mouse_click && inside && !disabled;
    if (clicked) state.focus_index = index;
    const bool activated = !disabled && (clicked || (g_activate && focused));
    fill_rect(renderer, x, y, w, h, disabled ? SDL_Color{36,43,55,255} :
              focused || inside ? SDL_Color{52,76,106,255} : SDL_Color{35,48,66,255});
    set_color(renderer, focused ? SDL_Color{102,190,255,255} : SDL_Color{63,80,103,255});
    SDL_RenderDrawRect(renderer, &bounds);
    draw_text(renderer, x+14, y+(h-14)/2, label,
        disabled ? SDL_Color{112,124,140,255} : SDL_Color{235,240,247,255}, 2,
        static_cast<std::size_t>(std::max(1,(w-28)/12)));
    return activated;
}
void page_title(SDL_Renderer* renderer, const char* title, const char* description) {
    draw_text(renderer, 260, 118, title, SDL_Color{246,193,95,255}, 3);
    draw_text(renderer, 260, 152, description, SDL_Color{166,180,199,255}, 2, 115);
}
void setting_toggle(SDL_Renderer* renderer, LauncherState& state, int y, const char* label,
                    const char* key, bool fallback = false) {
    const bool value = boolean_setting(state, key, fallback);
    if (draw_button(renderer, state, 270, y, 440, 46,
                    std::string(label) + ": " + (value ? "ON" : "OFF")))
        (void)commit_setting(state, key, value ? "0" : "1");
}
void render_ui(SDL_Renderer* renderer, LauncherState& state) {
    poll_build_events(state);
    update_child_status(state);
    state.button_count = 0;
    set_color(renderer, SDL_Color{15,23,42,255}); SDL_RenderClear(renderer);
    fill_rect(renderer, 0, 0, kWidth, 82, SDL_Color{24,35,54,255});
    draw_text(renderer, 28, 25, "DQIX ENHANCED", SDL_Color{245,191,84,255}, 3);
    draw_text(renderer, 330, 32, "LINUX LAUNCHER / SDL2", SDL_Color{168,184,205,255}, 2, 60);
    if (state.browser_kind != BrowserKind::None) {
        fill_rect(renderer,0,82,kWidth,kHeight-142,SDL_Color{15,23,42,255});
        page_title(renderer,state.browser_kind==BrowserKind::Rom?"SELECT YOUR ROM":"SELECT WORKSPACE FOLDER",
                   state.browser_kind==BrowserKind::Rom?"Choose a readable .nds file from your own dump.":
                                                        "Navigate to a folder, then choose Use This Folder.");
        draw_text(renderer,30,186,"CURRENT PATH",SDL_Color{126,150,178,255},1,120);
        draw_text(renderer,30,204,short_path(state.path_editing?fs::path(state.path_edit_buffer):state.browser_dir,128),
                  state.path_editing?SDL_Color{245,191,84,255}:SDL_Color{232,237,245,255},1,128);
        if (draw_button(renderer,state,30,226,112,38,"PARENT")) {
            if (state.path_editing) { state.path_editing=false; SDL_StopTextInput(); }
            const fs::path parent=state.browser_dir.parent_path();
            if (!parent.empty() && parent!=state.browser_dir) { state.browser_dir=parent; refresh_browser_entries(state); }
        }
        if (draw_button(renderer,state,154,226,112,38,"HOME")) {
            const char* home=std::getenv("HOME");
            if(home&&*home){state.browser_dir=absolute_path(home);refresh_browser_entries(state);}
        }
        if (state.browser_kind==BrowserKind::Workspace &&
            draw_button(renderer,state,278,226,200,38,"USE THIS FOLDER")) {
            const fs::path chosen=state.browser_dir;
            std::string error;
            if(dqix::linux_ui::validate_workspace_path(state.source_root,chosen,error))
                choose_workspace(state,chosen);
            else { state.message=error; state.message_is_error=true; }
        }
        if (draw_button(renderer,state,492,226,146,38,state.path_editing?"EDITING PATH":"EDIT PATH")) {
            if(state.path_editing) {
                (void)commit_browser_path(state);
            } else {
                state.path_editing=true;
                state.path_edit_buffer=state.browser_dir.string();
                SDL_StartTextInput();
            }
        }
        draw_text(renderer,660,238,state.path_editing?"Type path; Enter applies, Esc cancels edit.":"Click a folder to open it.",SDL_Color{144,160,181,255},1,92);
        const std::size_t total=state.browser_entries.size();
        if(state.browser_selection>=total && total>0) state.browser_selection=total-1;
        if(state.browser_selection<state.browser_scroll) state.browser_scroll=state.browser_selection;
        if(state.browser_selection>=state.browser_scroll+kBrowserVisibleRows)
            state.browser_scroll=state.browser_selection-kBrowserVisibleRows+1;
        int row_y=274;
        for(std::size_t row=0;row<kBrowserVisibleRows;++row) {
            const std::size_t i=state.browser_scroll+row;
            if(i>=total) break;
            const auto& entry=state.browser_entries[i];
            const std::string prefix=entry.directory?"[DIR] ":"      ";
            const bool clicked=draw_button(renderer,state,30,row_y,930,40,
                prefix+short_path(entry.path.filename(),100),false);
            if(clicked) {
                state.browser_selection=i;
                activate_browser_entry(state,i);
                break;
            }
            row_y+=46;
        }
        draw_text(renderer,30,700,"Entries: "+std::to_string(total)+" · Use ↑/↓ and Enter, Backspace for parent; Esc closes.",
                  SDL_Color{144,160,181,255},1,124);
    } else {
    fill_rect(renderer, 0, 82, 225, kHeight-142, SDL_Color{20,30,46,255});
    draw_text(renderer, 22, 102, "MENU", SDL_Color{126,150,178,255}, 2);
    for (int i = 0; i < static_cast<int>(kPageNames.size()); ++i)
        if (draw_button(renderer,state,12,132+i*51,200,43,kPageNames[static_cast<std::size_t>(i)]))
            state.page = static_cast<Page>(i);

    switch (state.page) {
    case Page::Game: {
        if (state.integrated_builder) {
            page_title(renderer, "BUILD / PLAY", "ROM, build progress, diagnostics and game launch in one SDL2 window.");
            draw_text(renderer,270,198,"WORKSPACE",SDL_Color{126,150,178,255},2);
            draw_text(renderer,270,221,short_path(state.workspace,104),SDL_Color{232,237,245,255},2,104);
            draw_text(renderer,270,256,"ROM",SDL_Color{126,150,178,255},2);
            draw_text(renderer,270,279,state.rom_path.empty()?"Not selected":short_path(state.rom_path,104),
                      state.rom_path.empty()?SDL_Color{255,190,125,255}:SDL_Color{232,237,245,255},2,104);
            if (draw_button(renderer,state,270,310,210,43,"SELECT WORKSPACE"))
                open_browser(state,BrowserKind::Workspace);
            if (draw_button(renderer,state,494,310,210,43,"SELECT ROM (.NDS)"))
                open_browser(state,BrowserKind::Rom);
            draw_text(renderer,270,371,"BUILD STATUS",SDL_Color{126,150,178,255},2);
            draw_text(renderer,270,394,state.build_status, state.build_failed?SDL_Color{255,133,125,255}:SDL_Color{190,205,222,255},2,100);
            fill_rect(renderer,270,419,438,12,SDL_Color{36,47,64,255});
            int progress_width=static_cast<int>(438.0*std::max(0,std::min(100,state.build_percent))/100.0);
            if (state.build_indeterminate) {
                const int phase=static_cast<int>((SDL_GetTicks()/5)%520);
                const int x=270+std::min(390,phase);
                fill_rect(renderer,x,419,48,12,SDL_Color{90,169,235,255});
            } else if(progress_width>0) fill_rect(renderer,270,419,progress_width,12,SDL_Color{90,169,235,255});
            if (state.confirm_merge) {
                draw_text(renderer,270,445,"WARNING: This folder is not an initialized workspace.",SDL_Color{255,190,125,255},2,100);
                draw_text(renderer,270,468,"Matching source files may be refreshed; game/build folders are preserved.",SDL_Color{255,190,125,255},1,120);
                if (draw_button(renderer,state,270,482,210,42,"CONFIRM + BUILD",state.build_in_progress))
                    begin_build(state,true);
                if (draw_button(renderer,state,494,482,160,42,"CANCEL",state.build_in_progress)) {
                    state.confirm_merge=false;
                    state.message="Workspace merge cancelled.";
                    state.message_is_error=false;
                }
            } else {
                if (draw_button(renderer,state,270,482,210,45,state.build_in_progress?"BUILD RUNNING":"BUILD / REBUILD",
                                state.build_in_progress))
                    begin_build(state,false);
                if (draw_button(renderer,state,494,482,210,45,"PLAY NOW",
                                state.build_in_progress || !game_ready(state)))
                    launch_game(state);
            }
            draw_text(renderer,270,538,"BUILD OUTPUT (launcher-build.log; build/game details in devlog.log)",SDL_Color{126,150,178,255},1,105);
            if (draw_button(renderer,state,950,528,105,30,"OLDER"))
                state.build_log_offset=std::min(state.build_lines.size(),state.build_log_offset+5);
            if (draw_button(renderer,state,1065,528,105,30,"NEWER"))
                state.build_log_offset=state.build_log_offset>5?state.build_log_offset-5:0;
            const std::size_t end=state.build_lines.size()>state.build_log_offset
                ?state.build_lines.size()-state.build_log_offset:0;
            const std::size_t first=end>8?end-8:0;
            int log_y=565;
            for(std::size_t i=first;i<end;++i) {
                draw_text(renderer,270,log_y,state.build_lines[i],SDL_Color{166,190,211,255},1,110);
                log_y+=20;
            }
            if (state.version!="unknown")
                draw_text(renderer,270,714,"APP VERSION: "+state.version,SDL_Color{126,150,178,255},1,100);
        } else {
            page_title(renderer,"GAME","Select a built game folder using --game-dir PATH.");
            draw_text(renderer,270,207,"GAME FOLDER",SDL_Color{126,150,178,255});
            draw_text(renderer,270,230,short_path(state.game_dir),SDL_Color{232,237,245,255},2,76);
            draw_text(renderer,270,275,std::string("EXECUTABLE: ")+
                (path_has_file(state.game_dir/"dqix") && ::access((state.game_dir/"dqix").c_str(),X_OK)==0?"READY":"MISSING"),
                SDL_Color{190,205,222,255});
            draw_text(renderer,270,298,std::string("ROM: ")+(path_has_file(state.game_dir/"data"/"dqix.nds")?"FOUND":"MISSING"),
                SDL_Color{190,205,222,255});
            std::error_code ec;
            draw_text(renderer,270,321,std::string("GENERATED CODE: ")+
                (fs::is_directory(state.game_dir/"data"/"code",ec)?"FOUND":"MISSING"),SDL_Color{190,205,222,255});
            if (draw_button(renderer,state,270,370,250,48,"RELOAD SETTINGS")) (void)load_settings(state);
            if (draw_button(renderer,state,270,430,250,54,"PLAY NOW",!game_ready(state))) launch_game(state);
            draw_text(renderer,270,510,"Uses the same data/dqix.nds and data/code/ layout as the build script.",
                      SDL_Color{144,160,181,255},2,95);
            draw_text(renderer,270,546,"Game output and SDL/OpenGL render diagnostics go to devlog.log.",
                      SDL_Color{144,160,181,255},2,95);
        }
        break;
    }
    case Page::Graphics:
        page_title(renderer,"GRAPHICS","Saved values may depend on the active gameplay renderer.");
        if (draw_button(renderer,state,270,215,440,46,
            "MODE: "+std::string(setting_value(state,"mode","classic")=="enhanced"?"ENHANCED":"CLASSIC")))
            (void)commit_setting(state,"mode",setting_value(state,"mode","classic")=="enhanced"?"classic":"enhanced");
        setting_toggle(renderer,state,274,"SMOOTH TEXTURES","smooth_textures");
        setting_toggle(renderer,state,333,"HD TEXT","hd_text");
        setting_toggle(renderer,state,392,"SOFT SHADOWS","soft_shadows",true);
        setting_toggle(renderer,state,451,"SMOOTH SHADING","smooth_shading");
        draw_text(renderer,270,526,"Some changes may require a game restart.",SDL_Color{144,160,181,255},2,96);
        break;
    case Page::Display:
        page_title(renderer,"SCREEN / DISPLAY","Display options are saved for the existing SDL2 runtime.");
        setting_toggle(renderer,state,215,"FULLSCREEN","fullscreen");
        setting_toggle(renderer,state,274,"INTEGER SCALING","integer_scaling");
        setting_toggle(renderer,state,333,"DS LITE FILTER","dslite_filter",true);
        setting_toggle(renderer,state,392,"LCD COLOURS","lcd_colors",true);
        setting_toggle(renderer,state,451,"ACTUAL SIZE","actual_size");
        if (draw_button(renderer,state,270,510,440,46,"WINDOW SCALE: "+
            std::to_string(integer_setting(state,"window_scale",2,1,8))+"X")) {
            const int value=integer_setting(state,"window_scale",2,1,8);
            (void)commit_setting(state,"window_scale",std::to_string(value>=8?1:value+1));
        }
        break;
    case Page::Audio:
        page_title(renderer,"AUDIO / MUSIC","Audio preferences are stored for the next game start.");
        setting_toggle(renderer,state,215,"MUTE","mute");
        if (draw_button(renderer,state,270,274,440,46,"GAME VOLUME: "+
            std::to_string(integer_setting(state,"volume",100,0,100))+"%")) {
            const int value=integer_setting(state,"volume",100,0,100);
            (void)commit_setting(state,"volume",std::to_string(value>=100?0:std::min(100,value+10)));
        }
        setting_toggle(renderer,state,333,"MUSIC PACKS","music_packs",true);
        setting_toggle(renderer,state,392,"SHOW SONG NAMES","music_song_names");
        break;
    case Page::Controls:
        page_title(renderer,"CONTROLS","Keyboard and gamepad remapping is not implemented here yet.");
        draw_text(renderer,270,220,"The game runtime owns input dispatch and controls.ini.",SDL_Color{190,205,222,255});
        draw_text(renderer,270,250,"This page does not claim to apply control changes.",SDL_Color{190,205,222,255});
        break;
    case Page::Mods: {
        page_title(renderer,"MODS","Matching assets are required for these settings to take effect.");
        setting_toggle(renderer,state,215,"TEXTURE PACKS","texture_packs",true);
        setting_toggle(renderer,state,274,"UPSCALE TEXTURES","upscale_textures");
        setting_toggle(renderer,state,333,"MUSIC PACKS","music_packs",true);
        std::error_code e1,e2;
        draw_text(renderer,270,410,std::string("TEXTURES FOLDER: ")+(fs::is_directory(state.game_dir/"textures",e1)?"FOUND":"NOT FOUND"),SDL_Color{190,205,222,255});
        draw_text(renderer,270,440,std::string("MUSIC FOLDER: ")+(fs::is_directory(state.game_dir/"music",e2)?"FOUND":"NOT FOUND"),SDL_Color{190,205,222,255});
        break;
    }
    case Page::Wireless:
        page_title(renderer,"WIRELESS / ONLINE","Network settings are saved; server connectivity is not tested here.");
        setting_toggle(renderer,state,215,"WIRELESS FEATURES","wireless",true);
        draw_text(renderer,270,290,"WFC SERVER: "+setting_value(state,"wfc_server","builtin"),SDL_Color{190,205,222,255},2,76);
        draw_text(renderer,270,325,"PLAYER NAME: "+setting_value(state,"player_name",""),SDL_Color{190,205,222,255},2,76);
        draw_text(renderer,270,360,"Editing these text fields is not yet implemented.",SDL_Color{144,160,181,255},2,90);
        break;
    case Page::System: {
        page_title(renderer,"SYSTEM / DIAGNOSTICS","These details describe the launcher, not gameplay capability.");
        SDL_version version{}; SDL_GetVersion(&version);
        draw_text(renderer,270,215,"SDL2 VERSION: "+std::to_string(version.major)+"."+std::to_string(version.minor)+"."+std::to_string(version.patch),SDL_Color{190,205,222,255});
        draw_text(renderer,270,255,"GAME: "+short_path(state.game_dir,70),SDL_Color{190,205,222,255},2,85);
        draw_text(renderer,270,290,"SETTINGS: "+short_path(state.settings_path,70),SDL_Color{190,205,222,255},2,85);
        draw_text(renderer,270,325,"LOG: "+short_path(state.log_path,70),SDL_Color{190,205,222,255},2,85);
        draw_text(renderer,270,350,"DEVLOG: "+short_path(state.devlog_path,70),SDL_Color{190,205,222,255},2,85);
        draw_text(renderer,270,372,"UI renderer: SDL_Renderer; gameplay uses the existing runtime paths.",SDL_Color{144,160,181,255},2,100);
        if (draw_button(renderer,state,270,430,260,46,"RELOAD SETTINGS")) (void)load_settings(state);
        if (draw_button(renderer,state,550,430,260,46,"SHOW LOG PATH")) state.message="Log file: "+state.log_path.string();
        break;
    }
    }
    }
    fill_rect(renderer,0,742,kWidth,58,SDL_Color{24,35,54,255});
    draw_text(renderer,20,765,state.message,
        state.message_is_error?SDL_Color{255,133,125,255}:SDL_Color{166,211,172,255},2,102);
    state.previous_button_count=std::max(1,state.button_count);
    if (state.focus_index>=state.button_count) state.focus_index=std::max(0,state.button_count-1);
    SDL_RenderPresent(renderer);
    g_activate=false; g_mouse_click=false;
}

bool parse_arguments(int argc,char** argv,LauncherState& state,bool& show_help,bool& self_test,std::string& error) {
    fs::path requested_game, requested_source, requested_workspace, requested_rom;
    std::string requested_version = "unknown";
    bool game_explicit=false, workspace_explicit=false, rom_explicit=false;
    for(int i=1;i<argc;++i) {
        const std::string_view arg(argv[i]);
        if(arg=="--help"||arg=="-h"){show_help=true;return true;}
        if(arg=="--self-test"){self_test=true;continue;}
        auto require_value=[&](const char* option)->const char* {
            if(i+1>=argc) { error=std::string(option)+" requires a value"; return nullptr; }
            return argv[++i];
        };
        if(arg=="--game-dir") {
            const char* value=require_value("--game-dir"); if(!value)return false;
            requested_game=value;game_explicit=true;continue;
        }
        if(arg=="--source-root") {
            const char* value=require_value("--source-root"); if(!value)return false;
            requested_source=value;continue;
        }
        if(arg=="--default-workspace"||arg=="--workspace") {
            const char* value=require_value("--default-workspace"); if(!value)return false;
            requested_workspace=value;workspace_explicit=true;continue;
        }
        if(arg=="--rom") {
            const char* value=require_value("--rom"); if(!value)return false;
            requested_rom=value;rom_explicit=true;continue;
        }
        if(arg=="--version") {
            const char* value=require_value("--version"); if(!value)return false;
            requested_version=value;continue;
        }
        error="unknown argument: "+std::string(arg); return false;
    }
    state.version=requested_version;
    state.integrated_builder=!requested_source.empty();
    if(state.integrated_builder) {
        state.source_root=absolute_path(requested_source);
        if(!workspace_explicit) {
            if(const char* env=std::getenv("DQIX_WORKSPACE");env&&*env) requested_workspace=env;
            else if(const char* data=std::getenv("XDG_DATA_HOME");data&&*data)
                requested_workspace=fs::path(data)/"dqix-enhanced-linux"/"workspace";
            else if(const char* home=std::getenv("HOME");home&&*home)
                requested_workspace=fs::path(home)/".local/share/dqix-enhanced-linux/workspace";
            else requested_workspace=fs::current_path()/"dqix-enhanced-workspace";
        }
        state.workspace=absolute_path(requested_workspace);
        state.game_dir=state.workspace/"game";
        if(game_explicit) {
            state.game_dir=absolute_path(requested_game);
            state.workspace=state.game_dir.filename()=="game"?state.game_dir.parent_path():state.game_dir.parent_path();
        }
        if(rom_explicit) state.rom_path=absolute_path(requested_rom);
        else {
            const fs::path built_rom=state.game_dir/"data"/"dqix.nds";
            std::error_code ec;
            if(fs::is_regular_file(built_rom,ec)&&!ec) state.rom_path=absolute_path(built_rom);
        }
    } else {
        fs::path requested; bool requested_explicitly=game_explicit;
        if(game_explicit) requested=requested_game;
        if(!requested_explicitly) {
            if(const char* env=std::getenv("DQIX_GAME_DIR");env&&*env){requested=env;requested_explicitly=true;}
        }
        if(!requested_explicitly) {
            std::error_code ec; const fs::path current=fs::current_path(ec); const fs::path candidate=current/"game";
            requested=(!ec&&fs::is_directory(candidate,ec))?candidate:current;
        }
        state.game_dir=absolute_path(requested);
        state.workspace=state.game_dir.filename()=="game"
            ?state.game_dir.parent_path():state.game_dir;
        if(rom_explicit) state.rom_path=absolute_path(requested_rom);
    }
    state.settings_path=state.game_dir/"settings.ini";
    state.log_path=state.game_dir/"launcher.log";
    state.devlog_path=(state.workspace.empty()?state.game_dir:state.workspace)/"devlog.log";
    return true;
}

int self_test() {
    (void)::setenv("SDL_VIDEODRIVER","dummy",1); (void)::setenv("SDL_AUDIODRIVER","dummy",1);
    SDL_SetHint(SDL_HINT_RENDER_DRIVER,"software");
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)!=0){std::fprintf(stderr,"SDL2 self-test init failed: %s\n",SDL_GetError());return 1;}
    SDL_Window* window=SDL_CreateWindow("DQIX SDL2 self-test",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,
        kWidth/2,kHeight/2,SDL_WINDOW_HIDDEN);
    if(!window){std::fprintf(stderr,"SDL2 self-test window failed: %s\n",SDL_GetError());SDL_Quit();return 1;}
    SDL_Renderer* renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
    if(!renderer){std::fprintf(stderr,"SDL2 self-test renderer failed: %s\n",SDL_GetError());SDL_DestroyWindow(window);SDL_Quit();return 1;}
    (void)SDL_RenderSetLogicalSize(renderer,kWidth,kHeight);
    LauncherState state; state.game_dir=fs::current_path(); state.settings_path=state.game_dir/"settings.ini"; state.log_path.clear();
    render_ui(renderer,state);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    std::puts("SDL2 software-renderer smoke test passed."); return 0;
}
} // namespace

int main(int argc,char** argv) {
    LauncherState state; bool show_help=false,self_test_requested=false; std::string argument_error;
    if(!parse_arguments(argc,argv,state,show_help,self_test_requested,argument_error)) {
        std::fprintf(stderr,"DQIX Linux launcher: %s\nRun --help for usage.\n",argument_error.c_str()); return 2;
    }
    if(show_help) {
        std::printf("DQIX Enhanced Linux Launcher (SDL2)\nUsage: %s [--source-root PATH --default-workspace PATH] [--game-dir PATH] [--rom PATH] [--version TEXT] [--self-test] [--help]\n"
                    "With --source-root, one SDL2 window owns ROM selection, build progress, settings and Play.\n"
                    "Without --source-root, --game-dir selects a built game folder containing dqix and data/.\n"
                    "--self-test runs a hidden SDL2 software-renderer smoke test.\n",argv[0]);
        return 0;
    }
    if(self_test_requested) return self_test();
    std::error_code ec;
    if(!state.integrated_builder) {
        fs::create_directories(state.game_dir,ec);
        if(ec) std::fprintf(stderr,"Warning: cannot create game directory: %s\n",ec.message().c_str());
    }
    log_message(state.log_path,"[launcher] SDL2 launcher starting\n[launcher] Game directory: %s\n",state.game_dir.c_str());
    (void)load_settings(state);
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)!=0) {
        log_message(state.log_path,"[sdl2] SDL_Init failed: %s\n",SDL_GetError()); return 1;
    }
    SDL_Window* window=SDL_CreateWindow("DQIX Enhanced - Linux Launcher",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
        kWidth,kHeight,SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    if(!window) {
        log_message(state.log_path,"[sdl2] SDL_CreateWindow failed: %s\n",SDL_GetError()); SDL_Quit(); return 1;
    }
    SDL_Renderer* renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if(!renderer) renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
    if(!renderer) {
        log_message(state.log_path,"[sdl2] SDL_CreateRenderer failed: %s\n",SDL_GetError());
        SDL_DestroyWindow(window); SDL_Quit(); return 1;
    }
    (void)SDL_RenderSetLogicalSize(renderer,kWidth,kHeight); (void)SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
    SDL_RendererInfo renderer_info{};
    if(SDL_GetRendererInfo(renderer,&renderer_info)==0)
        log_message(state.log_path,"[sdl2] renderer: %s\n",renderer_info.name);
    SDL_GameController* controller=nullptr;
    for(int i=0;i<SDL_NumJoysticks() && !controller;++i)
        if(SDL_IsGameController(i)) controller=SDL_GameControllerOpen(i);

    bool running=true;
    while(running) {
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
            if(event.type==SDL_QUIT) running=false;
            else if(event.type==SDL_WINDOWEVENT && event.window.event==SDL_WINDOWEVENT_CLOSE) running=false;
            else if(event.type==SDL_MOUSEBUTTONUP && event.button.button==SDL_BUTTON_LEFT) {
                g_mouse_click=true; g_mouse_x=event.button.x; g_mouse_y=event.button.y;
            } else if(event.type==SDL_MOUSEWHEEL && state.integrated_builder && state.page==Page::Game &&
                      state.browser_kind==BrowserKind::None) {
                if(event.wheel.y>0) state.build_log_offset=std::min(state.build_lines.size(),state.build_log_offset+3);
                else if(event.wheel.y<0) state.build_log_offset=state.build_log_offset>3?state.build_log_offset-3:0;
            } else if(event.type==SDL_TEXTINPUT && state.path_editing &&
                      state.path_edit_buffer.size()+std::strlen(event.text.text)<4096) {
                state.path_edit_buffer += event.text.text;
            } else if(event.type==SDL_KEYDOWN && !event.key.repeat) {
                const SDL_Keycode key=event.key.keysym.sym;
                if(state.browser_kind!=BrowserKind::None) {
                    if(state.path_editing) {
                        if(key==SDLK_ESCAPE) { state.path_editing=false; SDL_StopTextInput(); }
                        else if(key==SDLK_RETURN || key==SDLK_KP_ENTER) (void)commit_browser_path(state);
                        else if(key==SDLK_a && (event.key.keysym.mod & KMOD_CTRL)) state.path_edit_buffer.clear();
                        else if(key==SDLK_v && (event.key.keysym.mod & KMOD_CTRL)) {
                            char* clipboard=SDL_GetClipboardText();
                            if(clipboard) {
                                const std::size_t length=std::strlen(clipboard);
                                if(state.path_edit_buffer.size()+length<4096) state.path_edit_buffer+=clipboard;
                                SDL_free(clipboard);
                            }
                        }
                        else if(key==SDLK_BACKSPACE && !state.path_edit_buffer.empty()) {
                            std::size_t erase_at=state.path_edit_buffer.size()-1;
                            while(erase_at>0 &&
                                  (static_cast<unsigned char>(state.path_edit_buffer[erase_at])&0xC0)==0x80) --erase_at;
                            state.path_edit_buffer.erase(erase_at);
                        }
                    } else {
                        const std::size_t total=state.browser_entries.size();
                        if(key==SDLK_ESCAPE) close_browser(state);
                        else if(key==SDLK_BACKSPACE) {
                            const fs::path parent=state.browser_dir.parent_path();
                            if(!parent.empty()&&parent!=state.browser_dir){state.browser_dir=parent;refresh_browser_entries(state);}
                        } else if(key==SDLK_HOME) {
                            const char* home=std::getenv("HOME");
                            if(home&&*home){state.browser_dir=absolute_path(home);refresh_browser_entries(state);}
                        } else if(key==SDLK_UP && total>0) {
                            if(state.browser_selection>0)--state.browser_selection;
                            if(state.browser_selection<state.browser_scroll)state.browser_scroll=state.browser_selection;
                        } else if(key==SDLK_DOWN && total>0) {
                            if(state.browser_selection+1<total)++state.browser_selection;
                            if(state.browser_selection>=state.browser_scroll+kBrowserVisibleRows)
                                state.browser_scroll=state.browser_selection-kBrowserVisibleRows+1;
                        } else if((key==SDLK_RETURN||key==SDLK_KP_ENTER) && total>0)
                            activate_browser_entry(state,state.browser_selection);
                        else if(key==SDLK_PAGEUP) {
                            state.browser_scroll=state.browser_scroll>kBrowserVisibleRows?state.browser_scroll-kBrowserVisibleRows:0;
                            state.browser_selection=state.browser_scroll;
                        } else if(key==SDLK_PAGEDOWN && total>0) {
                            state.browser_scroll=std::min(total-1,state.browser_scroll+kBrowserVisibleRows);
                            state.browser_selection=state.browser_scroll;
                        }
                    }
                } else {
                    switch(key) {
                    case SDLK_ESCAPE: running=false; break;
                    case SDLK_PAGEUP:
                        if(state.integrated_builder&&state.page==Page::Game)
                            state.build_log_offset=std::min(state.build_lines.size(),state.build_log_offset+5);
                        break;
                    case SDLK_PAGEDOWN:
                        if(state.integrated_builder&&state.page==Page::Game)
                            state.build_log_offset=state.build_log_offset>5?state.build_log_offset-5:0;
                        break;
                    case SDLK_LEFT: state.page=static_cast<Page>((static_cast<int>(state.page)+7)%8);state.focus_index=0;break;
                    case SDLK_RIGHT: state.page=static_cast<Page>((static_cast<int>(state.page)+1)%8);state.focus_index=0;break;
                    case SDLK_UP: state.focus_index=(state.focus_index+state.previous_button_count-1)%state.previous_button_count;break;
                    case SDLK_DOWN: case SDLK_TAB: state.focus_index=(state.focus_index+1)%state.previous_button_count;break;
                    case SDLK_RETURN: case SDLK_SPACE: g_activate=true;break;
                    default: break;
                    }
                }
            } else if(event.type==SDL_CONTROLLERBUTTONDOWN) {
                if(state.browser_kind!=BrowserKind::None) {
                    const std::size_t total=state.browser_entries.size();
                    if(event.cbutton.button==SDL_CONTROLLER_BUTTON_B) close_browser(state);
                    else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP && total>0) {
                        if(state.browser_selection>0)--state.browser_selection;
                        if(state.browser_selection<state.browser_scroll)state.browser_scroll=state.browser_selection;
                    } else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN && total>0) {
                        if(state.browser_selection+1<total)++state.browser_selection;
                        if(state.browser_selection>=state.browser_scroll+kBrowserVisibleRows)
                            state.browser_scroll=state.browser_selection-kBrowserVisibleRows+1;
                    } else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_A && total>0)
                        activate_browser_entry(state,state.browser_selection);
                } else {
                    if(event.cbutton.button==SDL_CONTROLLER_BUTTON_B) running=false;
                    else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT) {state.page=static_cast<Page>((static_cast<int>(state.page)+7)%8);state.focus_index=0;}
                    else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {state.page=static_cast<Page>((static_cast<int>(state.page)+1)%8);state.focus_index=0;}
                    else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) state.focus_index=(state.focus_index+state.previous_button_count-1)%state.previous_button_count;
                    else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) state.focus_index=(state.focus_index+1)%state.previous_button_count;
                    else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_A) g_activate=true;
                }
            }
        }
        render_ui(renderer,state); SDL_Delay(16);
    }
    if(controller) SDL_GameControllerClose(controller);
    SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit(); return 0;
}
