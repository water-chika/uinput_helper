/* keyboard_joystick: emulate two virtual joysticks driven by one real
 * keyboard.
 *
 * Usage:
 *   keyboard_joystick -d <keyboard-device> [--grab]
 *
 * Reads key events from a keyboard's evdev node (e.g. /dev/input/event3,
 * see /proc/bus/input/devices or run input_dump) and translates them into
 * two independent virtual joysticks created via /dev/uinput, so two
 * players can share one keyboard, or a single game that only speaks
 * joystick can be driven from the keyboard.
 *
 * Default mapping:
 *   Joystick 1: W/A/S/D axes, buttons in the T/F/G/H diamond:
 *               G (A / south), H (B / east), T (X / north), F (Y / west)
 *   Joystick 2: arrow keys axes, buttons \ (A), Enter (B),
 *               Right Shift (X), Right Ctrl (Y)
 *
 * --grab takes the keyboard exclusively (EVIOCGRAB), so the mapped keys
 * (and every other key on that keyboard) stop reaching the desktop while
 * this runs. Off by default, so keys keep working normally and only get
 * mirrored onto the joysticks.
 *
 * Needs read access to the keyboard's event node and to /dev/uinput
 * (i.e. root, or suitable udev permissions).
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define JOYSTICK_COUNT 2

/* Full-deflection value reported on ABS_X/ABS_Y; matches uinput_test's
 * -512..512 range so both tools look alike to applications. */
#define AXIS_MAX 512
#define AXIS_FLAT 30

enum mapping_kind {
    MAP_AXIS,
    MAP_BUTTON,
};

struct key_mapping {
    int key;              /* KEY_* code coming from the real keyboard */
    int joystick;         /* which virtual joystick it drives */
    enum mapping_kind kind;
    int code;             /* ABS_* for MAP_AXIS, BTN_* for MAP_BUTTON */
    int direction;        /* MAP_AXIS only: -1 or +1 */
    const char *label;    /* human readable key name, for --help/startup log */
};

/* Axis index within a joystick's state, so an axis' two keys can be
 * tracked independently and cancel each other out when held together. */
#define AXIS_INDEX_X 0
#define AXIS_INDEX_Y 1

static const struct key_mapping key_mappings[] = {
    /* Joystick 1 */
    {KEY_W, 0, MAP_AXIS, ABS_Y, -1, "W"},
    {KEY_S, 0, MAP_AXIS, ABS_Y, +1, "S"},
    {KEY_A, 0, MAP_AXIS, ABS_X, -1, "A"},
    {KEY_D, 0, MAP_AXIS, ABS_X, +1, "D"},
    {KEY_G, 0, MAP_BUTTON, BTN_A, 0, "G"},
    {KEY_H, 0, MAP_BUTTON, BTN_B, 0, "H"},
    {KEY_T, 0, MAP_BUTTON, BTN_X, 0, "T"},
    {KEY_F, 0, MAP_BUTTON, BTN_Y, 0, "F"},

    /* Joystick 2 */
    {KEY_UP, 1, MAP_AXIS, ABS_Y, -1, "Up"},
    {KEY_DOWN, 1, MAP_AXIS, ABS_Y, +1, "Down"},
    {KEY_LEFT, 1, MAP_AXIS, ABS_X, -1, "Left"},
    {KEY_RIGHT, 1, MAP_AXIS, ABS_X, +1, "Right"},
    {KEY_BACKSLASH, 1, MAP_BUTTON, BTN_A, 0, "Backslash"},
    {KEY_ENTER, 1, MAP_BUTTON, BTN_B, 0, "Enter"},
    {KEY_RIGHTSHIFT, 1, MAP_BUTTON, BTN_X, 0, "Right Shift"},
    {KEY_RIGHTCTRL, 1, MAP_BUTTON, BTN_Y, 0, "Right Ctrl"},
};

#define KEY_MAPPING_COUNT ((int)(sizeof(key_mappings) / sizeof(key_mappings[0])))

struct joystick {
    int fd;
    /* [axis][0] = negative key held, [axis][1] = positive key held */
    int axis_keys[2][2];
    int axis_value[2];
    int dirty;
};

static volatile sig_atomic_t stop_requested = 0;

static void handle_stop_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static const char *button_name(int code) {
    switch (code) {
    case BTN_A:
        return "A";
    case BTN_B:
        return "B";
    case BTN_X:
        return "X";
    case BTN_Y:
        return "Y";
    default:
        return "?";
    }
}

