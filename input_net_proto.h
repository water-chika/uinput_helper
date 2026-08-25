#ifndef INPUT_NET_PROTO_H
#define INPUT_NET_PROTO_H

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* Simple network protocol to transfer a Linux input device (evdev) between
 * two peers: one side owns a real (or virtual) input device and sends it,
 * the other receives it and recreates it locally with uinput. Either
 * input_server or input_client can play either role - see
 * input_net_send_device()/input_net_receive_device() below - independently
 * of which side initiates the TCP connection.
 *
 * Wire format:
 *   1. struct input_net_device_info, sent once right after connect.
 *   2. A stream of "struct input_event" (as defined by the kernel ABI),
 *      one per event, forwarded verbatim until either side disconnects.
 *
 * This assumes client and server run on machines with matching
 * struct input_event layout/endianness (e.g. same architecture family),
 * which is the common case for local network use.
 */

#define INPUT_NET_MAGIC ((uint32_t)0x494e4554u) /* "INET" */
#define INPUT_NET_PORT_DEFAULT 9111

/* Log verbosity, ordered from quietest to loudest: each level also emits
 * everything above it in this list. "quiet" silences even errors, for
 * callers that only care about the exit status. */
enum input_net_log_level {
    INPUT_NET_LOG_QUIET = 0,
    INPUT_NET_LOG_ERROR,
    INPUT_NET_LOG_WARN,
    INPUT_NET_LOG_INFO,
    INPUT_NET_LOG_DEBUG,
    INPUT_NET_LOG_TRACE,
};

#define INPUT_NET_LOG_LEVEL_DEFAULT INPUT_NET_LOG_INFO

static enum input_net_log_level input_net_log_level = INPUT_NET_LOG_LEVEL_DEFAULT;

static inline const char *input_net_log_level_name(enum input_net_log_level level) {
    switch (level) {
    case INPUT_NET_LOG_QUIET:
        return "quiet";
    case INPUT_NET_LOG_ERROR:
        return "error";
    case INPUT_NET_LOG_WARN:
        return "warn";
    case INPUT_NET_LOG_INFO:
        return "info";
    case INPUT_NET_LOG_DEBUG:
        return "debug";
    case INPUT_NET_LOG_TRACE:
        return "trace";
    }
    return "?";
}

/* Parses a level given by name ("info") or by number ("3"). Returns 0 and
 * stores the level on success, -1 if the string names no level. */
static inline int input_net_log_level_parse(const char *text, enum input_net_log_level *out) {
    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    for (int level = INPUT_NET_LOG_QUIET; level <= INPUT_NET_LOG_TRACE; level++) {
        if (strcmp(text, input_net_log_level_name((enum input_net_log_level)level)) == 0) {
            *out = (enum input_net_log_level)level;
            return 0;
        }
    }
    if (text[1] == '\0' && text[0] >= '0' && text[0] <= '0' + INPUT_NET_LOG_TRACE) {
        *out = (enum input_net_log_level)(text[0] - '0');
        return 0;
    }
    return -1;
}

static inline void input_net_set_log_level(enum input_net_log_level level) {
    input_net_log_level = level;
}

static inline int input_net_log_enabled(enum input_net_log_level level) {
    return level <= input_net_log_level;
}

/* Logs a message if level is enabled. Errors and warnings go to stderr so
 * they stay visible when stdout is redirected/piped; informational output
 * stays on stdout as before. Both are line-buffered explicitly via fflush
 * so logs appear in real time under systemd/redirection. */
__attribute__((format(printf, 2, 3))) static inline void
input_net_log(enum input_net_log_level level, const char *fmt, ...) {
    if (!input_net_log_enabled(level)) {
        return;
    }

    FILE *out = (level <= INPUT_NET_LOG_WARN) ? stderr : stdout;
    fprintf(out, "[%s] ", input_net_log_level_name(level));

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fputc('\n', out);
    fflush(out);
}

/* Like input_net_log(), but appends ": <strerror(errno)>" - the
 * perror() replacement that honours the log level. errno is preserved so
 * callers can still inspect it afterwards. */
