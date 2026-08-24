/* input_dump: opens a Linux evdev input device and prints every event it
 * generates in a human-readable form (timestamp, event type, code, value),
 * along with a summary of the device's capabilities on startup.
 *
 * Usage: input_dump <input-device>
 *
 * Useful for inspecting a device locally, and as a debugging companion to
 * input_server/input_client when transferring a device over the network.
 */
#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void print_usage(FILE *out, const char *prog) {
    fprintf(out, "usage: %s <input-device>\n", prog);
    fprintf(out, "  input-device: e.g. /dev/input/event3 (see /proc/bus/input/devices)\n");
    fprintf(out, "  -h, --help: show this help and exit\n");
}

static void print_capabilities(struct libevdev *dev) {
    printf("Device: %s\n", libevdev_get_name(dev));
    printf("Bus: %04x Vendor: %04x Product: %04x Version: %04x\n",
           libevdev_get_id_bustype(dev), libevdev_get_id_vendor(dev),
           libevdev_get_id_product(dev), libevdev_get_id_version(dev));

    printf("Supported events:\n");
    for (unsigned int type = 0; type < EV_MAX; type++) {
        if (!libevdev_has_event_type(dev, type)) {
            continue;
        }
        printf("  Event type %u (%s)\n", type, libevdev_event_type_get_name(type));
        if (type == EV_SYN) {
            continue;
        }
        int max_code = libevdev_event_type_get_max(type);
        for (int code = 0; max_code >= 0 && code <= max_code; code++) {
            if (!libevdev_has_event_code(dev, type, (unsigned int)code)) {
                continue;
            }
            const char *code_name = libevdev_event_code_get_name(type, (unsigned int)code);
            printf("    Event code %d (%s)", code, code_name ? code_name : "?");
            if (type == EV_ABS) {
                const struct input_absinfo *abs = libevdev_get_abs_info(dev, (unsigned int)code);
                if (abs != NULL) {
                    printf(" [min %d, max %d, fuzz %d, flat %d, resolution %d]", abs->minimum,
                           abs->maximum, abs->fuzz, abs->flat, abs->resolution);
                }
            }
            printf("\n");
        }
    }
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

    int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open device");
        return 1;
    }

    struct libevdev *dev = NULL;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        fprintf(stderr, "libevdev_new_from_fd failed: %s\n", strerror(-rc));
        close(fd);
        return 1;
    }

    print_capabilities(dev);
    printf("--------------------------------------------------------------------\n");
    printf("Listening for events (Ctrl-C to stop)...\n");

    for (;;) {
        struct input_event ev;
        rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING,
                                  &ev);
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            /* Device fell behind (dropped events); drain the sync events. */
            fprintf(stderr, "dropped events, resyncing...\n");
            while (rc == LIBEVDEV_READ_STATUS_SYNC) {
                rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
            }
            continue;
        }
        if (rc == -EAGAIN) {
            continue;
        }
        if (rc != LIBEVDEV_READ_STATUS_SUCCESS) {
            fprintf(stderr, "libevdev_next_event failed: %s\n", strerror(-rc));
            break;
        }

        const char *type_name = libevdev_event_type_get_name(ev.type);
        const char *code_name = libevdev_event_code_get_name(ev.type, ev.code);
        printf("[%ld.%06ld] type %u (%s), code %u (%s), value %d\n", (long)ev.input_event_sec,
               (long)ev.input_event_usec, ev.type, type_name ? type_name : "?", ev.code,
               code_name ? code_name : "?", ev.value);
    }

    libevdev_free(dev);
    close(fd);
    return 0;
}
