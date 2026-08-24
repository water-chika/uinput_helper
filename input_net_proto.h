#ifndef INPUT_NET_PROTO_H
#define INPUT_NET_PROTO_H

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
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
        perror("open /dev/uinput");
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
        perror("UI_DEV_SETUP");
        close(uinput_fd);
        return -1;
    }
    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        close(uinput_fd);
        return -1;
    }

    printf("Created virtual device '%s'\n", usetup.name);
    return uinput_fd;
}

/* Sends dev_fd's capabilities to sock_fd, then forwards every event read
 * from dev_fd to sock_fd until dev_fd fails to read or sock_fd fails to
 * write (peer disconnected). Intended for whichever side owns/opened the
 * real (or virtual) input device being shared, regardless of whether that
 * side is the TCP listener or the connector. */
static inline void input_net_send_device(int dev_fd, int sock_fd) {
    struct input_net_device_info info;
    input_net_fill_device_info(dev_fd, &info);

    if (input_net_write_full(sock_fd, &info, sizeof(info)) != (ssize_t)sizeof(info)) {
        perror("send device info");
        return;
    }
    printf("Sent device info: %s\n", info.name);

    struct input_event ev;
    for (;;) {
        ssize_t n = read(dev_fd, &ev, sizeof(ev));
        if (n < 0) {
            perror("read input device");
            break;
        }
        if (n != (ssize_t)sizeof(ev)) {
            fprintf(stderr, "short read from input device (%zd bytes)\n", n);
            break;
        }
        if (input_net_write_full(sock_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
            fprintf(stderr, "peer disconnected\n");
            break;
        }
    }
}

/* Receives device capabilities from sock_fd, recreates a matching virtual
 * device via uinput, and forwards every received event onto it until the
 * peer disconnects or an error occurs. Intended for whichever side wants
 * to receive/recreate the device, regardless of whether that side is the
 * TCP listener or the connector.
 * Returns 0 on a clean disconnect, -1 on error (including failure to
 * receive/parse the device info or create the uinput device). */
static inline int input_net_receive_device(int sock_fd) {
    struct input_net_device_info info;
    ssize_t n = input_net_read_full(sock_fd, &info, sizeof(info));
    if (n != (ssize_t)sizeof(info)) {
        fprintf(stderr, "failed to receive device info from peer\n");
        return -1;
    }
    if (info.magic != INPUT_NET_MAGIC) {
        fprintf(stderr, "bad protocol magic from peer (got 0x%08x)\n", info.magic);
        return -1;
    }
    info.name[sizeof(info.name) - 1] = '\0';

    int uinput_fd = input_net_create_uinput_device(&info);
    if (uinput_fd < 0) {
        return -1;
    }

    struct input_event ev;
    for (;;) {
        ssize_t r = input_net_read_full(sock_fd, &ev, sizeof(ev));
        if (r == 0) {
            fprintf(stderr, "peer closed connection\n");
            break;
        }
        if (r < 0) {
            perror("read from peer");
            break;
        }
        if (write(uinput_fd, &ev, sizeof(ev)) < 0) {
            perror("write uinput");
            break;
        }
    }

    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    return 0;
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
        fprintf(stderr, "failed to resolve %s: %s\n", host, gai_strerror(gai_rc));
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
        fprintf(stderr, "failed to connect to %s:%d: %s\n", host, port, strerror(errno));
    }
    return sock_fd;
}

/* Creates a TCP listening socket bound to all interfaces on the given
 * port (backlog of 1: peers are served one at a time). Returns the
 * listening fd, or -1 on failure (with a message already printed). */
static inline int input_net_listen(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
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
        perror("bind");
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 1) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }
    return listen_fd;
}

#endif /* INPUT_NET_PROTO_H */
