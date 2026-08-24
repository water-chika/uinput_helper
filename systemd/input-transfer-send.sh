#!/bin/sh
# Wrapper for input-transfer-send.service: reads a config file listing
# one input device per line and execs input_transfer with one -d flag
# per device, either connecting to a remote host or listening for one,
# depending on the environment.
#
# Devices config file format (default /etc/input-transfer/devices.conf,
# override via DEVICES_FILE in /etc/default/input-transfer-send):
#   - one device path per line (e.g. /dev/input/event3)
#   - blank lines and lines starting with '#' are ignored
#   - trailing/leading whitespace on each line is trimmed
#
# Environment (see /etc/default/input-transfer-send):
#   DEVICES_FILE - path to the devices config file (see above)
#   LISTEN=1     - listen for a peer instead of connecting out; uses
#                  LISTEN_PORT (default: input_transfer's own default)
#   REMOTE_HOST / REMOTE_PORT - peer to connect to, when LISTEN is unset
set -e

DEVICES_FILE="${DEVICES_FILE:-/etc/input-transfer/devices.conf}"

if [ ! -f "$DEVICES_FILE" ]; then
    echo "input-transfer-send: devices file '$DEVICES_FILE' not found" >&2
    exit 1
fi

set --
while IFS= read -r line || [ -n "$line" ]; do
    # trim leading/trailing whitespace
    trimmed=$(printf '%s' "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    case "$trimmed" in
        ''|'#'*) continue ;;
    esac
    set -- "$@" -d "$trimmed"
done < "$DEVICES_FILE"

if [ "$#" -eq 0 ]; then
    echo "input-transfer-send: no devices listed in '$DEVICES_FILE'" >&2
    exit 1
fi

if [ "${LISTEN:-0}" = "1" ]; then
    exec /usr/local/bin/input_transfer send "$@" --listen ${LISTEN_PORT:+"$LISTEN_PORT"}
else
    exec /usr/local/bin/input_transfer send "$@" "$REMOTE_HOST" "$REMOTE_PORT"
fi