__attribute__((format(printf, 2, 3))) static inline void
input_net_log_errno(enum input_net_log_level level, const char *fmt, ...) {
    int saved_errno = errno;
    if (!input_net_log_enabled(level)) {
        return;
    }

    FILE *out = (level <= INPUT_NET_LOG_WARN) ? stderr : stdout;
    fprintf(out, "[%s] ", input_net_log_level_name(level));

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fprintf(out, ": %s\n", strerror(saved_errno));
    fflush(out);
    errno = saved_errno;
}

#define INPUT_NET_BITS_TO_BYTES(bit_count) (((bit_count) / 8) + 1)

struct input_net_device_info {
    uint32_t magic;
    char name[UINPUT_MAX_NAME_SIZE];
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
    uint8_t ev_bits[INPUT_NET_BITS_TO_BYTES(EV_MAX)];
    uint8_t key_bits[INPUT_NET_BITS_TO_BYTES(KEY_MAX)];
    uint8_t rel_bits[INPUT_NET_BITS_TO_BYTES(REL_MAX)];
    uint8_t abs_bits[INPUT_NET_BITS_TO_BYTES(ABS_MAX)];
    struct input_absinfo absinfo[ABS_MAX + 1];
};

static inline int input_net_bit_is_set(const uint8_t *bits, int bit) {
    return (bits[bit / 8] >> (bit % 8)) & 1;
}

