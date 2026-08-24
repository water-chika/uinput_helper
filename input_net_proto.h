#ifndef INPUT_NET_PROTO_H
#define INPUT_NET_PROTO_H

#include <errno.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

/* Simple network protocol to transfer a Linux input device (evdev) between
 * a server (which owns the real device) and a client (which recreates it
 * with uinput).
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

#endif /* INPUT_NET_PROTO_H */
