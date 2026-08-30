# uinput_helper

Small helper programs built on top of Linux's `uinput`/`evdev` interfaces.

## uinput_test

Interactively creates a virtual joystick device via `/dev/uinput` and lets
you drive its axes/buttons from stdin (`x <value>`, `y <value>`,
`a <0|1>`, `b <0|1>`).

## Emulating joysticks with a keyboard

`keyboard_joystick` reads a real keyboard's evdev node and turns its key
presses into independent virtual joysticks created via `/dev/uinput`, so
two players can share one keyboard, or a game that only speaks joystick
can be driven from the keyboard.

```sh
keyboard_joystick -d <keyboard-device> [--grab] [-c <map-file>]
                  [-m <joystick>:<key>:<target> ...] [--print-map]
```

- `-d, --device <dev>`: the keyboard's event node (e.g.
  `/dev/input/event3`; find it with `input_dump` or
  `/proc/bus/input/devices`).
- `-g, --grab`: grab the keyboard exclusively (`EVIOCGRAB`), so its keys
  stop reaching the desktop while this runs. Off by default, i.e. keys
  keep working normally and are only mirrored onto the joysticks.
- `-c, --config <file>`: load the key mapping from a file (see
  "Configuring the mapping" below), replacing the default mapping.
- `-m, --map <joystick>:<key>:<target>`: add a single mapping rule,
  repeatable; also replaces the default mapping.
- `--print-map`: print the mapping that would be used and exit.
- `-h, --help`: show the usage, mapping syntax and default mapping.

Each virtual device ("Keyboard Joystick 1", "Keyboard Joystick 2", ...)
exposes `ABS_X`/`ABS_Y` (range -512..512, `flat` 30, same as
`uinput_test`) and `BTN_A`/`BTN_B`/`BTN_X`/`BTN_Y`.

The default mapping drives two joysticks:

| Key (joystick 1) | Key (joystick 2) | Effect |
| --- | --- | --- |
| `W` / `S` | `Up` / `Down` | `ABS_Y` -512 / +512 |
| `A` / `D` | `Left` / `Right` | `ABS_X` -512 / +512 |
| `G` | `\` | `BTN_A` (south) |
| `H` | `Enter` | `BTN_B` (east) |
| `T` | `Right Shift` | `BTN_X` (north) |
| `F` | `Right Ctrl` | `BTN_Y` (west) |

Holding both directions of an axis cancels out (the axis returns to 0),
key auto-repeat is ignored, and all changes belonging to one keyboard
report are emitted before a single `SYN_REPORT`. `SIGINT`/`SIGTERM`
destroy the virtual devices and release the grab cleanly.

Needs read access to the keyboard's event node and to `/dev/uinput`
(i.e. root, or suitable udev permissions).

### Configuring the mapping

A mapping file (`-c`) holds one rule per line, and `-m` takes the same
three fields separated by `:` instead of whitespace:

```
<joystick> <key> <target>
```

- `<joystick>` — joystick number starting at 1 (up to 8). **The highest
  number used decides how many virtual joysticks are created**, so a
  four-player layout is just a matter of listing rules for joysticks
  1 to 4.
- `<key>` — key name, case-insensitive, with an optional `KEY_` prefix:
  `W`, `w`, `KEY_W`, `LEFTSHIFT`, `BACKSLASH`, `KP1`, `F1`, ...
- `<target>` — `UP`, `DOWN`, `LEFT` or `RIGHT` for an axis direction, or
  a button name `A`, `B`, `X` or `Y` (`BTN_` prefix optional).

Blank lines and `#` comments are ignored. Several keys may drive the
same target, and one key may drive several targets. A file (or any `-m`
rule) replaces the built-in mapping entirely, so list every rule you
want.

`--print-map` writes the effective mapping in exactly this syntax, so it
doubles as a way to bootstrap a file:

```sh
keyboard_joystick --print-map > my-layout.conf
$EDITOR my-layout.conf
keyboard_joystick -d /dev/input/event3 -c my-layout.conf
```

