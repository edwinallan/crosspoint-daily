#!/usr/bin/env bash

set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if command -v pio >/dev/null 2>&1; then
  pio_cmd="$(command -v pio)"
elif [[ -x "${HOME}/.platformio/penv/bin/pio" ]]; then
  pio_cmd="${HOME}/.platformio/penv/bin/pio"
else
  echo "PlatformIO was not found. Install it or add 'pio' to PATH." >&2
  exit 1
fi

cd "${project_dir}"
exec "${pio_cmd}" run -e simulator_x3 -t run_simulator
