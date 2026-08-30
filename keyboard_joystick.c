/* keyboard_joystick: emulate joysticks driven by one real keyboard.
 *
 * Usage:
 *   keyboard_joystick -d <keyboard-device> [--grab] [-c <map-file>]
 *                     [-m <joystick>:<key>:<target> ...] [--print-map]
 *
 * Reads key events from a keyboard's evdev node (e.g. /dev/input/event3,
 * see /proc/bus/input/devices or run input_dump) and translates them into
 * independent virtual joysticks created via /dev/uinput, so two players
 * can share one keyboard, or a single game that only speaks joystick can
 * be driven from the keyboard.
 *
 * The built-in default mapping drives two joysticks:
 *   Joystick 1: W/A/S/D axes, buttons in the T/F/G/H diamond:
 *               G (A / south), H (B / east), T (X / north), F (Y / west)
 *   Joystick 2: arrow keys axes, buttons in the Ins/Home/PgUp/ScLk/End
 *               cluster: End (A / south), PgUp (B / east),
 *               ScrollLock (X / north), Insert (Y / west), Home (START)
 *
 * It can be replaced entirely by a mapping file (-c) and/or individual
 * -m rules; see parse_mapping() for the syntax and --print-map to dump
 * the current mapping in that same syntax as a starting point. The
 * number of joysticks created follows the highest joystick number the
 * mapping mentions.
 *
 * --grab takes the keyboard exclusively (EVIOCGRAB), so the mapped keys
 * (and every other key on that keyboard) stop reaching the desktop while
 * this runs. Off by default, so keys keep working normally and only get
 * mirrored onto the joysticks.
 *
 * Needs read access to the keyboard's event node and to /dev/uinput
 * (i.e. root, or suitable udev permissions).
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_JOYSTICKS 8
#define MAX_MAPPINGS 256
#define MAX_LABEL 24

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
    int joystick;         /* 0-based index of the virtual joystick it drives */
    enum mapping_kind kind;
    int code;             /* ABS_* for MAP_AXIS, BTN_* for MAP_BUTTON */
    int direction;        /* MAP_AXIS only: -1 or +1 */
    char label[MAX_LABEL]; /* key name, for --help/--print-map/startup log */
};

/* Axis index within a joystick's state, so an axis' two keys can be
 * tracked independently and cancel each other out when held together. */
#define AXIS_INDEX_X 0
#define AXIS_INDEX_Y 1

static const struct key_mapping default_mappings[] = {
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
    {KEY_UP, 1, MAP_AXIS, ABS_Y, -1, "UP"},
    {KEY_DOWN, 1, MAP_AXIS, ABS_Y, +1, "DOWN"},
    {KEY_LEFT, 1, MAP_AXIS, ABS_X, -1, "LEFT"},
    {KEY_RIGHT, 1, MAP_AXIS, ABS_X, +1, "RIGHT"},
    {KEY_END, 1, MAP_BUTTON, BTN_A, 0, "END"},
    {KEY_PAGEUP, 1, MAP_BUTTON, BTN_B, 0, "PAGEUP"},
    {KEY_SCROLLLOCK, 1, MAP_BUTTON, BTN_X, 0, "SCROLLLOCK"},
    {KEY_INSERT, 1, MAP_BUTTON, BTN_Y, 0, "INSERT"},
    {KEY_HOME, 1, MAP_BUTTON, BTN_START, 0, "HOME"},
};

#define DEFAULT_MAPPING_COUNT ((int)(sizeof(default_mappings) / sizeof(default_mappings[0])))

/* Effective mapping: the defaults until a -c file or a -m rule replaces
 * them. */
static struct key_mapping mappings[MAX_MAPPINGS];
static int mapping_count;
static int joystick_count;

struct name_code {
    const char *name;
    int code;
};

/* Key names accepted in mapping rules. A leading "KEY_" is optional and
 * matching is case-insensitive, so both "w" and "KEY_W" work. */
