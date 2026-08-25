/* input_transfer: transfer one or more Linux input devices (evdev) between
 * two machines over the network, in either direction, with either side
 * acting as the TCP listener or the connector.
 *
 * Usage:
 *   input_transfer send -d <input-device> [-d <input-device> ...] --listen [port]
 *   input_transfer send -d <input-device> [-d <input-device> ...] <host> [port]
 *   input_transfer receive --listen [port]
 *   input_transfer receive <host> [port]
 *
 * "send" opens each -d <input-device> (e.g. /dev/input/eventX), grabs it
 * exclusively (EVIOCGRAB, best-effort so it stops acting locally while
 * shared), and streams a stream header + all devices' capabilities
 * followed by a multiplexed (index-tagged) stream of their events to the
 * peer.
 *
 * "receive" recreates a matching virtual device locally via /dev/uinput
 * for each device announced by the peer, and replays every received
 * (index-tagged) event onto the matching device.
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
    fprintf(out, "usage: %s send -d <input-device> [-d <input-device> ...] --listen [port]\n",
            prog);
    fprintf(out, "       %s send -d <input-device> [-d <input-device> ...] <host> [port]\n",
            prog);
    fprintf(out, "       %s receive --listen [port]\n", prog);
    fprintf(out, "       %s receive <host> [port]\n", prog);
    fprintf(out, "\n");
    fprintf(out,
            "  send -d <input-device>: open and share this device (e.g. /dev/input/event3,\n");
    fprintf(out, "                         see /proc/bus/input/devices), grabbing it\n");
    fprintf(out, "                         exclusively so it stops acting locally. May be\n");
    fprintf(out, "                         given multiple times (up to %d) to share several\n",
            INPUT_NET_MAX_DEVICES);
    fprintf(out, "                         devices over one connection.\n");
    fprintf(out,
            "  receive: recreate every device announced by the peer, locally via /dev/uinput\n");
    fprintf(out, "  --listen [port]: wait for a peer to connect, instead of connecting to\n");
    fprintf(out, "                    one (default port %d, one peer at a time)\n",
            INPUT_NET_PORT_DEFAULT);
    fprintf(out,
            "  <host> [port]: connect to a peer's hostname or IP address, instead of\n");
    fprintf(out, "                 listening (default port %d)\n", INPUT_NET_PORT_DEFAULT);
    fprintf(out, "  -h, --help: show this help and exit\n");
}

/* Opens each of dev_paths[0..dev_count-1], grabs it exclusively
 * (best-effort), and sends all of them multiplexed over sock_fd until
 * the peer disconnects. Closes every opened fd before returning. */
static void run_send(const char **dev_paths, int dev_count, int sock_fd) {
    int dev_fds[INPUT_NET_MAX_DEVICES];
    int opened = 0;

    for (int i = 0; i < dev_count; i++) {
        int fd = open(dev_paths[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "open %s: %s\n", dev_paths[i], strerror(errno));
            goto cleanup;
        }
        if (ioctl(fd, EVIOCGRAB, 1) < 0) {
            fprintf(stderr, "EVIOCGRAB %s: %s (continuing without exclusive grab)\n",
                    dev_paths[i], strerror(errno));
        }
        dev_fds[i] = fd;
        opened++;
    }

    input_net_send_devices(dev_fds, dev_count, sock_fd);

cleanup:
    for (int i = 0; i < opened; i++) {
        close(dev_fds[i]);
    }
}

int main(int argc, char **argv) {
    input_net_ignore_sigpipe();

    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(stdout, argv[0]);
        return 0;
    }
    if (argc < 2) {
        print_usage(stderr, argv[0]);
        return 1;
    }

    enum transfer_direction direction;
    const char *dev_paths[INPUT_NET_MAX_DEVICES];
    int dev_count = 0;
    int next = 2;

    if (strcmp(argv[1], "send") == 0) {
        direction = DIRECTION_SEND;
        next = 2;
        while (next < argc && (strcmp(argv[next], "-d") == 0 || strcmp(argv[next], "--device") == 0)) {
            if (next + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", argv[next]);
                print_usage(stderr, argv[0]);
                return 1;
            }
            if (dev_count >= INPUT_NET_MAX_DEVICES) {
                fprintf(stderr, "too many devices (max %d)\n", INPUT_NET_MAX_DEVICES);
                return 1;
            }
            dev_paths[dev_count++] = argv[next + 1];
            next += 2;
        }
        if (dev_count == 0) {
            fprintf(stderr, "send requires at least one -d <input-device>\n");
            print_usage(stderr, argv[0]);
            return 1;
        }
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
            printf("Listening on port %d, will share %d device(s) with connecting peers\n", port,
                   dev_count);
        } else {
            printf("Listening on port %d, will receive device(s) from connecting peers\n", port);
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

            input_net_configure_socket(peer_fd);

            if (direction == DIRECTION_SEND) {
                run_send(dev_paths, dev_count, peer_fd);
            } else {
                input_net_receive_devices(peer_fd);
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
        int rc = 0;
        input_net_configure_socket(sock_fd);
        if (direction == DIRECTION_SEND) {
            printf("Connecting to %s:%d to send %d device(s)\n", host, port, dev_count);
            run_send(dev_paths, dev_count, sock_fd);
        } else {
            printf("Connecting to %s:%d to receive device(s)\n", host, port);
            rc = input_net_receive_devices(sock_fd);
        }

        close(sock_fd);
        return rc < 0 ? 1 : 0;
    }

    return 0;
}
