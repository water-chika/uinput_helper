#!/bin/bash
# Wrapper for keyboard-joystick.service: turns a local keyboard into two
# virtual Xbox 360 pads (keyboard_joystick) and forwards both of them to
# a peer with input_transfer, as one unit.
#
# It starts keyboard_joystick, waits for it to report the event nodes of
# its virtual pads, then runs "input_transfer send" on them. If either
# process exits, the other is stopped too, so systemd can restart the
# pair cleanly.
#
# Environment (see /etc/default/keyboard-joystick):
#   KEYBOARD_DEVICE - keyboard event node to read (required), e.g.
#                     /dev/input/by-id/usb-...-event-kbd
#   MAP_FILE        - optional key mapping file (keyboard_joystick -c);
#                     the built-in default mapping is used if unset or
#                     if the file does not exist
#   GRAB=1          - grab the keyboard exclusively, so its keys stop
#                     reaching the local desktop while playing
#   LISTEN=1        - listen for a peer instead of connecting out; uses
#                     LISTEN_PORT (default: input_transfer's own default)
#   REMOTE_HOST / REMOTE_PORT - peer to connect to, when LISTEN is unset
set -e

BIN_DIR="${BIN_DIR:-/usr/local/bin}"
# How long to wait for the virtual devices to appear, in 0.1s steps.
WAIT_STEPS="${WAIT_STEPS:-50}"

map_args=()
if [ -n "$MAP_FILE" ]; then
    if [ -f "$MAP_FILE" ]; then
        map_args=(-c "$MAP_FILE")
    else
        echo "keyboard-joystick: MAP_FILE '$MAP_FILE' not found, using the default mapping" >&2
    fi
fi

# Ask keyboard_joystick itself how many joysticks the mapping creates,
# instead of assuming the default two.
joystick_count=$("$BIN_DIR/keyboard_joystick" "${map_args[@]}" --print-map |
    awk '$1 ~ /^[0-9]+$/ { if ($1 > n) n = $1 } END { print n + 0 }')
if [ "${joystick_count:-0}" -lt 1 ]; then
    echo "keyboard-joystick: mapping defines no joysticks" >&2
    exit 1
fi

if [ -z "$KEYBOARD_DEVICE" ]; then
    echo "keyboard-joystick: KEYBOARD_DEVICE is not set" >&2
    exit 1
fi

# All virtual pads share one device name (they impersonate the same
# model), so keyboard_joystick reports their event nodes here instead.
node_file=$(mktemp /tmp/keyboard-joystick-nodes.XXXXXX)

kj_pid=""
transfer_pid=""

cleanup() {
    trap - TERM INT EXIT
    [ -n "$transfer_pid" ] && kill "$transfer_pid" 2>/dev/null
    [ -n "$kj_pid" ] && kill "$kj_pid" 2>/dev/null
    rm -f "$node_file"
    wait 2>/dev/null
}
trap cleanup TERM INT EXIT

grab_args=()
if [ "${GRAB:-0}" = "1" ]; then
    grab_args=(--grab)
fi

"$BIN_DIR/keyboard_joystick" -d "$KEYBOARD_DEVICE" "${grab_args[@]}" "${map_args[@]}" \
    --node-file "$node_file" &
kj_pid=$!

for _ in $(seq "$WAIT_STEPS"); do
    if ! kill -0 "$kj_pid" 2>/dev/null; then
        echo "keyboard-joystick: keyboard_joystick exited before creating its devices" >&2
        exit 1
    fi
    if [ "$(wc -l < "$node_file")" -ge "$joystick_count" ]; then
        break
    fi
    sleep 0.1
done

if [ "$(wc -l < "$node_file")" -lt "$joystick_count" ]; then
    echo "keyboard-joystick: timed out waiting for $joystick_count virtual device(s)" >&2
    exit 1
fi

nodes=()
while read -r index node; do
    echo "keyboard-joystick: joystick $index is $node"
    nodes+=(-d "$node")
done < "$node_file"

if [ "${LISTEN:-0}" = "1" ]; then
    "$BIN_DIR/input_transfer" send "${nodes[@]}" --listen ${LISTEN_PORT:+"$LISTEN_PORT"} &
else
    "$BIN_DIR/input_transfer" send "${nodes[@]}" "$REMOTE_HOST" "$REMOTE_PORT" &
fi
transfer_pid=$!

# Exit as soon as either half dies, so the whole pair gets restarted
# together instead of leaving a half-working setup behind.
while kill -0 "$kj_pid" 2>/dev/null && kill -0 "$transfer_pid" 2>/dev/null; do
    sleep 1
done