`systemd/keyboard-joystick.conf.example` is a ready-made file
reproducing the default mapping. Quick one-off tweaks need no file:

```sh
keyboard_joystick -d /dev/input/event3 -m 1:kp8:up -m 1:kp2:down -m 1:space:a
```

## Transferring input devices over the network

`input_transfer` lets you take one or more real input devices (mouse,
keyboard, joystick, etc.) attached to one machine and make them appear
as virtual devices on another machine, over a single plain TCP
connection. Either side can send or receive, and either side can be the
one that listens for a connection vs. the one that connects out — pick
whichever combination suits your network (e.g. the side without a
routable/open port should usually be the one that connects out).

- **send** opens each `-d <input-device>` given (e.g. `/dev/input/eventX`),
  grabs it exclusively so events stop being delivered locally
  (`EVIOCGRAB`, best-effort), reads its capabilities (event/key/rel/abs
  bits and abs axis info), and sends a stream header plus every
  device's capabilities once to the peer, followed by a multiplexed
  stream of `input_event`s read from all the devices (each tagged with
  which device it came from) via `poll()`.

- **receive** waits for the peer's stream header and each device's
  capabilities, creates one matching virtual device per announced
  device via `/dev/uinput` (same event/key/rel/abs bits and abs axis
  ranges), and replays every received event onto the matching virtual
  device.

### Usage

```sh
input_transfer send -d <input-device> [-d <input-device> ...] --listen [port]   # share device(s), waiting for a peer
input_transfer send -d <input-device> [-d <input-device> ...] <host> [port]     # share device(s), connecting to a peer
input_transfer receive --listen [port]                                         # recreate device(s), waiting for a peer
input_transfer receive <host> [port]                                           # recreate device(s), connecting to a peer
```

Any of these accept `--log-level <level>` (or `-L <level>`) anywhere on
the command line to control verbosity; `$INPUT_TRANSFER_LOG_LEVEL` sets
the same thing when no flag is given (an explicit flag wins).

`-d <input-device>` may be repeated (up to 32 times) to share several
devices over one connection; `receive` needs no device arguments — it
recreates whatever the peer announces. `port` defaults to `9111`.
`<host>` may be a hostname (resolved via DNS/`/etc/hosts`) or a numeric
IPv4/IPv6 address.

For example, to share the physical devices at `/dev/input/event3` and
`/dev/input/event4` from machine A to machine B, with A listening for
the connection:

```sh
# on machine A (owns the physical devices)
sudo ./input_transfer send -d /dev/input/event3 -d /dev/input/event4 --listen

# on machine B (should receive the devices)
sudo ./input_transfer receive machine-a.local
```

Or with B listening instead (e.g. because A is behind NAT):

```sh
# on machine B
sudo ./input_transfer receive --listen

# on machine A
sudo ./input_transfer send -d /dev/input/event3 -d /dev/input/event4 machine-b.local
```

`send` (reading `/dev/input/eventX` and `EVIOCGRAB`) and `receive`
(writing to `/dev/uinput`) typically require root, or appropriate udev
permissions on those device nodes.

Notes:
- Connections use TCP keepalive (30s idle, 3 probes 10s apart) and
  `TCP_NODELAY`. An input stream can legitimately be idle for hours, so
  without keepalive a connection killed silently — NAT/conntrack
  timeout, Wi-Fi drop, peer powered off — is indistinguishable from
  "nobody is touching the device", and both sides would block forever
  (the transfer appearing to stall, with `send` still holding
  `EVIOCGRAB` on the real device). Keepalive both keeps middlebox state
  alive while idle and turns a dead peer into an error within about a
  minute. `send` additionally polls the socket alongside the device
  nodes, so it notices a peer that vanishes during an idle period
  immediately (releasing the grabbed device and, in `--listen` mode,
  going back to waiting for a peer), and gives up on a write that
  blocks for more than 30s.
