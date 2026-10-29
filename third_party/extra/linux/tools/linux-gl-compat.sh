#!/bin/sh
# Host GLX compatibility for the Linux-only game process.
#
# The bundled Conda toolchain supplies SDL2 and GLVND, but its X11/XCB/DRM
# libraries can conflict with the host Mesa GLX driver on some systems. Resolve
# host library paths using ldconfig and preload them only into the game process.
# The SDL2 builder UI itself keeps its normal video driver and environment.

_dqix_linux_gl_resolve_host_library() {
    _dqix_lgl_soname=$1
    _dqix_lgl_conda_prefix=${CONDA_PREFIX-}

    if command -v ldconfig >/dev/null 2>&1; then
        _dqix_lgl_candidate="$(ldconfig -p 2>/dev/null | awk -v soname="$_dqix_lgl_soname" '$1 == soname { print $NF; exit }' || true)"
        if [ -n "$_dqix_lgl_candidate" ] && [ -r "$_dqix_lgl_candidate" ]; then
            if [ -n "$_dqix_lgl_conda_prefix" ]; then
                case "$_dqix_lgl_candidate" in
                    "$_dqix_lgl_conda_prefix"/*) ;;
                    *) printf '%s\n' "$_dqix_lgl_candidate"; return 0 ;;
                esac
            else
                printf '%s\n' "$_dqix_lgl_candidate"
                return 0
            fi
        fi
    fi

    for _dqix_lgl_dir in /usr/lib /usr/lib64 /lib /lib64 /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu; do
        _dqix_lgl_candidate="$_dqix_lgl_dir/$_dqix_lgl_soname"
        if [ -r "$_dqix_lgl_candidate" ]; then
            if [ -n "$_dqix_lgl_conda_prefix" ]; then
                case "$_dqix_lgl_candidate" in
                    "$_dqix_lgl_conda_prefix"/*) continue ;;
                esac
            fi
            printf '%s\n' "$_dqix_lgl_candidate"
            return 0
        fi
    done
    return 1
}

_dqix_linux_gl_merge_paths() {
    printf '%s\n%s\n' "$1" "$2" |
        awk -F: '{
            for (i = 1; i <= NF; i++) {
                if ($i != "" && !seen[$i]++) {
                    printf "%s%s", separator, $i
                    separator = ":"
                }
            }
        }'
}

dqix_linux_gl_prepare() {
    # The verified workaround needs an X11 display. Respect a caller that
    # explicitly selected a different SDL backend, including headless tests.
    case "${SDL_VIDEODRIVER-}" in
        ""|x11) ;;
        *) unset DQIX_LINUX_GL_OVERRIDE_DIR DQIX_LINUX_GL_PRELOAD; return 0 ;;
    esac
    if [ -z "${DISPLAY-}" ]; then
        unset DQIX_LINUX_GL_OVERRIDE_DIR DQIX_LINUX_GL_PRELOAD
        return 0
    fi

    _dqix_lgl_override=${DQIX_LINUX_GL_OVERRIDE_DIR-}
    if [ -z "$_dqix_lgl_override" ]; then
        _dqix_lgl_cache_root=${XDG_CACHE_HOME-}
        if [ -z "$_dqix_lgl_cache_root" ]; then
            _dqix_lgl_home=${HOME-}
            [ -n "$_dqix_lgl_home" ] || _dqix_lgl_home=/tmp
            _dqix_lgl_cache_root="$_dqix_lgl_home/.cache"
        fi
        _dqix_lgl_override="$_dqix_lgl_cache_root/dqix-enhanced-appimage/system-gl-override"
    fi
    if ! mkdir -p "$_dqix_lgl_override" 2>/dev/null; then
        unset DQIX_LINUX_GL_OVERRIDE_DIR DQIX_LINUX_GL_PRELOAD
        return 0
    fi
    chmod 700 "$_dqix_lgl_override" 2>/dev/null || true

    _dqix_lgl_preload=
    for _dqix_lgl_soname in \
        libdrm.so.2 \
        libdrm_amdgpu.so.1 \
        libdrm_intel.so.1 \
        libX11-xcb.so.1 \
        libxcb-dri3.so.0 \
        libxcb-glx.so.0 \
        libxcb-present.so.0 \
        libxcb-randr.so.0 \
        libxcb-shm.so.0 \
        libxcb-sync.so.1 \
        libxcb-xfixes.so.0
    do
        _dqix_lgl_host_path="$(_dqix_linux_gl_resolve_host_library "$_dqix_lgl_soname")" || continue
        _dqix_lgl_link="$_dqix_lgl_override/$_dqix_lgl_soname"
        # Do not replace an unexpected regular file in the user's cache.
        if [ -e "$_dqix_lgl_link" ] && [ ! -L "$_dqix_lgl_link" ]; then
            continue
        fi
        if ln -sfn "$_dqix_lgl_host_path" "$_dqix_lgl_link" 2>/dev/null; then
            if [ -n "$_dqix_lgl_preload" ]; then
                _dqix_lgl_preload="$_dqix_lgl_preload:$_dqix_lgl_link"
            else
                _dqix_lgl_preload=$_dqix_lgl_link
            fi
        fi
    done

    if [ -n "$_dqix_lgl_preload" ]; then
        DQIX_LINUX_GL_OVERRIDE_DIR="$_dqix_lgl_override"
        DQIX_LINUX_GL_PRELOAD="$_dqix_lgl_preload"
        export DQIX_LINUX_GL_OVERRIDE_DIR DQIX_LINUX_GL_PRELOAD
        _dqix_lgl_debug=${DQIX_LINUX_GL_DEBUG-0}
        if [ "$_dqix_lgl_debug" = "1" ]; then
            printf '[linux-gl-compat] host preload prepared: %s\n' "$DQIX_LINUX_GL_PRELOAD" >&2
        fi
    else
        unset DQIX_LINUX_GL_OVERRIDE_DIR DQIX_LINUX_GL_PRELOAD
        _dqix_lgl_debug=${DQIX_LINUX_GL_DEBUG-0}
        if [ "$_dqix_lgl_debug" = "1" ]; then
            printf '[linux-gl-compat] no compatible host X11/DRM libraries found; retaining default SDL environment\n' >&2
        fi
    fi
    return 0
}

dqix_linux_gl_apply() {
    dqix_linux_gl_prepare
    [ -n "${DQIX_LINUX_GL_PRELOAD-}" ] || return 0
    [ -n "${DQIX_LINUX_GL_OVERRIDE_DIR-}" ] || return 0

    _dqix_lgl_existing_preload=${LD_PRELOAD-}
    _dqix_lgl_existing_library_path=${LD_LIBRARY_PATH-}
    LD_PRELOAD="$(_dqix_linux_gl_merge_paths "$DQIX_LINUX_GL_PRELOAD" "$_dqix_lgl_existing_preload")"
    LD_LIBRARY_PATH="$(_dqix_linux_gl_merge_paths "$DQIX_LINUX_GL_OVERRIDE_DIR" "$_dqix_lgl_existing_library_path")"
    export LD_PRELOAD LD_LIBRARY_PATH
    # This workaround is verified with X11/XWayland + GLX on the affected host.
    # Scope it to the game process; the native SDL2 builder may remain on Wayland.
    SDL_VIDEODRIVER=x11
    SDL_VIDEO_X11_FORCE_EGL=0
    export SDL_VIDEODRIVER SDL_VIDEO_X11_FORCE_EGL
    _dqix_lgl_debug=${DQIX_LINUX_GL_DEBUG-0}
    if [ "$_dqix_lgl_debug" = "1" ]; then
        printf '[linux-gl-compat] game environment: SDL_VIDEODRIVER=x11, SDL_VIDEO_X11_FORCE_EGL=0\n' >&2
        printf '[linux-gl-compat] LD_LIBRARY_PATH=%s\n' "$LD_LIBRARY_PATH" >&2
    fi
}
