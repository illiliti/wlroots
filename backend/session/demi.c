#define _POSIX_C_SOURCE 200809L
#include <demi.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <wlr/backend/session.h>
#include <wlr/util/log.h>
#include "backend/session/dev.h"
#include "backend/session/dev_demi.h"
#include "backend/session/session.h"
#include "util/signal.h"

// TODO
// #define WAIT_GPU_TIMEOUT 10000 // ms

struct dev *dev_create(void) {
	struct dev *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	if (demi_init(&ctx->demi_ctx) == -1) {
		wlr_log_errno(WLR_ERROR, "Failed to initialize demi context");
		goto free_ctx;
	}

	if (demi_monitor_init(&ctx->demi_mon, &ctx->demi_ctx) == -1) {
		wlr_log_errno(WLR_ERROR, "Failed to initialize demi monitor");
		goto free_demi;
	}

	return ctx;

free_demi:
	demi_finish(&ctx->demi_ctx);
free_ctx:
	free(ctx);
	return NULL;
}

void dev_destroy(struct dev *ctx) {
	demi_monitor_finish(&ctx->demi_mon);
	demi_finish(&ctx->demi_ctx);
	free(ctx);
}

int dev_get_fd(struct dev *ctx) {
	return demi_monitor_get_fd(&ctx->demi_mon);
}

int dev_handle_event(int fd, uint32_t mask, void *data) {
	struct wlr_session *session = data;
	struct demi_monitor *demi_mon = &session->dev_handle->demi_mon;

	struct demi_device demi_dev;
	if (demi_monitor_recv_device(demi_mon, &demi_dev) == -1) {
		return 1;
	}

	enum demi_class class;
	if (demi_device_get_class(&demi_dev, &class) == -1) {
		goto out;
	}

	if (class != DEMI_CLASS_DRM) {
		goto out;
	}

	const char *devnode;
	if (demi_device_get_devnode(&demi_dev, &devnode) == -1) {
		goto out;
	}

	enum demi_action action;
	if (demi_device_get_action(&demi_dev, &action) == -1) {
		goto out;
	}

	wlr_log(WLR_DEBUG, "kernel event for %s (code %u)", devnode, action);

	// TODO
	// if (!is_drm_card(sysname) || !action || !devnode) {
	// 	goto out;
	// }

	const char *seat;
	if (demi_device_get_seat(&demi_dev, &seat) == -1) {
		seat = "seat0";
	}

	if (session->seat[0] != '\0' && strcmp(session->seat, seat) != 0) {
		goto out;
	}

	if (action == DEMI_ACTION_ATTACH) {
		wlr_log(WLR_DEBUG, "DRM device %s added", devnode);
		struct wlr_session_add_event event = {
			.path = devnode,
		};
		wlr_signal_emit_safe(&session->events.add_drm_card, &event);
	} else if (action == DEMI_ACTION_CHANGE || action == DEMI_ACTION_DETACH) {
		dev_t devnum;
		if (demi_device_get_devnum(&demi_dev, &devnum) == -1) {
			goto out;
		}

		struct wlr_device *dev;
		wl_list_for_each(dev, &session->devices, link) {
			if (dev->dev != devnum) {
				continue;
			}

			if (action == DEMI_ACTION_CHANGE) {
				wlr_log(WLR_DEBUG, "DRM device %s changed", devnode);
				// TODO
				// struct wlr_device_change_event event = {0};
				// read_udev_change_event(&event, udev_dev);
				// wlr_signal_emit_safe(&dev->events.change, &event);
				wlr_signal_emit_safe(&dev->events.change, NULL);
			} else if (action == DEMI_ACTION_DETACH) {
				wlr_log(WLR_DEBUG, "DRM device %s removed", devnode);
				wlr_signal_emit_safe(&dev->events.remove, NULL);
			} else {
				abort();
			}
			break;
		}
	}

out:
	demi_device_finish(&demi_dev);
	return 1;
}

struct find_gpus_ctx {
	struct wlr_session *session;
	struct wlr_device **devices;
	size_t max_len;
	size_t count;
};

static int find_gpus_callback(struct demi_device *dev, void *ptr) {
	struct find_gpus_ctx *ctx = ptr;

	enum demi_class class;
	if (demi_device_get_class(dev, &class) == -1) {
		goto out;
	}

	if (class != DEMI_CLASS_DRM) {
		goto out;
	}

	const char *devnode;
	if (demi_device_get_devnode(dev, &devnode) == -1) {
		goto out;
	}

	const char *seat;
	if (demi_device_get_seat(dev, &seat) == -1) {
		seat = "seat0";
	}

	if (ctx->session->seat[0] != '\0' && strcmp(ctx->session->seat, seat) != 0) {
		goto out;
	}

	bool is_boot_vga = false;

	uint32_t type;
	if (demi_device_get_type(dev, &type) == 0) {
		if (type & DEMI_TYPE_BOOT_VGA) {
			is_boot_vga = true;
		}
	}

	struct wlr_device *wlr_dev = session_open_if_kms(ctx->session, devnode);
	if (!wlr_dev) {
		goto out;
	}

	ctx->devices[ctx->count] = wlr_dev;
	if (is_boot_vga) {
		struct wlr_device *tmp = ctx->devices[0];
		ctx->devices[0] = ctx->devices[ctx->count];
		ctx->devices[ctx->count] = tmp;
	}

	++ctx->count;
	if (ctx->count == ctx->max_len) {
		return -1;
	}

out:
	demi_device_finish(dev);
	return 0;
}

ssize_t dev_find_gpus(struct wlr_session *session,
		size_t ret_len, struct wlr_device **ret) {
	struct demi_enumerate enu;
	if (demi_enumerate_init(&enu, &session->dev_handle->demi_ctx) == -1) {
		return -1;
	}

	struct find_gpus_ctx ctx = {
		.session = session,
		.devices = ret,
		.max_len = ret_len,
		.count = 0,
	};

	int retval = demi_enumerate_scan_system(&enu, find_gpus_callback, &ctx);
	demi_enumerate_finish(&enu);

	if (retval == -1 && ctx.count != ctx.max_len) {
		return -1;
	}

	return ctx.count;
}
