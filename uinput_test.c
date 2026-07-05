#include <linux/uinput.h>
#include <linux/serio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

void emit(int fd, int type, int code, int val) {
    struct input_event ie;

    ie.type = type;
    ie.code = code;
    ie.value = val;
    ie.time.tv_sec = 0;
    ie.time.tv_usec = 0;

    write(fd, &ie, sizeof(ie));
}

int main(void) {
    struct uinput_setup usetup;
    int i = 50;

    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    //ioctl(fd, UI_SET_EVBIT, EV_ABS);

    ioctl(fd, UI_SET_KEYBIT, BTN_A);
    ioctl(fd, UI_SET_KEYBIT, BTN_B);
    //ioctl(fd, UI_SET_KEYBIT, BTN_C);
    //ioctl(fd, UI_SET_KEYBIT, BTN_X);
    //ioctl(fd, UI_SET_KEYBIT, BTN_Y);
    //ioctl(fd, UI_SET_KEYBIT, BTN_Z);
    //ioctl(fd, UI_SET_KEYBIT, BTN_TL);
    //ioctl(fd, UI_SET_KEYBIT, BTN_TR);
    //ioctl(fd, UI_SET_KEYBIT, BTN_START);
    //ioctl(fd, UI_SET_KEYBIT, BTN_SELECT);

    //ioctl(fd, UI_SET_ABSBIT, ABS_X);
    //ioctl(fd, UI_SET_ABSBIT, ABS_Y);

    struct uinput_abs_setup abs_x{
        .code = ABS_X,
        .absinfo = {
            .minimum = -512,
            .maximum = 511,
            .flat = 4,
        },
    };
    //ioctl(fd, UI_ABS_SETUP, &abs_x);
    struct uinput_abs_setup abs_y{
        .code = ABS_Y,
        .absinfo = {
            .minimum = -512,
            .maximum = 511,
            .flat = 4,
        },
    };
    //ioctl(fd, UI_ABS_SETUP, &abs_y);

    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x0000;
    usetup.id.product = 0x0001;
    usetup.id.version = 0x0100;
    strcpy(usetup.name, "Emulated Joystick");

    ioctl(fd, UI_DEV_SETUP, &usetup);
    ioctl(fd, UI_DEV_CREATE);

    sleep(1);

    int x = 0, y = 500;
    while (true) {
        //emit(fd, EV_ABS, ABS_X, x);
        //emit(fd, EV_ABS, ABS_Y, y);
        //emit(fd, EV_SYN, SYN_REPORT, 0);
        printf("sleep\n");
        sleep(1);
        y = -y;
    }

    sleep(1);
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);

    return 0;
}

