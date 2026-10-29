cmake_minimum_required(VERSION 3.20)
project(dqix_linux_ui LANGUAGES C CXX)

include(CTest)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
find_program(BASH_EXECUTABLE bash REQUIRED)

add_library(dqix-linux-overlay STATIC overlay/linux_overlay.c overlay/linux_overlay_gl.c)
target_link_libraries(dqix-linux-overlay PUBLIC PkgConfig::SDL2)
target_include_directories(dqix-linux-overlay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/overlay")
target_compile_features(dqix-linux-overlay PUBLIC c_std_11)

add_library(dqix-linux-settings STATIC settings/settings_model.cpp)
target_include_directories(dqix-linux-settings PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/settings")
target_compile_features(dqix-linux-settings PUBLIC cxx_std_17)

add_library(dqix-linux-builder STATIC builder/build_job.cpp builder/game_process.cpp)
target_include_directories(dqix-linux-builder PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/builder")
target_compile_features(dqix-linux-builder PUBLIC cxx_std_17)
target_link_libraries(dqix-linux-builder PUBLIC Threads::Threads)

add_executable(dqix-linux-ui-launcher ui/launcher_main.cpp)
target_compile_features(dqix-linux-ui-launcher PRIVATE cxx_std_17)
target_link_libraries(dqix-linux-ui-launcher PRIVATE dqix-linux-settings dqix-linux-builder PkgConfig::SDL2)

if(BUILD_TESTING)
    add_executable(dqix-linux-builder-tests tests/builder-tests.cpp)
    target_compile_features(dqix-linux-builder-tests PRIVATE cxx_std_17)
    target_link_libraries(dqix-linux-builder-tests PRIVATE dqix-linux-builder)
    add_test(NAME linux-builder-dry-run-success-failure
        COMMAND "$<TARGET_FILE:dqix-linux-builder-tests>")
    set_tests_properties(linux-builder-dry-run-success-failure PROPERTIES TIMEOUT 30)

    add_executable(dqix-linux-overlay-tests overlay/linux_overlay_test.c)
    target_compile_features(dqix-linux-overlay-tests PRIVATE c_std_11)
    target_link_libraries(dqix-linux-overlay-tests PRIVATE dqix-linux-overlay)
    add_test(NAME linux-ingame-overlay-raster COMMAND "$<TARGET_FILE:dqix-linux-overlay-tests>")

    add_executable(dqix-linux-settings-tests settings/settings_model_test.cpp)
    target_compile_features(dqix-linux-settings-tests PRIVATE cxx_std_17)
    target_link_libraries(dqix-linux-settings-tests PRIVATE dqix-linux-settings)
    add_test(NAME linux-settings-model-and-runtime-parity
        COMMAND "$<TARGET_FILE:dqix-linux-settings-tests>"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/frontend_sdl.c")
    add_test(NAME linux-ui-launcher-help COMMAND "$<TARGET_FILE:dqix-linux-ui-launcher>" --help)
    add_test(NAME linux-ui-sdl2-software-renderer-smoke
        COMMAND "${BASH_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke-ui.sh"
                "$<TARGET_FILE:dqix-linux-ui-launcher>")
    add_test(NAME linux-opengl-host-compat-shell-syntax
        COMMAND /bin/sh -n "${CMAKE_CURRENT_SOURCE_DIR}/tools/linux-gl-compat.sh")
    set_tests_properties(linux-ui-sdl2-software-renderer-smoke PROPERTIES TIMEOUT 15)
endif()

message(STATUS "DQIX Linux launcher: SDL2 + SDL_Renderer; no external UI toolkit or Vulkan")
message(STATUS "DQIX Linux in-game overlay: GL and SDL_Renderer helper compiled; live-game acceptance remains required")
