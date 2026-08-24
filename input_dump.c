/* input_dump: opens one or more Linux evdev input devices and prints every
 * event they generate in a human-readable form (device, timestamp, event
 * type, code, value), along with a summary of each device's capabilities on
 * startup.
 *
 * Usage:
 *   input_dump                    open every /dev/input/eventN device and
 *                                  multiplex their events
 *   input_dump <device> [device...]  open only the given device node(s)
 *
 * Useful for inspecting devices locally, and as a debugging companion to
 * input_server/input_client when transferring a device over the network.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_DEVICES 256
#define DEFAULT_INPUT_DIR "/dev/input"
#define PATH_MAX_LEN 256

struct tracked_device {
    int fd;
    struct libevdev *dev;
    char path[PATH_MAX_LEN];
};

static void print_usage(FILE *out, const char *prog) {
    fprintf(out, "usage: %s [input-device ...]\n", prog);
    fprintf(out, "  input-device: e.g. /dev/input/event3 (see /proc/bus/input/devices)\n");
    fprintf(out, "  with no arguments, opens every /dev/input/eventN device found\n");
    fprintf(out, "  and shows events from all of them\n");
    fprintf(out, "  -h, --help: show this help and exit\n");
}

static void print_capabilities(const char *path, struct libevdev *dev) {
    printf("Device: %s (%s)\n", path, libevdev_get_name(dev));
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

/* Fills paths[] with every /dev/input/eventN device node found, sorted by
 * name. Returns the number of entries filled (may be 0), or -1 on error. */
static int discover_event_devices(char paths[][PATH_MAX_LEN], int max_paths) {
    DIR *dir = opendir(DEFAULT_INPUT_DIR);
    if (dir == NULL) {
        perror("opendir " DEFAULT_INPUT_DIR);
        return -1;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        if (count >= max_paths) {
            fprintf(stderr, "warning: too many input devices, truncating list\n");
            break;
        }
        snprintf(paths[count], PATH_MAX_LEN, "%s/%s", DEFAULT_INPUT_DIR, entry->d_name);
        count++;
    }
    closedir(dir);

    /* Simple insertion sort by path for stable, predictable output. */
    for (int i = 1; i < count; i++) {
        char tmp[PATH_MAX_LEN];
        strncpy(tmp, paths[i], PATH_MAX_LEN);
        int j = i - 1;
        while (j >= 0 && strcmp(paths[j], tmp) > 0) {
            strncpy(paths[j + 1], paths[j], PATH_MAX_LEN);
            j--;
        }
        strncpy(paths[j + 1], tmp, PATH_MAX_LEN);
    }

    return count;
}

static void print_event(const char *path, struct libevdev *dev, const struct input_event *ev) {
    const char *type_name = libevdev_event_type_get_name(ev->type);
    const char *code_name = libevdev_event_code_get_name(ev->type, ev->code);
    (void)dev;
    printf("%s: [%ld.%06ld] type %u (%s), code %u (%s), value %d\n", path,
           (long)ev->input_event_sec, (long)ev->input_event_usec, ev->type,
           type_name ? type_name : "?", ev->code, code_name ? code_name : "?", ev->value);
}

int main(int argc, char **argv) {
    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(stdout, argv[0]);
        return 0;
    }

    char discovered_paths[MAX_DEVICES][PATH_MAX_LEN];
    const char **requested_paths = NULL;
    int path_count = 0;

    if (argc < 2) {
        path_count = discover_event_devices(discovered_paths, MAX_DEVICES);
        if (path_count < 0) {
            return 1;
        }
        if (path_count == 0) {
            fprintf(stderr, "no input devices found under " DEFAULT_INPUT_DIR "\n");
            return 1;
        }
        requested_paths = malloc(sizeof(char *) * (size_t)path_count);
        for (int i = 0; i < path_count; i++) {
            requested_paths[i] = discovered_paths[i];
        }
    } else {
        path_count = argc - 1;
        requested_paths = malloc(sizeof(char *) * (size_t)path_count);
        for (int i = 0; i < path_count; i++) {
            requested_paths[i] = argv[i + 1];
        }
    }

    struct tracked_device devices[MAX_DEVICES];
    struct pollfd pfds[MAX_DEVICES];
    int dev_count = 0;

    for (int i = 0; i < path_count && dev_count < MAX_DEVICES; i++) {
        const char *path = requested_paths[i];
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            fprintf(stderr, "skipping %s: %s\n", path, strerror(errno));
            continue;
        }

        struct libevdev *dev = NULL;
        int rc = libevdev_new_from_fd(fd, &dev);
        if (rc < 0) {
            fprintf(stderr, "skipping %s: libevdev_new_from_fd failed: %s\n", path,
                    strerror(-rc));
            close(fd);
            continue;
        }

        print_capabilities(path, dev);
        printf("--------------------------------------------------------------------\n");

        strncpy(devices[dev_count].path, path, PATH_MAX_LEN - 1);
        devices[dev_count].path[PATH_MAX_LEN - 1] = '\0';
        devices[dev_count].fd = fd;
        devices[dev_count].dev = dev;
        pfds[dev_count].fd = fd;
        pfds[dev_count].events = POLLIN;
        dev_count++;
    }

    free(requested_paths);

    if (dev_count == 0) {
        fprintf(stderr, "no input devices could be opened\n");
        return 1;
    }

    printf("Listening for events on %d device(s) (Ctrl-C to stop)...\n", dev_count);

    for (;;) {
        int ready = poll(pfds, (nfds_t)dev_count, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        for (int i = 0; i < dev_count; i++) {
            if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP))) {
                continue;
            }

            struct input_event ev;
            int rc = libevdev_next_event(devices[i].dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            while (rc == LIBEVDEV_READ_STATUS_SUCCESS || rc == LIBEVDEV_READ_STATUS_SYNC) {
                if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                    fprintf(stderr, "%s: dropped events, resyncing...\n", devices[i].path);
                    while (rc == LIBEVDEV_READ_STATUS_SYNC) {
                        rc = libevdev_next_event(devices[i].dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
                    }
                    continue;
                }
                print_event(devices[i].path, devices[i].dev, &ev);
                rc = libevdev_next_event(devices[i].dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            }
            if (rc != -EAGAIN) {
                fprintf(stderr, "%s: libevdev_next_event failed: %s, closing\n", devices[i].path,
                        strerror(-rc));
                libevdev_free(devices[i].dev);
                close(devices[i].fd);
                pfds[i].fd = -1; /* ignored by poll() */
                devices[i].fd = -1;
            }
        }
    }

    for (int i = 0; i < dev_count; i++) {
        if (devices[i].fd >= 0) {
            libevdev_free(devices[i].dev);
            close(devices[i].fd);
        }
    }

    return 0;
}