static const struct name_code key_names[] = {
    {"A", KEY_A}, {"B", KEY_B}, {"C", KEY_C}, {"D", KEY_D}, {"E", KEY_E},
    {"F", KEY_F}, {"G", KEY_G}, {"H", KEY_H}, {"I", KEY_I}, {"J", KEY_J},
    {"K", KEY_K}, {"L", KEY_L}, {"M", KEY_M}, {"N", KEY_N}, {"O", KEY_O},
    {"P", KEY_P}, {"Q", KEY_Q}, {"R", KEY_R}, {"S", KEY_S}, {"T", KEY_T},
    {"U", KEY_U}, {"V", KEY_V}, {"W", KEY_W}, {"X", KEY_X}, {"Y", KEY_Y},
    {"Z", KEY_Z},

    {"0", KEY_0}, {"1", KEY_1}, {"2", KEY_2}, {"3", KEY_3}, {"4", KEY_4},
    {"5", KEY_5}, {"6", KEY_6}, {"7", KEY_7}, {"8", KEY_8}, {"9", KEY_9},

    {"F1", KEY_F1}, {"F2", KEY_F2}, {"F3", KEY_F3}, {"F4", KEY_F4},
    {"F5", KEY_F5}, {"F6", KEY_F6}, {"F7", KEY_F7}, {"F8", KEY_F8},
    {"F9", KEY_F9}, {"F10", KEY_F10}, {"F11", KEY_F11}, {"F12", KEY_F12},

    {"UP", KEY_UP}, {"DOWN", KEY_DOWN}, {"LEFT", KEY_LEFT}, {"RIGHT", KEY_RIGHT},

    {"ESC", KEY_ESC}, {"TAB", KEY_TAB}, {"ENTER", KEY_ENTER},
    {"SPACE", KEY_SPACE}, {"BACKSPACE", KEY_BACKSPACE}, {"CAPSLOCK", KEY_CAPSLOCK},
    {"LEFTSHIFT", KEY_LEFTSHIFT}, {"RIGHTSHIFT", KEY_RIGHTSHIFT},
    {"LEFTCTRL", KEY_LEFTCTRL}, {"RIGHTCTRL", KEY_RIGHTCTRL},
    {"LEFTALT", KEY_LEFTALT}, {"RIGHTALT", KEY_RIGHTALT},
    {"LEFTMETA", KEY_LEFTMETA}, {"RIGHTMETA", KEY_RIGHTMETA},

    {"MINUS", KEY_MINUS}, {"EQUAL", KEY_EQUAL}, {"LEFTBRACE", KEY_LEFTBRACE},
    {"RIGHTBRACE", KEY_RIGHTBRACE}, {"BACKSLASH", KEY_BACKSLASH},
    {"SEMICOLON", KEY_SEMICOLON}, {"APOSTROPHE", KEY_APOSTROPHE},
    {"GRAVE", KEY_GRAVE}, {"COMMA", KEY_COMMA}, {"DOT", KEY_DOT},
    {"SLASH", KEY_SLASH},

    {"SCROLLLOCK", KEY_SCROLLLOCK}, {"SYSRQ", KEY_SYSRQ}, {"PAUSE", KEY_PAUSE},

    {"INSERT", KEY_INSERT}, {"DELETE", KEY_DELETE}, {"HOME", KEY_HOME},
    {"END", KEY_END}, {"PAGEUP", KEY_PAGEUP}, {"PAGEDOWN", KEY_PAGEDOWN},

    {"KP0", KEY_KP0}, {"KP1", KEY_KP1}, {"KP2", KEY_KP2}, {"KP3", KEY_KP3},
    {"KP4", KEY_KP4}, {"KP5", KEY_KP5}, {"KP6", KEY_KP6}, {"KP7", KEY_KP7},
    {"KP8", KEY_KP8}, {"KP9", KEY_KP9}, {"KPDOT", KEY_KPDOT},
    {"KPPLUS", KEY_KPPLUS}, {"KPMINUS", KEY_KPMINUS},
    {"KPASTERISK", KEY_KPASTERISK}, {"KPSLASH", KEY_KPSLASH},
    {"KPENTER", KEY_KPENTER}, {"NUMLOCK", KEY_NUMLOCK},
};

#define KEY_NAME_COUNT ((int)(sizeof(key_names) / sizeof(key_names[0])))

