#include <libudev.h>

#ifndef BACKEND_SESSION_DEV_UDEV_H
#define BACKEND_SESSION_DEV_UDEV_H

struct dev {
	struct udev *udev_ctx;
	struct udev_monitor *udev_mon;
};

#endif