static void print_mapping(FILE *out) {
    for (int joy = 0; joy < JOYSTICK_COUNT; joy++) {
        fprintf(out, "  Joystick %d:\n", joy + 1);
        for (int i = 0; i < KEY_MAPPING_COUNT; i++) {
            const struct key_mapping *m = &key_mappings[i];
            if (m->joystick != joy) {
                continue;
            }
            if (m->kind == MAP_AXIS) {
                fprintf(out, "    %-12s -> %s %s\n", m->label, m->code == ABS_X ? "ABS_X" : "ABS_Y",
                        m->direction < 0 ? "-" : "+");
            } else {
                fprintf(out, "    %-12s -> BTN_%s\n", m->label, button_name(m->code));
            }
        }
    }
}

static void print_usage(FILE *out, const char *prog) {
    fprintf(out, "usage: %s -d <keyboard-device> [--grab]\n", prog);
    fprintf(out, "\n");
    fprintf(out, "  Emulates %d virtual joysticks driven by one real keyboard.\n", JOYSTICK_COUNT);
    fprintf(out, "\n");
    fprintf(out, "  -d, --device <dev>: keyboard event node to read (e.g. /dev/input/event3,\n");
    fprintf(out, "                      see /proc/bus/input/devices or run input_dump)\n");
    fprintf(out, "  -g, --grab: grab the keyboard exclusively (EVIOCGRAB), so its keys stop\n");
    fprintf(out, "              reaching the desktop while this runs. Off by default.\n");
    fprintf(out, "  -h, --help: show this help and exit\n");
    fprintf(out, "\n");
    fprintf(out, "Key mapping:\n");
    print_mapping(out);
}

static int emit(int fd, int type, int code, int value) {
    struct input_event ie;

    memset(&ie, 0, sizeof(ie));
    ie.type = type;
    ie.code = code;
    ie.value = value;

    ssize_t written = write(fd, &ie, sizeof(ie));
    if (written != (ssize_t)sizeof(ie)) {
        perror("write to /dev/uinput");
        return -1;
    }
    return 0;
}

/* Creates one virtual joystick with the same axes/buttons the mapping
 * table can drive. Returns the /dev/uinput fd, or -1 on failure. */
static int create_joystick(int index) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/uinput");
        return -1;
    }

    static const int buttons[] = {BTN_A, BTN_B, BTN_X, BTN_Y};
    static const int axes[] = {ABS_X, ABS_Y};

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        perror("UI_SET_EVBIT");
        goto fail;
    }
    for (int i = 0; i < (int)(sizeof(buttons) / sizeof(buttons[0])); i++) {
        if (ioctl(fd, UI_SET_KEYBIT, buttons[i]) < 0) {
            perror("UI_SET_KEYBIT");
            goto fail;
        }
    }
    for (int i = 0; i < (int)(sizeof(axes) / sizeof(axes[0])); i++) {
        if (ioctl(fd, UI_SET_ABSBIT, axes[i]) < 0) {
            perror("UI_SET_ABSBIT");
            goto fail;
        }
        struct uinput_abs_setup abs_setup;
        memset(&abs_setup, 0, sizeof(abs_setup));
        abs_setup.code = axes[i];
        abs_setup.absinfo.minimum = -AXIS_MAX;
        abs_setup.absinfo.maximum = AXIS_MAX;
        abs_setup.absinfo.flat = AXIS_FLAT;
        if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) {
            perror("UI_ABS_SETUP");
            goto fail;
        }
    }

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor = 0x0000;
    usetup.id.product = 0x0002;
    usetup.id.version = 0x0100;
    snprintf(usetup.name, sizeof(usetup.name), "Keyboard Joystick %d", index + 1);

    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
        perror("UI_DEV_SETUP");
        goto fail;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        goto fail;
    }

    fprintf(stderr, "Created virtual device \"%s\"\n", usetup.name);
    return fd;

fail:
    close(fd);
    return -1;
}

static void destroy_joystick(int fd) {
    if (fd < 0) {
        return;
    }
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
}

/* Applies one key press/release to the joystick state, without emitting
 * anything yet (so all changes from one keyboard report can share a
 * single SYN_REPORT). */
