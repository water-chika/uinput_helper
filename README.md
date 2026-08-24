# uinput_helper

Small helper programs built on top of Linux's `uinput`/`evdev` interfaces.

## uinput_test

Interactively creates a virtual joystick device via `/dev/uinput` and lets
you drive its axes/buttons from stdin (`x <value>`, `y <value>`,
`a <0|1>`, `b <0|1>`).

## Transferring an input device over the network

`input_server` and `input_client` let you take a real input device
(mouse, keyboard, joystick, etc.) attached to one machine and make it
appear as a virtual device on another machine, over a plain TCP
connection.

- **input_server** opens a real evdev device (e.g. `/dev/input/eventX`),
  grabs it exclusively so events stop being delivered locally
  (`EVIOCGRAB`), reads its capabilities (event/key/rel/abs bits and
  abs axis info), and listens for TCP clients. Once a client connects,
  it sends the device's capabilities once, then streams every
  `input_event` it reads from the device to the client.

- **input_client** connects to `input_server`, receives the device
  capabilities, creates a matching virtual device via `/dev/uinput`
  (same event/key/rel/abs bits and abs axis ranges), and replays every
  event it receives onto that virtual device.

### Usage

On the machine that owns the physical device:

```sh
# find the device node, e.g. from /proc/bus/input/devices or evtest
sudo ./input_server /dev/input/event3 [port]   # port defaults to 9111
```

On the machine that should receive the device:

```sh
sudo ./input_client <server-ip> [port]
```

Both `input_server` (reading `/dev/input/eventX` and `EVIOCGRAB`) and
`input_client` (writing to `/dev/uinput`) typically require root, or
appropriate udev permissions on those device nodes.

Notes:
- Only one client is served at a time; `input_server` accepts a new
  client after the previous one disconnects.
- The protocol (see `input_net_proto.h`) assumes client and server run
  with a compatible `struct input_event` ABI (matching architecture
  word size/endianness), which holds for typical same-family Linux
  hosts on a local network.

## Inspecting a device's events

`input_dump` opens an evdev device, prints a summary of its capabilities
(supported event types/codes, and abs axis ranges), then prints every
event it receives in a human-readable form (timestamp, type, code,
value) using `libevdev`'s name lookups.

```sh
sudo ./input_dump /dev/input/event3
```

Handy for finding the right `/dev/input/eventX` node and its capabilities
before pointing `input_server` at it, or for verifying what a device
actually sends. Requires `libevdev` (`pkg-config libevdev`) to build.