/* Buttons a virtual joystick exposes, and the names accepted for them.
 * The whole gamepad set is created on every joystick so a mapping can
 * use any of them without the device having to be reconfigured. */
static const struct name_code button_names[] = {
    {"A", BTN_A}, {"B", BTN_B}, {"C", BTN_C},
    {"X", BTN_X}, {"Y", BTN_Y}, {"Z", BTN_Z},
    {"TL", BTN_TL}, {"TR", BTN_TR}, {"TL2", BTN_TL2}, {"TR2", BTN_TR2},
    {"SELECT", BTN_SELECT}, {"START", BTN_START}, {"MODE", BTN_MODE},
    {"THUMBL", BTN_THUMBL}, {"THUMBR", BTN_THUMBR},
};

#define BUTTON_COUNT ((int)(sizeof(button_names) / sizeof(button_names[0])))

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

static int equals_ignore_case(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static int starts_with_ignore_case(const char *s, const char *prefix) {
    while (*prefix != '\0') {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*prefix)) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

static int lookup_name(const struct name_code *table, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (equals_ignore_case(table[i].name, name)) {
            return table[i].code;
        }
    }
    return -1;
}

static const char *button_name(int code) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (button_names[i].code == code) {
            return button_names[i].name;
        }
    }
    return "?";
}

/* Prints the mapping in the same syntax mapping files use, so
 * --print-map output can be saved and fed back with -c. */
static void print_mapping(FILE *out, const struct key_mapping *maps, int count, int joysticks,
                          const char *prefix) {
    for (int joy = 0; joy < joysticks; joy++) {
        fprintf(out, "%s# joystick %d\n", prefix, joy + 1);
        for (int i = 0; i < count; i++) {
            const struct key_mapping *m = &maps[i];
            if (m->joystick != joy) {
                continue;
            }
            const char *target;
            if (m->kind == MAP_AXIS) {
                if (m->code == ABS_X) {
                    target = (m->direction < 0) ? "LEFT" : "RIGHT";
                } else {
                    target = (m->direction < 0) ? "UP" : "DOWN";
                }
            } else {
                target = button_name(m->code);
            }
            fprintf(out, "%s%d %-12s %s\n", prefix, joy + 1, m->label, target);
        }
    }
}

static void print_usage(FILE *out, const char *prog) {
    fprintf(out, "usage: %s -d <keyboard-device> [--grab] [-c <map-file>]\n", prog);
    fprintf(out, "       %*s [-m <joystick>:<key>:<target> ...] [--print-map]\n",
            (int)strlen(prog), "");
    fprintf(out, "\n");
    fprintf(out, "  Emulates virtual joysticks driven by one real keyboard.\n");
    fprintf(out, "\n");
    fprintf(out, "  -d, --device <dev>: keyboard event node to read (e.g. /dev/input/event3,\n");
    fprintf(out, "                      see /proc/bus/input/devices or run input_dump)\n");
    fprintf(out, "  -g, --grab: grab the keyboard exclusively (EVIOCGRAB), so its keys stop\n");
    fprintf(out, "              reaching the desktop while this runs. Off by default.\n");
    fprintf(out, "  -c, --config <file>: load the key mapping from a file, replacing the\n");
    fprintf(out, "              built-in default mapping shown below.\n");
    fprintf(out, "  -m, --map <joystick>:<key>:<target>: add one mapping rule, also replacing\n");
    fprintf(out, "              the default mapping. May be given several times.\n");
    fprintf(out, "  --print-map: print the mapping that would be used and exit.\n");
    fprintf(out, "  -h, --help: show this help and exit\n");
    fprintf(out, "\n");
    fprintf(out, "Mapping syntax (one rule per line in a -c file, or one -m rule with ':'\n");
    fprintf(out, "instead of whitespace):\n");
    fprintf(out, "  <joystick> <key> <target>\n");
    fprintf(out, "    <joystick>: joystick number, 1 to %d. The highest number used decides\n",
            MAX_JOYSTICKS);
    fprintf(out, "                how many virtual joysticks are created.\n");
    fprintf(out, "    <key>:      key name, case-insensitive, with an optional KEY_ prefix\n");
    fprintf(out, "                (e.g. W, w, KEY_W, LEFTSHIFT, KP1, BACKSLASH, F1).\n");
    fprintf(out, "    <target>:   UP, DOWN, LEFT or RIGHT for an axis direction, or a button\n");
    fprintf(out, "                name (BTN_ prefix optional):");
    for (int i = 0; i < BUTTON_COUNT; i++) {
        fprintf(out, "%s%s", (i % 6 == 0) ? "\n                  " : " ", button_names[i].name);
    }
    fprintf(out, "\n");
    fprintf(out, "  Blank lines and lines starting with '#' are ignored.\n");
    fprintf(out, "  Several keys may drive the same target, and one key may drive several\n");
    fprintf(out, "  targets.\n");
    fprintf(out, "\n");
    fprintf(out, "Default mapping:\n");
    print_mapping(out, default_mappings, DEFAULT_MAPPING_COUNT, 2, "  ");
}

