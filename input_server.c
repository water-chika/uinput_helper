/* input_server: reads events from a real input device (e.g. /dev/input/eventX)
 * and streams them to a connected input_client over TCP, so the device can be
 * "shared" onto another machine's uinput.
 *
 * Usage: input_server <input-device> [port]
 */
#include "input_net_proto.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int fill_device_info(int dev_fd, struct input_net_device_info *info) {
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

    return 0;
}

static void serve_client(int dev_fd, int client_fd, const struct input_net_device_info *info) {
    int one = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (input_net_write_full(client_fd, info, sizeof(*info)) != (ssize_t)sizeof(*info)) {
        perror("send device info");
        return;
    }

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
        if (input_net_write_full(client_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
            fprintf(stderr, "client disconnected\n");
            break;
        }
    }
}

static void print_usage(FILE *out, const char *prog) {
    fprintf(out, "usage: %s <input-device> [port]\n", prog);
    fprintf(out, "  input-device: e.g. /dev/input/event3 (see /proc/bus/input/devices)\n");
    fprintf(out, "  port: TCP port to listen on (default %d)\n", INPUT_NET_PORT_DEFAULT);
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
    const char *dev_path = argv[1];
    int port = argc >= 3 ? atoi(argv[2]) : INPUT_NET_PORT_DEFAULT;

    int dev_fd = open(dev_path, O_RDONLY);
    if (dev_fd < 0) {
        perror("open device");
        return 1;
    }

    /* Grab the device exclusively so events are not also delivered locally
     * while they are being forwarded to the client. Not fatal if it fails. */
    if (ioctl(dev_fd, EVIOCGRAB, 1) < 0) {
        perror("EVIOCGRAB (continuing without exclusive grab)");
    }

    struct input_net_device_info info;
    fill_device_info(dev_fd, &info);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
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
        return 1;
    }
    if (listen(listen_fd, 1) < 0) {
        perror("listen");
        return 1;
    }

    printf("Serving input device '%s' (%s) on port %d\n", dev_path, info.name, port);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }
        printf("Client connected: %s\n", inet_ntoa(client_addr.sin_addr));

        serve_client(dev_fd, client_fd, &info);

        close(client_fd);
        printf("Client disconnected, waiting for next connection\n");
    }

    close(dev_fd);
    close(listen_fd);
    return 0;
}