- In `--listen` mode, only one peer is served at a time;
  `input_transfer` accepts a new peer after the previous one
  disconnects.
- The protocol (see `input_net_proto.h`) assumes both sides run with a
  compatible `struct input_event` ABI (matching architecture word
  size/endianness), which holds for typical same-family Linux hosts on
  a local network. One stream header + device-info record per device is
  sent once at the start of each connection; every subsequent event is
  tagged with a device index so devices are multiplexed over the one
  connection.

### Log levels

Every message is tagged with its level (`[info] ...`); errors and
warnings go to stderr, everything else to stdout. Levels, quietest
first — each level also logs everything quieter than itself, and may be
given by name or by number (`0`-`5`):

| Level | What it adds |
| --- | --- |
| `quiet` | nothing at all, only the exit status |
| `error` | failures (connect/bind/open/protocol errors) |
| `warn` | non-fatal problems (e.g. a failed `EVIOCGRAB`) |
| `info` | connection and device lifecycle — the default, i.e. the previous behaviour |
| `debug` | socket settings, each device's announced capabilities and abs ranges, and an event counter every 100 events |
| `trace` | every single forwarded/replayed event (device index, type, code, value) |

```sh
input_transfer send -d /dev/input/event3 --listen --log-level debug
input_transfer receive --listen -L trace     # very loud, debugging only
INPUT_TRANSFER_LOG_LEVEL=warn input_transfer receive --listen
```

`trace` writes a line per event on both peers, so it easily floods a
terminal or the journal during normal mouse movement — prefer `debug`
unless individual events are what you are after.

### Running as systemd services

`systemd/` contains ready-to-use unit files to run `input_transfer` as
a persistent service, restarting on failure/disconnect:

- `input-transfer-send.service` + `input-transfer-send.env` +
  `input-transfer-send.sh` — the service runs the `input-transfer-send.sh`
  wrapper script, which reads the device list from a config file (one
  `/dev/input/eventN` path per line, `#` comments and blank lines
  ignored — see `devices.conf.example`) and execs `input_transfer send`
  with one `-d <device>` per line, either connecting out to
  `$REMOTE_HOST:$REMOTE_PORT` or, if `LISTEN=1`, listening on
  `$LISTEN_PORT` instead. This way the device list can be edited (and
  the service restarted) without touching the unit file, and doesn't
  depend on shell word-splitting of an environment variable.
Both `.env` files also carry a commented-out
`INPUT_TRANSFER_LOG_LEVEL=` line for setting the service's verbosity
(see "Log levels" above) without editing the unit.

- `input-transfer-receive.service` + `input-transfer-receive.env` —
  runs `input_transfer receive --listen $LISTEN_PORT`, i.e. waits for a
  peer to connect and recreates its device(s) locally (`receive` takes
  no device arguments — it recreates whatever the peer announces, so it
  needs no devices config file).

Install (as root), on the machine owning the physical device(s):

```sh
install -m755 build/input_transfer /usr/local/bin/input_transfer
install -m755 systemd/input-transfer-send.sh /usr/local/bin/input-transfer-send.sh
install -m644 systemd/input-transfer-send.service /etc/systemd/system/
install -m600 systemd/input-transfer-send.env /etc/default/input-transfer-send
mkdir -p /etc/input-transfer
install -m644 systemd/devices.conf.example /etc/input-transfer/devices.conf
$EDITOR /etc/input-transfer/devices.conf   # list the device node(s) to share
$EDITOR /etc/default/input-transfer-send   # set REMOTE_HOST/REMOTE_PORT (or LISTEN=1/LISTEN_PORT)
systemctl daemon-reload
systemctl enable --now input-transfer-send.service
```

And on the receiving machine:

```sh
install -m755 build/input_transfer /usr/local/bin/input_transfer
install -m644 systemd/input-transfer-receive.service /etc/systemd/system/
install -m600 systemd/input-transfer-receive.env /etc/default/input-transfer-receive
$EDITOR /etc/default/input-transfer-receive   # set LISTEN_PORT
systemctl daemon-reload
systemctl enable --now input-transfer-receive.service
```