static inline void input_net_set_bit(uint8_t *bits, int bit) {
    bits[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

/* Sent once right after connect: how many devices follow, then that many
 * struct input_net_device_info records (one per shared device, in the
 * same order events will be tagged with below). */
struct input_net_stream_header {
    uint32_t magic;
    uint32_t device_count;
};

/* Every event on the wire after the header/device-info records is
 * prefixed with the (0-based) index of the device it belongs to (into
 * the device_count devices announced above), so a single TCP connection
 * can multiplex events from any number of devices. */
struct input_net_tagged_event {
    uint32_t device_index;
    struct input_event ev;
};

#define INPUT_NET_MAX_DEVICES 32

/* An input stream can legitimately be idle for hours (nobody touching the
 * device), so a silently broken connection (NAT/conntrack timeout, Wi-Fi
 * drop, peer machine powered off) is otherwise indistinguishable from
 * "no input right now": both peers would block forever and the transfer
 * would appear to stall, with the sender still holding EVIOCGRAB on the
 * real device. TCP keepalive both keeps middlebox state alive during idle
 * periods and turns a dead peer into a read/write error within roughly
 * INPUT_NET_KEEPALIVE_IDLE + INPUT_NET_KEEPALIVE_CNT *
 * INPUT_NET_KEEPALIVE_INTVL seconds. */
#define INPUT_NET_KEEPALIVE_IDLE 30
#define INPUT_NET_KEEPALIVE_INTVL 10
#define INPUT_NET_KEEPALIVE_CNT 3

/* Upper bound on how long a single write to the peer may block before it
 * is treated as a failed connection. Only hit if the peer stops draining
 * the socket entirely, which for an input stream means it is gone. */
#define INPUT_NET_SEND_TIMEOUT 30

/* Applies the socket options every input_transfer connection wants,
 * regardless of direction or which side listened: low latency (events are
 * tiny and latency-sensitive) plus dead-peer detection (see above). */
static inline void input_net_configure_socket(int sock_fd) {
    int one = 1;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(sock_fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

#ifdef TCP_KEEPIDLE
    int idle = INPUT_NET_KEEPALIVE_IDLE;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
    int intvl = INPUT_NET_KEEPALIVE_INTVL;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#ifdef TCP_KEEPCNT
    int cnt = INPUT_NET_KEEPALIVE_CNT;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif

    struct timeval snd_timeout;
    snd_timeout.tv_sec = INPUT_NET_SEND_TIMEOUT;
    snd_timeout.tv_usec = 0;
    setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &snd_timeout, sizeof(snd_timeout));

    input_net_log(INPUT_NET_LOG_DEBUG,
                  "socket configured: nodelay, keepalive idle=%ds intvl=%ds cnt=%d, "
                  "send timeout=%ds",
                  INPUT_NET_KEEPALIVE_IDLE, INPUT_NET_KEEPALIVE_INTVL, INPUT_NET_KEEPALIVE_CNT,
                  INPUT_NET_SEND_TIMEOUT);
}

/* A peer that disappears makes write() raise SIGPIPE, whose default action
 * kills the process without any diagnostic. Ignoring it turns those writes
 * into plain EPIPE errors the send/receive loops already handle. */
static inline void input_net_ignore_sigpipe(void) {
    signal(SIGPIPE, SIG_IGN);
}


/* Reads exactly len bytes, retrying on EINTR/short reads.
 * Returns len on success, 0 on clean EOF before any byte, -1 on error. */
static inline ssize_t input_net_read_full(int fd, void *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n == 0) {
            return (ssize_t)(len - left) == 0 ? 0 : -1;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    return (ssize_t)len;
}

/* Writes exactly len bytes, retrying on EINTR/short writes.
 * Returns len on success, -1 on error. */
static inline ssize_t input_net_write_full(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    return (ssize_t)len;
}

/* Queries an open evdev fd (dev_fd) for its name/id/capabilities and fills
 * info accordingly, ready to be sent to a peer. */
static inline void input_net_fill_device_info(int dev_fd, struct input_net_device_info *info) {
    memset(info, 0, sizeof(*info));
    info->magic = INPUT_NET_MAGIC;

    char name[UINPUT_MAX_NAME_SIZE] = {0};
    if (ioctl(dev_fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        strncpy(name, "networked-input-device", sizeof(name) - 1);
    }
    strncpy(info->name, name, sizeof(info->name) - 1);

    struct input_id id;
    if (ioctl(dev_fd, EVIOCGID, &id) == 0) {
        info->bustype = id.bustype;
        info->vendor = id.vendor;
        info->product = id.product;
        info->version = id.version;
    } else {
        info->bustype = BUS_VIRTUAL;
    }

    ioctl(dev_fd, EVIOCGBIT(0, sizeof(info->ev_bits)), info->ev_bits);
    ioctl(dev_fd, EVIOCGBIT(EV_KEY, sizeof(info->key_bits)), info->key_bits);
    ioctl(dev_fd, EVIOCGBIT(EV_REL, sizeof(info->rel_bits)), info->rel_bits);
    ioctl(dev_fd, EVIOCGBIT(EV_ABS, sizeof(info->abs_bits)), info->abs_bits);

    for (int code = 0; code <= ABS_MAX; code++) {
        if (input_net_bit_is_set(info->abs_bits, code)) {
            ioctl(dev_fd, EVIOCGABS(code), &info->absinfo[code]);
        }
    }
}

/* Creates a uinput virtual device matching the capabilities in info.
 * Returns an open fd on success (device already created via
 * UI_DEV_CREATE), or -1 on failure. Caller should eventually
 * ioctl(fd, UI_DEV_DESTROY) and close(fd). */
static inline int input_net_create_uinput_device(const struct input_net_device_info *info) {
    int uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) {
        input_net_log_errno(INPUT_NET_LOG_ERROR, "open /dev/uinput");
        return -1;
    }

    for (int ev_type = 0; ev_type <= EV_MAX; ev_type++) {
        if (input_net_bit_is_set(info->ev_bits, ev_type)) {
            ioctl(uinput_fd, UI_SET_EVBIT, ev_type);
        }
    }
    for (int code = 0; code <= KEY_MAX; code++) {
        if (input_net_bit_is_set(info->key_bits, code)) {
            ioctl(uinput_fd, UI_SET_KEYBIT, code);
        }
    }
    for (int code = 0; code <= REL_MAX; code++) {
        if (input_net_bit_is_set(info->rel_bits, code)) {
            ioctl(uinput_fd, UI_SET_RELBIT, code);
        }
    }
    for (int code = 0; code <= ABS_MAX; code++) {
        if (input_net_bit_is_set(info->abs_bits, code)) {
            ioctl(uinput_fd, UI_SET_ABSBIT, code);

            struct uinput_abs_setup abs_setup;
            memset(&abs_setup, 0, sizeof(abs_setup));
            abs_setup.code = (uint16_t)code;
            abs_setup.absinfo = info->absinfo[code];
            ioctl(uinput_fd, UI_ABS_SETUP, &abs_setup);
        }
    }

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = info->bustype ? info->bustype : BUS_VIRTUAL;
    usetup.id.vendor = info->vendor;
    usetup.id.product = info->product;
    usetup.id.version = info->version;
    strncpy(usetup.name, info->name[0] ? info->name : "networked-input-device",
            sizeof(usetup.name) - 1);

    if (ioctl(uinput_fd, UI_DEV_SETUP, &usetup) < 0) {
        input_net_log_errno(INPUT_NET_LOG_ERROR, "UI_DEV_SETUP");
        close(uinput_fd);
        return -1;
    }
    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        input_net_log_errno(INPUT_NET_LOG_ERROR, "UI_DEV_CREATE");
        close(uinput_fd);
        return -1;
    }

    input_net_log(INPUT_NET_LOG_INFO, "Created virtual device '%s'", usetup.name);
    return uinput_fd;
}

/* How many forwarded events between periodic debug-level throughput
 * lines. Individual events are only logged at trace level, which is far
 * too loud for anything but short debugging sessions. */
#define INPUT_NET_LOG_EVENT_INTERVAL 100

/* Logs one input event (device index, type/code/value) at the given
 * level; direction is a short verb like "sent" or "replayed". */
static inline void input_net_log_event(enum input_net_log_level level, const char *direction,
                                       int dev_index, const struct input_event *ev) {
    if (!input_net_log_enabled(level)) {
        return;
    }
    input_net_log(level, "%s device[%d] type=0x%04x code=0x%04x value=%d", direction, dev_index,
                  (unsigned)ev->type, (unsigned)ev->code, ev->value);
}

/* Logs a summary of a device's announced capabilities at the given
 * level: its id, how many key/rel/abs codes it supports, and each abs
 * axis' range. */
static inline void input_net_log_device_info(enum input_net_log_level level, int dev_index,
                                             const struct input_net_device_info *info) {
    if (!input_net_log_enabled(level)) {
        return;
    }

    int key_count = 0, rel_count = 0, abs_count = 0;
    for (int code = 0; code <= KEY_MAX; code++) {
        key_count += input_net_bit_is_set(info->key_bits, code);
    }
    for (int code = 0; code <= REL_MAX; code++) {
        rel_count += input_net_bit_is_set(info->rel_bits, code);
    }
    for (int code = 0; code <= ABS_MAX; code++) {
        abs_count += input_net_bit_is_set(info->abs_bits, code);
    }

    input_net_log(level,
                  "device[%d] '%s' bus=0x%04x vendor=0x%04x product=0x%04x version=0x%04x "
                  "keys=%d rels=%d abs=%d",
                  dev_index, info->name, (unsigned)info->bustype, (unsigned)info->vendor,
                  (unsigned)info->product, (unsigned)info->version, key_count, rel_count,
                  abs_count);

    for (int code = 0; code <= ABS_MAX; code++) {
        if (input_net_bit_is_set(info->abs_bits, code)) {
            const struct input_absinfo *abs = &info->absinfo[code];
            input_net_log(level, "device[%d]   abs code 0x%02x: min=%d max=%d fuzz=%d flat=%d",
                          dev_index, (unsigned)code, abs->minimum, abs->maximum, abs->fuzz,
                          abs->flat);
        }
    }
}

/* Sends the capabilities of dev_fds[0..dev_count-1] to sock_fd (stream
 * header + one input_net_device_info per device), then multiplexes
 * events read from any of them (via poll()) to sock_fd, each tagged with
 * its device's index, until every device fd errors/closes or sock_fd
 * fails to write (peer disconnected). Intended for whichever side
 * owns/opens the real (or virtual) input devices being shared,
 * regardless of whether that side is the TCP listener or the connector. */
static inline void input_net_send_devices(const int *dev_fds, int dev_count, int sock_fd) {
    if (dev_count <= 0 || dev_count > INPUT_NET_MAX_DEVICES) {
        input_net_log(INPUT_NET_LOG_ERROR, "invalid device count %d", dev_count);
        return;
    }

    struct input_net_stream_header header;
    header.magic = INPUT_NET_MAGIC;
    header.device_count = (uint32_t)dev_count;
    if (input_net_write_full(sock_fd, &header, sizeof(header)) != (ssize_t)sizeof(header)) {
        input_net_log_errno(INPUT_NET_LOG_ERROR, "send stream header");
        return;
    }

    for (int i = 0; i < dev_count; i++) {
        struct input_net_device_info info;
        input_net_fill_device_info(dev_fds[i], &info);
        if (input_net_write_full(sock_fd, &info, sizeof(info)) != (ssize_t)sizeof(info)) {
            input_net_log_errno(INPUT_NET_LOG_ERROR, "send device info");
            return;
        }
        input_net_log(INPUT_NET_LOG_INFO, "Sent device info [%d]: %s", i, info.name);
        input_net_log_device_info(INPUT_NET_LOG_DEBUG, i, &info);
    }

    /* One slot per device plus a trailing slot for the socket itself: the
     * peer never sends anything back, so the socket only becomes readable
     * on EOF/error. Watching it means a peer that goes away while the
     * devices are idle is noticed right away, instead of only when the
     * next event happens to be written (which for a device nobody touches
     * may be never). */
    struct pollfd pfds[INPUT_NET_MAX_DEVICES + 1];
    for (int i = 0; i < dev_count; i++) {
        pfds[i].fd = dev_fds[i];
        pfds[i].events = POLLIN;
    }
    pfds[dev_count].fd = sock_fd;
    pfds[dev_count].events = POLLIN;

    int active = dev_count;
    uint64_t sent_events = 0;
    while (active > 0) {
        int ready = poll(pfds, (nfds_t)(dev_count + 1), -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            input_net_log_errno(INPUT_NET_LOG_ERROR, "poll");
            break;
        }

        if (pfds[dev_count].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
            input_net_log(INPUT_NET_LOG_INFO, "peer disconnected");
            break;
        }

        int stop = 0;
        for (int i = 0; i < dev_count && !stop; i++) {
            if (pfds[i].fd < 0 || !(pfds[i].revents & (POLLIN | POLLERR | POLLHUP))) {
                continue;
            }

            struct input_net_tagged_event tagged;
            tagged.device_index = (uint32_t)i;
            ssize_t n = read(pfds[i].fd, &tagged.ev, sizeof(tagged.ev));
            if (n != (ssize_t)sizeof(tagged.ev)) {
                if (n < 0) {
                    input_net_log_errno(INPUT_NET_LOG_ERROR, "read input device");
                } else {
                    input_net_log(INPUT_NET_LOG_WARN, "device [%d] closed/short read", i);
                }
                pfds[i].fd = -1;
                active--;
                continue;
            }
            if (input_net_write_full(sock_fd, &tagged, sizeof(tagged)) != (ssize_t)sizeof(tagged)) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    input_net_log(INPUT_NET_LOG_ERROR, "peer stopped reading for %ds, giving up",
                                  INPUT_NET_SEND_TIMEOUT);
                } else {
                    input_net_log_errno(INPUT_NET_LOG_INFO, "peer disconnected");
                }
                stop = 1;
                continue;
            }
            input_net_log_event(INPUT_NET_LOG_TRACE, "sent", i, &tagged.ev);
            sent_events++;
            if ((sent_events % INPUT_NET_LOG_EVENT_INTERVAL) == 0) {
                input_net_log(INPUT_NET_LOG_DEBUG, "forwarded %llu events to peer",
                              (unsigned long long)sent_events);
            }
        }
        if (stop) {
            break;
        }
    }
}

