#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <libudev.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend/session.h>
#include <wlr/util/log.h>
#include <xf86drm.h>
#include "backend/session/dev.h"
#include "backend/session/dev_udev.h"
#include "backend/session/session.h"
#include "util/signal.h"

#define WAIT_GPU_TIMEOUT 10000 // ms

struct dev *dev_create(void) {
	struct dev *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	ctx->udev_ctx = udev_new();
	if (!ctx->udev_ctx) {
		wlr_log_errno(WLR_ERROR, "Failed to create udev context");
		goto error_ctx;
	}

	ctx->udev_mon = udev_monitor_new_from_netlink(ctx->udev_ctx, "udev");
	if (!ctx->udev_mon) {
		wlr_log_errno(WLR_ERROR, "Failed to create udev monitor");
		goto error_udev;
	}

	assert(udev_monitor_filter_add_match_subsystem_devtype(ctx->udev_mon,
		"drm", NULL) == 0);
	assert(udev_monitor_enable_receiving(ctx->udev_mon) == 0);

	return ctx;

error_udev:
	udev_unref(ctx->udev_ctx);
error_ctx:
	free(ctx);
	return NULL;
}

void dev_destroy(struct dev *ctx) {
	udev_monitor_unref(ctx->udev_mon);
	udev_unref(ctx->udev_ctx);
	free(ctx);
}

static bool is_drm_card(const char *sysname) {
	const char prefix[] = DRM_PRIMARY_MINOR_NAME;
	if (strncmp(sysname, prefix, strlen(prefix)) != 0) {
		return false;
	}
	for (size_t i = strlen(prefix); sysname[i] != '\0'; i++) {
		if (sysname[i] < '0' || sysname[i] > '9') {
			return false;
		}
	}
	return true;
}

static void read_udev_change_event(struct wlr_device_change_event *event,
		struct udev_device *udev_dev) {
	const char *hotplug = udev_device_get_property_value(udev_dev, "HOTPLUG");
	if (hotplug != NULL && strcmp(hotplug, "1") == 0) {
		event->type = WLR_DEVICE_HOTPLUG;
		struct wlr_device_hotplug_event *hotplug = &event->hotplug;

		const char *connector =
			udev_device_get_property_value(udev_dev, "CONNECTOR");
		if (connector != NULL) {
			hotplug->connector_id = strtoul(connector, NULL, 10);
		}

		const char *prop =
			udev_device_get_property_value(udev_dev, "PROPERTY");
		if (prop != NULL) {
			hotplug->prop_id = strtoul(prop, NULL, 10);
		}

		return;
	}

	const char *lease = udev_device_get_property_value(udev_dev, "LEASE");
	if (lease != NULL && strcmp(lease, "1") == 0) {
		event->type = WLR_DEVICE_LEASE;
		return;
	}
}

int dev_handle_event(int fd, uint32_t mask, void *data) {
	struct wlr_session *session = data;

	struct udev_device *udev_dev = udev_monitor_receive_device(
		session->dev_handle->udev_mon);
	if (!udev_dev) {
		return 1;
	}

	const char *sysname = udev_device_get_sysname(udev_dev);
	const char *devnode = udev_device_get_devnode(udev_dev);
	const char *action = udev_device_get_action(udev_dev);
	wlr_log(WLR_DEBUG, "kernel event for %s (%s)", sysname, action);

	if (!is_drm_card(sysname) || !action || !devnode) {
		goto out;
	}

	const char *seat = udev_device_get_property_value(udev_dev, "ID_SEAT");
	if (!seat) {
		seat = "seat0";
	}
	if (session->seat[0] != '\0' && strcmp(session->seat, seat) != 0) {
		goto out;
	}

	if (strcmp(action, "add") == 0) {
		wlr_log(WLR_DEBUG, "DRM device %s added", sysname);
		struct wlr_session_add_event event = {
			.path = devnode,
		};
		wlr_signal_emit_safe(&session->events.add_drm_card, &event);
	} else if (strcmp(action, "change") == 0 || strcmp(action, "remove") == 0) {
		dev_t devnum = udev_device_get_devnum(udev_dev);
		struct wlr_device *dev;
		wl_list_for_each(dev, &session->devices, link) {
			if (dev->dev != devnum) {
				continue;
			}

			if (strcmp(action, "change") == 0) {
				wlr_log(WLR_DEBUG, "DRM device %s changed", sysname);
				struct wlr_device_change_event event = {0};
				read_udev_change_event(&event, udev_dev);
				wlr_signal_emit_safe(&dev->events.change, &event);
			} else if (strcmp(action, "remove") == 0) {
				wlr_log(WLR_DEBUG, "DRM device %s removed", sysname);
				wlr_signal_emit_safe(&dev->events.remove, NULL);
			} else {
				abort();
			}
			break;
		}
	}

out:
	udev_device_unref(udev_dev);
	return 1;
}