Both services run as root (`Type=simple`, restart on failure) since
`send` needs to read the `/dev/input/eventX` node(s) and `EVIOCGRAB`
them, and `receive` needs to write to `/dev/uinput`; adjust `User=`/
`DeviceAllow=`/udev rules if you want to run unprivileged with scoped
device permissions instead. To pair a listening sender with a
connecting receiver instead, set `LISTEN=1` (and optionally
`LISTEN_PORT`) in `/etc/default/input-transfer-send` and point
`input-transfer-receive.service`'s peer at it via `input_transfer
receive <host> [port]` on the other machine.

`input-transfer-receive@.service` is a templated variant that takes the
listen port from the instance name, for running several independent
receivers side by side (each restartable on its own):

```sh
install -m644 systemd/input-transfer-receive@.service /etc/systemd/system/
systemctl enable --now input-transfer-receive@9112.service   # listens on 9112
```

### Playing a two-player game across machines

`systemd/keyboard-joystick.{service,env,sh}` combine
`keyboard_joystick` and `input_transfer send` into one service: it
turns a local keyboard into the two virtual joysticks, waits for both
to appear, and forwards them to the machine running the game. If either
half dies the wrapper exits so systemd restarts the pair together.

On the machine with the keyboard:

```sh
install -m755 build/keyboard_joystick /usr/local/bin/keyboard_joystick
install -m755 systemd/keyboard-joystick.sh /usr/local/bin/keyboard-joystick.sh
install -m644 systemd/keyboard-joystick.service /etc/systemd/system/
install -m600 systemd/keyboard-joystick.env /etc/default/keyboard-joystick
$EDITOR /etc/default/keyboard-joystick   # KEYBOARD_DEVICE, GRAB, REMOTE_HOST/REMOTE_PORT
# optional: a custom key mapping instead of the built-in one
mkdir -p /etc/keyboard-joystick
install -m644 systemd/keyboard-joystick.conf.example /etc/keyboard-joystick/map.conf
$EDITOR /etc/keyboard-joystick/map.conf   # then set MAP_FILE= in the env file
systemctl daemon-reload
systemctl enable --now keyboard-joystick.service
```

On the machine running the game, run a receiver on the matching port
(`input-transfer-receive@9112.service` above). Use a port of its own if
another `input_transfer` pair (e.g. a forwarded mouse) already uses
9111, so restarting the joysticks doesn't disturb it.

`MAP_FILE` is passed to `keyboard_joystick -c`, and the wrapper asks
`keyboard_joystick --print-map` how many joysticks that mapping defines,
so a mapping with more than two joysticks is forwarded as-is.

Point `KEYBOARD_DEVICE` at a `/dev/input/by-id/...-event-kbd` path
rather than `/dev/input/eventN`, which can change across reboots.
**`GRAB=1` takes that keyboard away from its own machine's desktop**, so
make sure it isn't the only keyboard you can type on there (or that you
can reach the machine over SSH to `systemctl stop keyboard-joystick`).

## Inspecting a device's events

`input_dump` opens one or more evdev devices, prints a summary of each
device's capabilities (supported event types/codes, and abs axis
ranges), then prints every event received from any of them in a
human-readable form (device path, timestamp, type, code, value) using
`libevdev`'s name lookups.

```sh
sudo ./input_dump /dev/input/event3      # dump a single device
sudo ./input_dump                        # dump every /dev/input/eventN device
```

With no arguments it discovers and opens every `/dev/input/eventN`
device node (skipping any it can't open, e.g. due to permissions) and
multiplexes their events with `poll()`, each line prefixed by the
originating device path.

Handy for finding the right `/dev/input/eventX` node and its capabilities
before pointing `input_transfer` at it, or for verifying what a device
actually sends. Requires `libevdev` (`pkg-config libevdev`) to build.
