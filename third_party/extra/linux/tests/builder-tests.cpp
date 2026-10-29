#include "build_job.hpp"
#include "game_process.hpp"

#include <filesystem>
#include <cerrno>
#include <cstdlib>
#include <initializer_list>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <sys/wait.h>

namespace fs = std::filesystem;
using dqix::linux_ui::BuildEvent;
using dqix::linux_ui::BuildJob;
using dqix::linux_ui::BuildRequest;

namespace {
bool require(bool condition, const std::string& message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
void write_file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}
struct SavedEnvironment {
    bool present = false;
    std::string value;
};
SavedEnvironment save_environment(const char* name) {
    const char* value = std::getenv(name);
    return {value != nullptr, value ? value : ""};
}
void restore_environment(const char* name, const SavedEnvironment& saved) {
    if (saved.present) (void)::setenv(name, saved.value.c_str(), 1);
    else (void)::unsetenv(name);
}
std::string first_existing_library(std::initializer_list<const char*> candidates) {
    for (const char* candidate : candidates) {
        if (candidate && ::access(candidate, R_OK) == 0) return candidate;
    }
    return {};
}
}

int main() {
    const fs::path temp = fs::temp_directory_path() /
        ("dqix-linux-builder-test-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp);

    const fs::path rom = temp / "fixture.nds";
    write_file(rom, "synthetic ROM placeholder for builder integration test only\n");
    write_file(temp / "fixture.txt", "not a ROM\n");

    bool ok = true;
    std::string error;
    ok &= require(dqix::linux_ui::valid_rom_path(rom, error), "synthetic .nds fixture must validate");
    error.clear();
    ok &= require(!dqix::linux_ui::valid_rom_path(temp / "fixture.txt", error),
                  "non-.nds file must be rejected");

    const fs::path success_source = temp / "source-success";
    write_file(success_source / "third_party/extra/linux/src/tools/build-game.sh", R"script(#!/bin/bash
set -eu
echo "== Checking the ROM"
mkdir -p game/data/code
printf '#!/bin/sh\nexit 0\n' > game/dqix
chmod +x game/dqix
printf 'synthetic generated ROM placeholder\n' > game/data/dqix.nds
touch game/data/code/generated.c
echo "== Done."
)script");
    write_file(success_source / "game/should-not-be-copied.txt", "must be excluded\n");
    write_file(success_source / ".git/config", "must be excluded\n");
    const fs::path success_workspace = temp / "workspace-success";
    error.clear();
    ok &= require(dqix::linux_ui::validate_workspace_path(success_source, success_workspace, error),
                  "new nested workspace should validate: " + error);
    error.clear();
    ok &= require(!dqix::linux_ui::validate_workspace_path(success_source, success_source, error),
                  "workspace equal to source must be rejected");

    const fs::path merge = temp / "existing-unmarked";
    write_file(merge / "keep-me.txt", "user data\n");
    ok &= require(dqix::linux_ui::workspace_needs_merge_confirmation(merge),
                  "non-empty unmarked workspace must require confirmation");
    write_file(merge / ".dqix-enhanced-appimage-workspace", "test marker\n");
    ok &= require(!dqix::linux_ui::workspace_needs_merge_confirmation(merge),
                  "marked workspace should not ask for merge confirmation");
    const fs::path logs_only = temp / "logs-only-workspace";
    write_file(logs_only / "devlog.log", "[test] previous build log\\n");
    ok &= require(!dqix::linux_ui::workspace_needs_merge_confirmation(logs_only),
                  "launcher-generated logs alone must not trigger a merge warning");

    BuildRequest request;
    request.source_root = success_source;
    request.workspace = success_workspace;
    request.rom = rom;
    BuildJob success_job;
    error.clear();
    ok &= require(success_job.start(request, error), "simulated successful build should start: " + error);
    success_job.wait();
    bool saw_success=false, saw_progress=false;
    for (const auto& event : success_job.drain_events()) {
        saw_success |= event.kind == BuildEvent::Kind::Succeeded;
        saw_progress |= event.kind == BuildEvent::Kind::Progress && event.percent >= 100;
    }
    ok &= require(saw_success, "successful child should emit Succeeded");
    ok &= require(saw_progress, "successful child should emit final progress");
    ok &= require(dqix::linux_ui::game_directory_ready(success_workspace / "game"),
                  "simulated game output should pass readiness checks");
    ok &= require(fs::is_regular_file(success_workspace / "third_party/extra/linux/src/tools/build-game.sh"),
                  "source staging must include the patch-derived Linux build entry point");
    ok &= require(fs::is_regular_file(success_workspace / "launcher-build.log"),
                  "build output should be persisted to launcher-build.log");
    ok &= require(fs::is_regular_file(success_workspace / "devlog.log"),
                  "combined build/game development log should be created in the workspace");
    std::ifstream devlog_stream(success_workspace / "devlog.log", std::ios::binary);
    const std::string devlog_contents((std::istreambuf_iterator<char>(devlog_stream)),
                                      std::istreambuf_iterator<char>());
    ok &= require(devlog_contents.find("[build] [builder] DQIX Enhanced native SDL2 build started") != std::string::npos,
                  "devlog should contain timestamped builder session records");
    ok &= require(devlog_contents.find("[build] == Checking the ROM") != std::string::npos,
                  "devlog should mirror build-tool output");
    ok &= require(!fs::exists(success_workspace / "game/should-not-be-copied.txt"),
                  "source staging must preserve the game's generated output area");
    ok &= require(!fs::exists(success_workspace / ".git/config"),
                  "source staging must exclude .git metadata");

    const fs::path failure_source = temp / "source-failure";
    write_file(failure_source / "third_party/extra/linux/src/tools/build-game.sh",
               "#!/bin/bash\nset -eu\necho 'simulated build failure'\nexit 7\n");
    const fs::path failure_workspace = temp / "workspace-failure";
    BuildRequest fail_request;
    fail_request.source_root = failure_source;
    fail_request.workspace = failure_workspace;
    fail_request.rom = rom;
    BuildJob failure_job;
    error.clear();
    ok &= require(failure_job.start(fail_request, error), "simulated failing build should start: " + error);
    failure_job.wait();
    bool saw_failure=false;
    for (const auto& event : failure_job.drain_events())
        saw_failure |= event.kind == BuildEvent::Kind::Failed;
    ok &= require(saw_failure, "non-zero child exit must emit Failed");
    ok &= require(fs::is_regular_file(failure_workspace / "launcher-build.log"),
                  "failed build diagnostics must be persisted");

    // Exercise the production launch helper using synthetic stdout/stderr.
    const fs::path game_workspace = temp / "game-log-workspace";
    const fs::path game_dir = game_workspace / "game";
    write_file(game_dir / "play.sh", R"script(#!/bin/bash
set -eu
printf 'synthetic-game-stdout-marker\n'
printf 'synthetic-game-stderr-marker\n' >&2
printf 'synthetic-gl-ld-preload=%s\n' "$LD_PRELOAD"
printf 'synthetic-gl-library-path=%s\n' "$LD_LIBRARY_PATH"
printf 'synthetic-gl-video-driver=%s\n' "$SDL_VIDEODRIVER"
printf 'synthetic-gl-force-egl=%s\n' "$SDL_VIDEO_X11_FORCE_EGL"
exit 0
)script");
    const std::string test_host_preload = first_existing_library({
        "/usr/lib/libm.so.6", "/usr/lib64/libm.so.6", "/lib/libm.so.6", "/lib64/libm.so.6",
        "/usr/lib/x86_64-linux-gnu/libm.so.6", "/lib/x86_64-linux-gnu/libm.so.6"});
    const std::string test_user_preload = first_existing_library({
        "/usr/lib/libgcc_s.so.1", "/usr/lib64/libgcc_s.so.1", "/lib/libgcc_s.so.1",
        "/lib64/libgcc_s.so.1", "/usr/lib/x86_64-linux-gnu/libgcc_s.so.1",
        "/lib/x86_64-linux-gnu/libgcc_s.so.1"});
    ok &= require(!test_host_preload.empty() && !test_user_preload.empty(),
                  "system libm and libgcc_s must be available for safe preload-environment testing");
    const auto old_dqix_gl_preload = save_environment("DQIX_LINUX_GL_PRELOAD");
    const auto old_dqix_gl_override = save_environment("DQIX_LINUX_GL_OVERRIDE_DIR");
    const auto old_ld_preload = save_environment("LD_PRELOAD");
    const auto old_ld_library_path = save_environment("LD_LIBRARY_PATH");
    const auto old_video_driver = save_environment("SDL_VIDEODRIVER");
    const auto old_force_egl = save_environment("SDL_VIDEO_X11_FORCE_EGL");
    const std::string test_gl_override = (temp / "system-gl-override").string();
    fs::create_directories(test_gl_override);
    (void)::setenv("DQIX_LINUX_GL_PRELOAD", test_host_preload.c_str(), 1);
    (void)::setenv("DQIX_LINUX_GL_OVERRIDE_DIR", test_gl_override.c_str(), 1);
    (void)::setenv("LD_PRELOAD", test_user_preload.c_str(), 1);
    (void)::setenv("LD_LIBRARY_PATH", "/test/toolchain/lib:/usr/lib", 1);
    (void)::setenv("SDL_VIDEODRIVER", "wayland", 1);
    (void)::setenv("SDL_VIDEO_X11_FORCE_EGL", "1", 1);
    const auto launched_game = dqix::linux_ui::launch_game_process(
        game_workspace, game_dir, game_dir / "launcher.log", "test-version");
    restore_environment("DQIX_LINUX_GL_PRELOAD", old_dqix_gl_preload);
    restore_environment("DQIX_LINUX_GL_OVERRIDE_DIR", old_dqix_gl_override);
    restore_environment("LD_PRELOAD", old_ld_preload);
    restore_environment("LD_LIBRARY_PATH", old_ld_library_path);
    restore_environment("SDL_VIDEODRIVER", old_video_driver);
    restore_environment("SDL_VIDEO_X11_FORCE_EGL", old_force_egl);
    ok &= require(launched_game.pid > 0, "synthetic game process should start: " + launched_game.error);
    ok &= require(launched_game.devlog_enabled, "game output capture should open devlog.log");
    ok &= require(launched_game.used_play_script, "production launch helper should prefer game/play.sh");
    if (launched_game.pid > 0) {
        int game_status = 0;
        pid_t waited;
        do { waited = ::waitpid(launched_game.pid, &game_status, 0); } while (waited < 0 && errno == EINTR);
        ok &= require(waited == launched_game.pid, "synthetic game child should be reaped");
        ok &= require(WIFEXITED(game_status) && WEXITSTATUS(game_status) == 0,
                      "synthetic game child should exit successfully");
        (void)dqix::linux_ui::append_devlog_message(launched_game.devlog_path, "[game] Game exited with code 0.");
    }
    std::ifstream game_devlog_stream(game_workspace / "devlog.log", std::ios::binary);
    const std::string game_devlog((std::istreambuf_iterator<char>(game_devlog_stream)),
                                  std::istreambuf_iterator<char>());
    ok &= require(game_devlog.find("GAME SESSION START") != std::string::npos,
                  "devlog should have an explicit game-session header");
    ok &= require(game_devlog.find("[launcher] version=test-version") != std::string::npos,
                  "devlog should identify the launcher version");
    ok &= require(game_devlog.find("synthetic-game-stdout-marker") != std::string::npos,
                  "game stdout must be captured in devlog.log");
    ok &= require(game_devlog.find("synthetic-game-stderr-marker") != std::string::npos,
                  "game stderr/renderer diagnostics must be captured in devlog.log");
    ok &= require(game_devlog.find("synthetic-gl-ld-preload=" + test_host_preload + ":" + test_user_preload) != std::string::npos,
                  "game launch must prepend host libraries and preserve the existing LD_PRELOAD");
    ok &= require(game_devlog.find("synthetic-gl-library-path=" + test_gl_override + ":/test/toolchain/lib:/usr/lib") != std::string::npos,
                  "game launch must put the host override before the toolchain and preserve other paths");
    ok &= require(game_devlog.find("synthetic-gl-video-driver=x11") != std::string::npos,
                  "game launch must select X11 for host-GL compatibility");
    ok &= require(game_devlog.find("synthetic-gl-force-egl=0") != std::string::npos,
                  "game launch must select GLX rather than EGL under X11");
    ok &= require(game_devlog.find("OpenGL host-library compatibility enabled") != std::string::npos,
                  "devlog must mark when host OpenGL compatibility is applied");
    ok &= require(game_devlog.find("[game] Game exited with code 0.") != std::string::npos,
                  "game exit status should be appendable to devlog.log");

    // A symlink at devlog.log must not be followed; fall back to game/launcher.log.
    const fs::path fallback_workspace = temp / "game-log-fallback";
    const fs::path fallback_game = fallback_workspace / "game";
    const fs::path protected_log = temp / "protected-log.txt";
    write_file(protected_log, "protected-sentinel\n");
    fs::create_directories(fallback_game);
    fs::create_symlink(protected_log, fallback_workspace / "devlog.log", ec);
    if (ec) {
        ok &= require(false, "fallback test could not create a symlink: " + ec.message());
        ec.clear();
    } else {
        write_file(fallback_game / "play.sh", R"script(#!/bin/bash
printf 'fallback-game-output-marker\n'
)script");
        const auto fallback_launch = dqix::linux_ui::launch_game_process(
            fallback_workspace, fallback_game, fallback_game / "launcher.log", "test-version");
        ok &= require(fallback_launch.pid > 0, "fallback game process should start: " + fallback_launch.error);
        ok &= require(!fallback_launch.devlog_enabled && fallback_launch.fallback_log_enabled,
                      "unsafe devlog symlink should force launcher.log fallback");
        ok &= require(fallback_launch.output_log_path == fallback_game / "launcher.log",
                      "launch helper should report the active fallback log path");
        if (fallback_launch.pid > 0) {
            int status = 0;
            pid_t waited;
            do { waited = ::waitpid(fallback_launch.pid, &status, 0); } while (waited < 0 && errno == EINTR);
            ok &= require(waited == fallback_launch.pid, "fallback game child should be reaped");
        }
        (void)dqix::linux_ui::append_devlog_message(
            fallback_launch.output_log_path, "[game] fallback exited with code 0.");
        std::ifstream fallback_stream(fallback_game / "launcher.log", std::ios::binary);
        const std::string fallback_contents((std::istreambuf_iterator<char>(fallback_stream)),
                                            std::istreambuf_iterator<char>());
        ok &= require(fallback_contents.find("fallback-game-output-marker") != std::string::npos,
                      "game stdout should fall back to launcher.log when devlog is unsafe");
        ok &= require(fallback_contents.find("[game] fallback exited with code 0.") != std::string::npos,
                      "game exit status should be appended to the active fallback log");
        std::ifstream protected_stream(protected_log, std::ios::binary);
        std::string protected_contents;
        std::getline(protected_stream, protected_contents);
        ok &= require(protected_contents == "protected-sentinel",
                      "devlog symlink fallback must not touch its target");
    }

    // A pre-existing symlink in the user workspace must never redirect writes outside it.
    const fs::path link_source = temp / "source-link";
    write_file(link_source / "third_party/extra/linux/src/tools/build-game.sh", "#!/bin/bash\nexit 0\n");
    write_file(link_source / "docs/private.txt", "bundled source text\n");
    const fs::path link_workspace = temp / "workspace-link";
    const fs::path outside = temp / "outside";
    write_file(outside / "private.txt", "must remain untouched\n");
    fs::create_directories(link_workspace);
    write_file(link_workspace / ".dqix-enhanced-appimage-workspace", "test marker\n");
    fs::create_directory_symlink(outside, link_workspace / "docs", ec);
    if (ec) {
        ok &= require(false, "test setup could not create a symlink: " + ec.message());
        ec.clear();
    } else {
        BuildRequest link_request;
        link_request.source_root = link_source;
        link_request.workspace = link_workspace;
        link_request.rom = rom;
        BuildJob link_job;
        error.clear();
        ok &= require(link_job.start(link_request, error), "symlink safety job should start: " + error);
        link_job.wait();
        bool saw_symlink_failure=false;
        for (const auto& event : link_job.drain_events())
            saw_symlink_failure |= event.kind == BuildEvent::Kind::Failed &&
                                    event.text.find("workspace symlink") != std::string::npos;
        ok &= require(saw_symlink_failure, "source staging must reject destination symlinks");
        std::ifstream outside_file(outside / "private.txt", std::ios::binary);
        std::string outside_text;
        std::getline(outside_file, outside_text);
        ok &= require(outside_text == "must remain untouched",
                      "workspace staging must not overwrite data through symlinks");
    }

    fs::remove_all(temp, ec);
    if (!ok) return 1;
    std::cout << "Native SDL2 builder dry-run tests passed (synthetic ROM fixture; no game data).\n";
    return 0;
}