/* Parses one "<joystick> <key> <target>" rule into mappings[]. The three
 * fields are given already split. Returns 0 on success, -1 on error
 * (having logged what was wrong, prefixed by "where"). */
static int add_mapping(const char *joystick_field, const char *key_field, const char *target_field,
                       const char *where) {
    if (mapping_count >= MAX_MAPPINGS) {
        fprintf(stderr, "%s: too many mapping rules (max %d)\n", where, MAX_MAPPINGS);
        return -1;
    }

    char *end = NULL;
    long joystick = strtol(joystick_field, &end, 10);
    if (end == joystick_field || *end != '\0' || joystick < 1 || joystick > MAX_JOYSTICKS) {
        fprintf(stderr, "%s: invalid joystick number '%s' (expected 1 to %d)\n", where,
                joystick_field, MAX_JOYSTICKS);
        return -1;
    }

    const char *key_name = key_field;
    if (starts_with_ignore_case(key_name, "KEY_")) {
        key_name += 4;
    }
    int key = lookup_name(key_names, KEY_NAME_COUNT, key_name);
    if (key < 0) {
        fprintf(stderr, "%s: unknown key name '%s'\n", where, key_field);
        return -1;
    }

    const char *target = target_field;
    if (starts_with_ignore_case(target, "BTN_")) {
        target += 4;
    }

    struct key_mapping *m = &mappings[mapping_count];
    memset(m, 0, sizeof(*m));
    m->key = key;
    m->joystick = (int)joystick - 1;

    if (equals_ignore_case(target_field, "UP") || equals_ignore_case(target_field, "DOWN") ||
        equals_ignore_case(target_field, "LEFT") || equals_ignore_case(target_field, "RIGHT")) {
        m->kind = MAP_AXIS;
        m->code = (equals_ignore_case(target_field, "LEFT") ||
                   equals_ignore_case(target_field, "RIGHT"))
                      ? ABS_X
                      : ABS_Y;
        m->direction = (equals_ignore_case(target_field, "LEFT") ||
                        equals_ignore_case(target_field, "UP"))
                           ? -1
                           : +1;
    } else {
        int button = lookup_name(button_names, BUTTON_COUNT, target);
        if (button < 0) {
            fprintf(stderr,
                    "%s: unknown target '%s' (expected UP/DOWN/LEFT/RIGHT or a button name; "
                    "see --help)\n",
                    where, target_field);
            return -1;
        }
        m->kind = MAP_BUTTON;
        m->code = button;
    }

    snprintf(m->label, sizeof(m->label), "%s", key_name);
    for (char *p = m->label; *p != '\0'; p++) {
        *p = (char)toupper((unsigned char)*p);
    }

    mapping_count++;
    if (m->joystick + 1 > joystick_count) {
        joystick_count = m->joystick + 1;
    }
    return 0;
}

/* Splits a line into up to 3 whitespace-separated fields, ignoring a
 * '#' comment. Returns the number of fields found. The line is modified
 * in place. */
