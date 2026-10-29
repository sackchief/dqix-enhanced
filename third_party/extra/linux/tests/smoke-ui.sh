#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 1 ]]; then
  echo "usage: $0 /path/to/dqix-linux-ui-launcher" >&2
  exit 2
fi
app=$1
if [[ ! -x "$app" ]]; then
  echo "FAIL: launcher is not executable: $app" >&2
  exit 1
fi
log_file=$(mktemp)
trap 'rm -f "$log_file"' EXIT
if ! SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 15 "$app" --self-test >"$log_file" 2>&1; then
  cat "$log_file"
  echo "FAIL: SDL2 software-renderer smoke test failed." >&2
  exit 1
fi
cat "$log_file"
grep -Fq "SDL2 software-renderer smoke test passed." "$log_file"
