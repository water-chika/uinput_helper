# uinput_helper

Small helper programs built on top of Linux's `uinput`/`evdev` interfaces.

## uinput_test

Interactively creates a virtual joystick device via `/dev/uinput` and lets
you drive its axes/buttons from stdin (`x <value>`, `y <value>`,
`a <0|1>`, `b <0|1>`).

## Transferring an input device over the network

`input_transfer` lets you take a real input device (mouse, keyboard,
joystick, etc.) attached to one machine and make it appear as a virtual
device on another machine, over a plain TCP connection. Either side can
send or receive, and either side can be the one that listens for a
connection vs. the one that connects out — pick whichever combination
suits your network (e.g. the side without a routable/open port should
usually be the one that connects out).

- **send** opens a real evdev device (e.g. `/dev/input/eventX`), grabs
  it exclusively so events stop being delivered locally (`EVIOCGRAB`,
  best-effort), reads its capabilities (event/key/rel/abs bits and abs
  axis info), and sends them once to the peer, followed by every
  `input_event` read from the device.

- **receive** waits for a peer's device capabilities, creates a
  matching virtual device via `/dev/uinput` (same event/key/rel/abs
  bits and abs axis ranges), and replays every event received onto
  that virtual device.

### Usage

```sh
input_transfer send <input-device> --listen [port]   # share a device, waiting for a peer
input_transfer send <input-device> <host> [port]     # share a device, connecting to a peer
input_transfer receive --listen [port]               # recreate a device, waiting for a peer
input_transfer receive <host> [port]                 # recreate a device, connecting to a peer
```

`port` defaults to `9111`. `<host>` may be a hostname (resolved via
DNS/`/etc/hosts`) or a numeric IPv4/IPv6 address.

For example, to share the physical device at `/dev/input/event3` from
machine A to machine B, with A listening for the connection:

```sh
# on machine A (owns the physical device)
sudo ./input_transfer send /dev/input/event3 --listen

# on machine B (should receive the device)
sudo ./input_transfer receive machine-a.local
```

Or with B listening instead (e.g. because A is behind NAT):

```sh
# on machine B
sudo ./input_transfer receive --listen

# on machine A
sudo ./input_transfer send /dev/input/event3 machine-b.local
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
  a local network.

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