/* Convenience wrapper for a single device. */
static inline void input_net_send_device(int dev_fd, int sock_fd) {
    input_net_send_devices(&dev_fd, 1, sock_fd);
}

/* Receives the stream header and each device's capabilities from
 * sock_fd, recreates a matching virtual device via uinput for each, and
 * forwards every received (tagged) event onto the matching device until
 * the peer disconnects or an error occurs. Intended for whichever side
 * wants to receive/recreate the devices, regardless of whether that side
 * is the TCP listener or the connector.
 * Returns 0 on a clean disconnect, -1 on error (including failure to
 * receive/parse the stream header/device info or create a uinput
 * device). */
static inline int input_net_receive_devices(int sock_fd) {
    struct input_net_stream_header header;
    ssize_t n = input_net_read_full(sock_fd, &header, sizeof(header));
    if (n != (ssize_t)sizeof(header)) {
        input_net_log(INPUT_NET_LOG_ERROR, "failed to receive stream header from peer");
        return -1;
    }
    if (header.magic != INPUT_NET_MAGIC) {
        input_net_log(INPUT_NET_LOG_ERROR, "bad protocol magic from peer (got 0x%08x)", header.magic);
        return -1;
    }
    if (header.device_count == 0 || header.device_count > INPUT_NET_MAX_DEVICES) {
        input_net_log(INPUT_NET_LOG_ERROR, "peer announced invalid device count %u",
                      (unsigned)header.device_count);
        return -1;
    }

    int uinput_fds[INPUT_NET_MAX_DEVICES];
    int dev_count = (int)header.device_count;
    int rc = 0;

    for (int i = 0; i < dev_count; i++) {
        struct input_net_device_info info;
        ssize_t r = input_net_read_full(sock_fd, &info, sizeof(info));
        if (r != (ssize_t)sizeof(info)) {
            input_net_log(INPUT_NET_LOG_ERROR, "failed to receive device info [%d] from peer", i);
            dev_count = i;
            rc = -1;
            goto cleanup;
        }
        info.name[sizeof(info.name) - 1] = '\0';
        input_net_log_device_info(INPUT_NET_LOG_DEBUG, i, &info);

        uinput_fds[i] = input_net_create_uinput_device(&info);
        if (uinput_fds[i] < 0) {
            dev_count = i;
            rc = -1;
            goto cleanup;
        }
    }

    input_net_log(INPUT_NET_LOG_DEBUG, "replaying events for %d device(s)", dev_count);

    uint64_t received_events = 0;
    for (;;) {
        struct input_net_tagged_event tagged;
        ssize_t r = input_net_read_full(sock_fd, &tagged, sizeof(tagged));
        if (r == 0) {
            input_net_log(INPUT_NET_LOG_INFO, "peer closed connection");
            break;
        }
        if (r < 0) {
            input_net_log_errno(INPUT_NET_LOG_ERROR, "read from peer");
            break;
        }
        if (tagged.device_index >= (uint32_t)dev_count) {
            input_net_log(INPUT_NET_LOG_ERROR, "bad device index %u from peer",
                          (unsigned)tagged.device_index);
            break;
        }
        if (write(uinput_fds[tagged.device_index], &tagged.ev, sizeof(tagged.ev)) < 0) {
            input_net_log_errno(INPUT_NET_LOG_ERROR, "write uinput");
            break;
        }
        input_net_log_event(INPUT_NET_LOG_TRACE, "replayed", (int)tagged.device_index, &tagged.ev);
        received_events++;
        if ((received_events % INPUT_NET_LOG_EVENT_INTERVAL) == 0) {
            input_net_log(INPUT_NET_LOG_DEBUG, "replayed %llu events from peer",
                          (unsigned long long)received_events);
        }
    }

cleanup:
    for (int i = 0; i < dev_count; i++) {
        ioctl(uinput_fds[i], UI_DEV_DESTROY);
        close(uinput_fds[i]);
    }
    return rc;
}

