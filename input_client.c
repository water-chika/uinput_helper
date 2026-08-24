/* input_client: connects to an input_server, receives the remote device's
 * capabilities, recreates it as a local virtual device via uinput, and
 * replays every received event onto it.
 *
 * Usage: input_client <server-ip> [port]
 */
#include "input_net_proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int create_uinput_device(const struct input_net_device_info *info) {
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

/* Resolves host (hostname or numeric IPv4/IPv6 address) and port, and
 * connects to the first address that succeeds. Returns a connected socket
 * fd, or -1 on failure. */
static int connect_to_server(const char *host, int port) {
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

static void print_usage(FILE *out, const char *prog) {
    fprintf(out, "usage: %s <server-host> [port]\n", prog);
    fprintf(out, "  server-host: hostname or IP address of the input_server\n");
    fprintf(out, "  port: TCP port the server listens on (default %d)\n", INPUT_NET_PORT_DEFAULT);
    fprintf(out, "  -h, --help: show this help and exit\n");
}

int main(int argc, char **argv) {
    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(stdout, argv[0]);
        return 0;
    }
    if (argc < 2) {
        print_usage(stderr, argv[0]);
        return 1;
    }
    const char *server_host = argv[1];
    int port = argc >= 3 ? atoi(argv[2]) : INPUT_NET_PORT_DEFAULT;

    int sock_fd = connect_to_server(server_host, port);
    if (sock_fd < 0) {
        return 1;
    }

    int one = 1;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct input_net_device_info info;
    ssize_t n = input_net_read_full(sock_fd, &info, sizeof(info));
    if (n != (ssize_t)sizeof(info)) {
        fprintf(stderr, "failed to receive device info from server\n");
        return 1;
    }
    if (info.magic != INPUT_NET_MAGIC) {
        fprintf(stderr, "bad protocol magic from server (got 0x%08x)\n", info.magic);
        return 1;
    }
    info.name[sizeof(info.name) - 1] = '\0';

    int uinput_fd = create_uinput_device(&info);
    if (uinput_fd < 0) {
        return 1;
    }

    printf("Forwarding events from %s:%d\n", server_host, port);

    struct input_event ev;
    for (;;) {
        ssize_t r = input_net_read_full(sock_fd, &ev, sizeof(ev));
        if (r == 0) {
            fprintf(stderr, "server closed connection\n");
            break;
        }
        if (r < 0) {
            perror("read from server");
            break;
        }
        if (write(uinput_fd, &ev, sizeof(ev)) < 0) {
            perror("write uinput");
            break;
        }
    }

    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    close(sock_fd);
    return 0;
}
