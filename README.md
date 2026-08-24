# uinput_helper

Small helper programs built on top of Linux's `uinput`/`evdev` interfaces.

## uinput_test

Interactively creates a virtual joystick device via `/dev/uinput` and lets
you drive its axes/buttons from stdin (`x <value>`, `y <value>`,
`a <0|1>`, `b <0|1>`).

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