int dev_get_fd(struct dev *ctx) {
	return udev_monitor_get_fd(ctx->udev_mon);
}

static struct udev_enumerate *enumerate_drm_cards(struct udev *udev) {
	struct udev_enumerate *en = udev_enumerate_new(udev);
	if (!en) {
		wlr_log(WLR_ERROR, "udev_enumerate_new failed");
		return NULL;
	}

	assert(udev_enumerate_add_match_subsystem(en, "drm") == 0);
	assert(udev_enumerate_add_match_sysname(en, DRM_PRIMARY_MINOR_NAME "[0-9]*") == 0);

	if (udev_enumerate_scan_devices(en) != 0) {
		wlr_log(WLR_ERROR, "udev_enumerate_scan_devices failed");
		udev_enumerate_unref(en);
		return NULL;
	}

	return en;
}

static uint64_t get_current_time_ms(void) {
	struct timespec ts = {0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

struct find_gpus_add_handler {
	bool added;
	struct wl_listener listener;
};

static void find_gpus_handle_add(struct wl_listener *listener, void *data) {
	struct find_gpus_add_handler *handler =
		wl_container_of(listener, handler, listener);
	handler->added = true;
}

ssize_t dev_find_gpus(struct wlr_session *session,
		size_t ret_len, struct wlr_device **ret) {
	struct udev *udev = session->dev_handle->udev_ctx;

	struct udev_enumerate *en = enumerate_drm_cards(udev);
	if (!en) {
		return -1;
	}

	if (udev_enumerate_get_list_entry(en) == NULL) {
		udev_enumerate_unref(en);
		wlr_log(WLR_INFO, "Waiting for a DRM card device");

		struct find_gpus_add_handler handler = {0};
		handler.listener.notify = find_gpus_handle_add;
		wl_signal_add(&session->events.add_drm_card, &handler.listener);

		uint64_t started_at = get_current_time_ms();
		uint64_t timeout = WAIT_GPU_TIMEOUT;
		struct wl_event_loop *event_loop =
			wl_display_get_event_loop(session->display);
		while (!handler.added) {
			int ret = wl_event_loop_dispatch(event_loop, (int)timeout);
			if (ret < 0) {
				wlr_log_errno(WLR_ERROR, "Failed to wait for DRM card device: "
					"wl_event_loop_dispatch failed");
				udev_enumerate_unref(en);
				return -1;
			}

			uint64_t now = get_current_time_ms();
			if (now >= started_at + WAIT_GPU_TIMEOUT) {
				break;
			}
			timeout = started_at + WAIT_GPU_TIMEOUT - now;
		}

		wl_list_remove(&handler.listener.link);

		en = enumerate_drm_cards(udev);
		if (!en) {
			return -1;
		}
	}

	struct udev_list_entry *entry;
	size_t i = 0;

	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
		if (i == ret_len) {
			break;
		}

		bool is_boot_vga = false;

		const char *path = udev_list_entry_get_name(entry);
		struct udev_device *dev = udev_device_new_from_syspath(udev, path);
		if (!dev) {
			continue;
		}

		const char *seat = udev_device_get_property_value(dev, "ID_SEAT");
		if (!seat) {
			seat = "seat0";
		}
		if (session->seat[0] && strcmp(session->seat, seat) != 0) {
			udev_device_unref(dev);
			continue;
		}

		// This is owned by 'dev', so we don't need to free it
		struct udev_device *pci =
			udev_device_get_parent_with_subsystem_devtype(dev, "pci", NULL);

		if (pci) {
			const char *id = udev_device_get_sysattr_value(pci, "boot_vga");
			if (id && strcmp(id, "1") == 0) {
				is_boot_vga = true;
			}
		}

		struct wlr_device *wlr_dev =
			session_open_if_kms(session, udev_device_get_devnode(dev));
		if (!wlr_dev) {
			udev_device_unref(dev);
			continue;
		}

		udev_device_unref(dev);

		ret[i] = wlr_dev;
		if (is_boot_vga) {
			struct wlr_device *tmp = ret[0];
			ret[0] = ret[i];
			ret[i] = tmp;
		}

		++i;
	}

	udev_enumerate_unref(en);

	return i;
}
