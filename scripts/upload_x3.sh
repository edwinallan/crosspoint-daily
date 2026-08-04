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

upload_port="${1:-}"

if [[ -z "${upload_port}" ]]; then
  detected_ports=()
  candidate_port=""

  while IFS= read -r line; do
    case "${line}" in
      /dev/cu.*|/dev/tty.*)
        candidate_port="${line}"
        ;;
      *"VID:PID=303A:"*)
        if [[ -n "${candidate_port}" ]]; then
          detected_ports+=("${candidate_port}")
        fi
        ;;
    esac
  done < <("${pio_cmd}" device list)

  if [[ ${#detected_ports[@]} -eq 0 ]]; then
    echo "No connected Espressif USB serial/JTAG device was found." >&2
    echo "Connect the Xteink X3 and try again, or pass its port explicitly:" >&2
    echo "  $0 /dev/cu.usbmodemXXXX" >&2
    exit 1
  fi

  if [[ ${#detected_ports[@]} -gt 1 ]]; then
    echo "More than one Espressif device was found. Pass the intended port:" >&2
    printf '  %s\n' "${detected_ports[@]}" >&2
    exit 1
  fi

  upload_port="${detected_ports[0]}"
fi

if [[ ! -e "${upload_port}" ]]; then
  echo "Serial port does not exist: ${upload_port}" >&2
  exit 1
fi

echo "Uploading the default firmware to ${upload_port}..."
cd "${project_dir}"
exec "${pio_cmd}" run -e default -t upload --upload-port "${upload_port}"