/* Convenience wrapper matching the old single-device receive API. */
static inline int input_net_receive_device(int sock_fd) {
    return input_net_receive_devices(sock_fd);
}

/* Resolves host (hostname or numeric IPv4/IPv6 address) and port via
 * getaddrinfo, and connects to the first address that succeeds. Returns a
 * connected socket fd, or -1 on failure (with a message already printed). */
static inline int input_net_connect(const char *host, int port) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = NULL;
    int gai_rc = getaddrinfo(host, port_str, &hints, &result);
    if (gai_rc != 0) {
        input_net_log(INPUT_NET_LOG_ERROR, "failed to resolve %s: %s", host, gai_strerror(gai_rc));
        return -1;
    }

    int sock_fd = -1;
    for (struct addrinfo *rp = result; rp != NULL; rp = rp->ai_next) {
        sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd < 0) {
            continue;
        }
        if (connect(sock_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(sock_fd);
        sock_fd = -1;
    }

    freeaddrinfo(result);

    if (sock_fd < 0) {
        input_net_log_errno(INPUT_NET_LOG_ERROR, "failed to connect to %s:%d", host, port);
    }
    return sock_fd;
}

/* Creates a TCP listening socket bound to all interfaces on the given
 * port (backlog of 1: peers are served one at a time). Returns the
 * listening fd, or -1 on failure (with a message already printed). */
static inline int input_net_listen(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        input_net_log_errno(INPUT_NET_LOG_ERROR, "socket");
        return -1;
    }
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        input_net_log_errno(INPUT_NET_LOG_ERROR, "bind");
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 1) < 0) {
        input_net_log_errno(INPUT_NET_LOG_ERROR, "listen");
        close(listen_fd);
        return -1;
    }
    return listen_fd;
}

#endif /* INPUT_NET_PROTO_H */