static void apply_mapping(struct joystick *joys, const struct key_mapping *m, int pressed) {
    struct joystick *joy = &joys[m->joystick];

    if (m->kind == MAP_BUTTON) {
        if (emit(joy->fd, EV_KEY, m->code, pressed) == 0) {
            joy->dirty = 1;
        }
        return;
    }

    int axis = (m->code == ABS_X) ? AXIS_INDEX_X : AXIS_INDEX_Y;
    int slot = (m->direction < 0) ? 0 : 1;
    joy->axis_keys[axis][slot] = pressed;

    /* Holding both directions cancels out, matching how a real stick
     * cannot be pushed two ways at once. */
    int value = 0;
    if (joy->axis_keys[axis][1]) {
        value += AXIS_MAX;
    }
    if (joy->axis_keys[axis][0]) {
        value -= AXIS_MAX;
    }

    if (value == joy->axis_value[axis]) {
        return;
    }
    joy->axis_value[axis] = value;
    if (emit(joy->fd, EV_ABS, m->code, value) == 0) {
        joy->dirty = 1;
    }
}

static void sync_joysticks(struct joystick *joys) {
    for (int i = 0; i < JOYSTICK_COUNT; i++) {
        if (joys[i].dirty) {
            emit(joys[i].fd, EV_SYN, SYN_REPORT, 0);
            joys[i].dirty = 0;
        }
    }
}

int main(int argc, char **argv) {
    const char *dev_path = NULL;
    int grab = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--device") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", argv[i]);
                print_usage(stderr, argv[0]);
                return 1;
            }
            dev_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--grab") == 0) {
            grab = 1;
            continue;
        }
        fprintf(stderr, "unknown argument '%s'\n", argv[i]);
        print_usage(stderr, argv[0]);
        return 1;
    }

    if (dev_path == NULL) {
        fprintf(stderr, "a keyboard device is required (-d <keyboard-device>)\n");
        print_usage(stderr, argv[0]);
        return 1;
    }

    int kbd_fd = open(dev_path, O_RDONLY);
    if (kbd_fd < 0) {
        fprintf(stderr, "open %s: %s\n", dev_path, strerror(errno));
        return 1;
    }
    if (grab && ioctl(kbd_fd, EVIOCGRAB, 1) < 0) {
        fprintf(stderr, "EVIOCGRAB %s failed, continuing without exclusive grab: %s\n", dev_path,
                strerror(errno));
        grab = 0;
    }

    struct joystick joys[JOYSTICK_COUNT];
    memset(joys, 0, sizeof(joys));
    for (int i = 0; i < JOYSTICK_COUNT; i++) {
        joys[i].fd = -1;
    }

    int rc = 0;
    for (int i = 0; i < JOYSTICK_COUNT; i++) {
        joys[i].fd = create_joystick(i);
        if (joys[i].fd < 0) {
            rc = 1;
            goto cleanup;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stop_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "Reading %s%s, key mapping:\n", dev_path, grab ? " (grabbed)" : "");
    print_mapping(stderr);

    while (!stop_requested) {
        struct pollfd pfd = {.fd = kbd_fd, .events = POLLIN};
        /* Time out regularly so a signal that arrives between the check
         * above and the poll() still stops us promptly. */
        int ready = poll(&pfd, 1, 500);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            rc = 1;
            break;
        }
        if (ready == 0) {
            continue;
        }

        struct input_event ev;
        ssize_t got = read(kbd_fd, &ev, sizeof(ev));
        if (got == 0) {
            fprintf(stderr, "%s closed\n", dev_path);
            break;
        }
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            if (errno == ENODEV) {
                /* Keyboard unplugged (or its virtual device destroyed). */
                fprintf(stderr, "%s went away\n", dev_path);
                break;
            }
            fprintf(stderr, "read %s: %s\n", dev_path, strerror(errno));
            rc = 1;
            break;
        }
        if (got != (ssize_t)sizeof(ev)) {
            fprintf(stderr, "short read from %s (%zd bytes)\n", dev_path, got);
            rc = 1;
            break;
        }

        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            sync_joysticks(joys);
            continue;
        }
        /* value 2 is auto-repeat: the key is already held, nothing changes. */
        if (ev.type != EV_KEY || ev.value == 2) {
            continue;
        }

        for (int i = 0; i < KEY_MAPPING_COUNT; i++) {
            if (key_mappings[i].key == ev.code) {
                apply_mapping(joys, &key_mappings[i], ev.value ? 1 : 0);
            }
        }
    }

    sync_joysticks(joys);

cleanup:
    for (int i = 0; i < JOYSTICK_COUNT; i++) {
        destroy_joystick(joys[i].fd);
    }
    if (grab) {
        ioctl(kbd_fd, EVIOCGRAB, 0);
    }
    close(kbd_fd);

    return rc;
}