static int split_fields(char *line, char *fields[3]) {
    int count = 0;
    char *p = line;

    while (*p != '\0' && count < 3) {
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            break;
        }
        fields[count++] = p;
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }

    /* Anything after the third field (other than a comment) is an error
     * the caller detects by looking at the rest of the line. */
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '\0' && *p != '#') {
        return -1;
    }
    return count;
}

static int load_mapping_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }

    char line[256];
    int lineno = 0;
    int rc = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        char where[288];
        snprintf(where, sizeof(where), "%s:%d", path, lineno);

        char *fields[3];
        int count = split_fields(line, fields);
        if (count == 0) {
            continue;
        }
        if (count != 3) {
            fprintf(stderr, "%s: expected '<joystick> <key> <target>'\n", where);
            rc = -1;
            break;
        }
        if (add_mapping(fields[0], fields[1], fields[2], where) < 0) {
            rc = -1;
            break;
        }
    }

    fclose(f);
    return rc;
}

/* Parses a "<joystick>:<key>:<target>" command line rule. */
static int add_mapping_arg(const char *spec) {
    char buf[128];
    if (snprintf(buf, sizeof(buf), "%s", spec) >= (int)sizeof(buf)) {
        fprintf(stderr, "-m %s: rule too long\n", spec);
        return -1;
    }

    char *fields[3];
    int count = 0;
    char *p = buf;
    while (count < 3) {
        fields[count++] = p;
        char *sep = strchr(p, ':');
        if (sep == NULL) {
            break;
        }
        *sep = '\0';
        p = sep + 1;
    }
    if (count != 3 || strchr(p, ':') != NULL) {
        fprintf(stderr, "-m %s: expected '<joystick>:<key>:<target>'\n", spec);
        return -1;
    }

    char where[144];
    snprintf(where, sizeof(where), "-m %s", spec);
    return add_mapping(fields[0], fields[1], fields[2], where);
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

    static const int axes[] = {ABS_X, ABS_Y};

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        perror("UI_SET_EVBIT");
        goto fail;
    }
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (ioctl(fd, UI_SET_KEYBIT, button_names[i].code) < 0) {
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
    for (int i = 0; i < joystick_count; i++) {
        if (joys[i].dirty) {
            emit(joys[i].fd, EV_SYN, SYN_REPORT, 0);
            joys[i].dirty = 0;
        }
    }
}

static void use_default_mapping(void) {
    memcpy(mappings, default_mappings, sizeof(default_mappings));
    mapping_count = DEFAULT_MAPPING_COUNT;
    joystick_count = 2;
}

int main(int argc, char **argv) {
    const char *dev_path = NULL;
    int grab = 0;
    int print_map_only = 0;

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
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", argv[i]);
                print_usage(stderr, argv[0]);
                return 1;
            }
            if (load_mapping_file(argv[++i]) < 0) {
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--map") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", argv[i]);
                print_usage(stderr, argv[0]);
                return 1;
            }
            if (add_mapping_arg(argv[++i]) < 0) {
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--print-map") == 0) {
            print_map_only = 1;
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

    /* -c/-m replace the built-in mapping rather than extending it, so a
     * mapping file fully describes the layout it configures. */
    if (mapping_count == 0) {
        use_default_mapping();
    }

    if (print_map_only) {
        print_mapping(stdout, mappings, mapping_count, joystick_count, "");
        return 0;
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

    struct joystick joys[MAX_JOYSTICKS];
    memset(joys, 0, sizeof(joys));
    for (int i = 0; i < MAX_JOYSTICKS; i++) {
        joys[i].fd = -1;
    }

    int rc = 0;
    for (int i = 0; i < joystick_count; i++) {
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
    print_mapping(stderr, mappings, mapping_count, joystick_count, "  ");

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

        for (int i = 0; i < mapping_count; i++) {
            if (mappings[i].key == ev.code) {
                apply_mapping(joys, &mappings[i], ev.value ? 1 : 0);
            }
        }
    }

    sync_joysticks(joys);

cleanup:
    for (int i = 0; i < MAX_JOYSTICKS; i++) {
        destroy_joystick(joys[i].fd);
    }
    if (grab) {
        ioctl(kbd_fd, EVIOCGRAB, 0);
    }
    close(kbd_fd);

    return rc;
}
