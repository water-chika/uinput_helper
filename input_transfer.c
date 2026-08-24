/* input_transfer: transfer a Linux input device (evdev) between two
 * machines over the network, in either direction, with either side acting
 * as the TCP listener or the connector.
 *
 * Usage:
 *   input_transfer send <input-device> --listen [port]
 *   input_transfer send <input-device> <host> [port]
 *   input_transfer receive --listen [port]
 *   input_transfer receive <host> [port]
 *
 * "send" opens <input-device> (e.g. /dev/input/eventX), grabs it
 * exclusively (EVIOCGRAB, best-effort so it stops acting locally while
 * shared), and streams its capabilities followed by its events to the
 * peer.
 *
 * "receive" recreates a matching virtual device locally via /dev/uinput,
 * based on the capabilities received from the peer, and replays every
 * event received onto it.
 *
 * --listen makes this process wait for a peer to connect (serving one
 * peer at a time, repeatedly); otherwise it connects to <host>[:port]
 * (hostname or numeric IPv4/IPv6 address). Either "send" or "receive" can
 * be combined with either --listen or <host>, so e.g. a "send --listen"
 * process can be paired with a "receive <host>" process, or a
 * "receive --listen" process can be paired with a "send <host>" process.
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

enum transfer_direction {
    DIRECTION_SEND,
    DIRECTION_RECEIVE,
};

static void print_usage(FILE *out, const char *prog) {
    fprintf(out, "usage: %s send <input-device> --listen [port]\n", prog);
    fprintf(out, "       %s send <input-device> <host> [port]\n", prog);
    fprintf(out, "       %s receive --listen [port]\n", prog);
    fprintf(out, "       %s receive <host> [port]\n", prog);
    fprintf(out, "\n");
    fprintf(out,
            "  send <input-device>: open and share this device (e.g. /dev/input/event3,\n");
    fprintf(out, "                       see /proc/bus/input/devices), grabbing it\n");
    fprintf(out, "                       exclusively so it stops acting locally\n");
    fprintf(out, "  receive: recreate the peer's device locally via /dev/uinput\n");
    fprintf(out, "  --listen [port]: wait for a peer to connect, instead of connecting to\n");
    fprintf(out, "                    one (default port %d, one peer at a time)\n",
            INPUT_NET_PORT_DEFAULT);
    fprintf(out,
            "  <host> [port]: connect to a peer's hostname or IP address, instead of\n");
    fprintf(out, "                 listening (default port %d)\n", INPUT_NET_PORT_DEFAULT);
    fprintf(out, "  -h, --help: show this help and exit\n");
}

/* Opens <input-device>, grabs it exclusively (best-effort), and sends it
 * over sock_fd until the peer disconnects. */
static void run_send(const char *dev_path, int sock_fd) {
    int dev_fd = open(dev_path, O_RDONLY);
    if (dev_fd < 0) {
        perror("open device");
        return;
    }
    if (ioctl(dev_fd, EVIOCGRAB, 1) < 0) {
        perror("EVIOCGRAB (continuing without exclusive grab)");
    }
    input_net_send_device(dev_fd, sock_fd);
    close(dev_fd);
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

    enum transfer_direction direction;
    const char *dev_path = NULL;
    int next = 2;

    if (strcmp(argv[1], "send") == 0) {
        direction = DIRECTION_SEND;
        if (argc < 3) {
            fprintf(stderr, "send requires <input-device>\n");
            print_usage(stderr, argv[0]);
            return 1;
        }
        dev_path = argv[2];
        next = 3;
    } else if (strcmp(argv[1], "receive") == 0) {
        direction = DIRECTION_RECEIVE;
        next = 2;
    } else {
        fprintf(stderr, "unknown mode '%s' (expected 'send' or 'receive')\n", argv[1]);
        print_usage(stderr, argv[0]);
        return 1;
    }

    int listen_mode = 0;
    const char *host = NULL;
    int port = INPUT_NET_PORT_DEFAULT;

    if (next < argc && (strcmp(argv[next], "--listen") == 0 || strcmp(argv[next], "-l") == 0)) {
        listen_mode = 1;
        next++;
        if (next < argc) {
            port = atoi(argv[next]);
            next++;
        }
    } else if (next < argc) {
        host = argv[next];
        next++;
        if (next < argc) {
            port = atoi(argv[next]);
            next++;
        }
    } else {
        fprintf(stderr, "expected --listen or <host>\n");
        print_usage(stderr, argv[0]);
        return 1;
    }

    if (listen_mode) {
        int listen_fd = input_net_listen(port);
        if (listen_fd < 0) {
            return 1;
        }
        if (direction == DIRECTION_SEND) {
            printf("Listening on port %d, will share '%s' with connecting peers\n", port,
                   dev_path);
        } else {
            printf("Listening on port %d, will receive a device from connecting peers\n", port);
        }

        for (;;) {
            struct sockaddr_in peer_addr;
            socklen_t peer_len = sizeof(peer_addr);
            int peer_fd = accept(listen_fd, (struct sockaddr *)&peer_addr, &peer_len);
            if (peer_fd < 0) {
                perror("accept");
                continue;
            }
            printf("Peer connected: %s\n", inet_ntoa(peer_addr.sin_addr));

            int one = 1;
            setsockopt(peer_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

            if (direction == DIRECTION_SEND) {
                run_send(dev_path, peer_fd);
            } else {
                input_net_receive_device(peer_fd);
            }

            close(peer_fd);
            printf("Peer disconnected, waiting for next connection\n");
        }
        /* unreachable */
        close(listen_fd);
    } else {
        int sock_fd = input_net_connect(host, port);
        if (sock_fd < 0) {
            return 1;
        }
        int one = 1;
        setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        int rc = 0;
        if (direction == DIRECTION_SEND) {
            printf("Connecting to %s:%d to send '%s'\n", host, port, dev_path);
            run_send(dev_path, sock_fd);
        } else {
            printf("Connecting to %s:%d to receive a device\n", host, port);
            rc = input_net_receive_device(sock_fd);
        }

        close(sock_fd);
        return rc < 0 ? 1 : 0;
    }

    return 0;
}
