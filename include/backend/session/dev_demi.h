#include <demi.h>

#ifndef BACKEND_SESSION_DEV_DEMI_H
#define BACKEND_SESSION_DEV_DEMI_H

struct dev {
	struct demi demi_ctx;
	struct demi_monitor demi_mon;
};

#endif
