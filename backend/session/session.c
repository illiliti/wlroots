#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend/session.h>
#include <wlr/config.h>
#include <wlr/util/log.h>
#include <xf86drmMode.h>
#include "backend/session/dev.h"
#include "backend/session/session.h"
#include "util/signal.h"

#include <libseat.h>

static void handle_enable_seat(struct libseat *seat, void *data) {
	struct wlr_session *session = data;
	session->active = true;
	wlr_signal_emit_safe(&session->events.active, NULL);
}

static void handle_disable_seat(struct libseat *seat, void *data) {
	struct wlr_session *session = data;
	session->active = false;
	wlr_signal_emit_safe(&session->events.active, NULL);
	libseat_disable_seat(session->seat_handle);
}

static int libseat_event(int fd, uint32_t mask, void *data) {
	struct wlr_session *session = data;
	if (libseat_dispatch(session->seat_handle, 0) == -1) {
		wlr_log_errno(WLR_ERROR, "Failed to dispatch libseat");
		wl_display_terminate(session->display);
	}
	return 1;
}

static struct libseat_seat_listener seat_listener = {
	.enable_seat = handle_enable_seat,
	.disable_seat = handle_disable_seat,
};

static enum wlr_log_importance libseat_log_level_to_wlr(
		enum libseat_log_level level) {
	switch (level) {
	case LIBSEAT_LOG_LEVEL_ERROR:
		return WLR_ERROR;
	case LIBSEAT_LOG_LEVEL_INFO:
		return WLR_INFO;
	default:
		return WLR_DEBUG;
	}
}

static void log_libseat(enum libseat_log_level level,
		const char *fmt, va_list args) {
	enum wlr_log_importance importance = libseat_log_level_to_wlr(level);

	static char wlr_fmt[1024];
	snprintf(wlr_fmt, sizeof(wlr_fmt), "[libseat] %s", fmt);

	_wlr_vlog(importance, wlr_fmt, args);
}

static int libseat_session_init(struct wlr_session *session, struct wl_display *disp) {
	libseat_set_log_handler(log_libseat);
	libseat_set_log_level(LIBSEAT_LOG_LEVEL_INFO);

	// libseat will take care of updating the logind state if necessary
	setenv("XDG_SESSION_TYPE", "wayland", 1);

	session->seat_handle = libseat_open_seat(&seat_listener, session);
	if (session->seat_handle == NULL) {
		wlr_log_errno(WLR_ERROR, "Unable to create seat");
		return -1;
	}

	const char *seat_name = libseat_seat_name(session->seat_handle);
	if (seat_name == NULL) {
		wlr_log_errno(WLR_ERROR, "Unable to get seat info");
		goto error;
	}
	snprintf(session->seat, sizeof(session->seat), "%s", seat_name);

	struct wl_event_loop *event_loop = wl_display_get_event_loop(disp);
	session->libseat_event = wl_event_loop_add_fd(event_loop, libseat_get_fd(session->seat_handle),
		WL_EVENT_READABLE, libseat_event, session);
	if (session->libseat_event == NULL) {
		wlr_log(WLR_ERROR, "Failed to create libseat event source");
		goto error;
	}

	// We may have received enable_seat immediately after the open_seat result,
	// so, dispatch once without timeout to speed up activation.
	if (libseat_dispatch(session->seat_handle, 0) == -1) {
		wlr_log_errno(WLR_ERROR, "libseat dispatch failed");
		goto error_dispatch;
	}

	wlr_log(WLR_INFO, "Successfully loaded libseat session");
	return 0;

error_dispatch:
	wl_event_source_remove(session->libseat_event);
	session->libseat_event = NULL;
error:
	libseat_close_seat(session->seat_handle);
	session->seat_handle = NULL;
	return -1;
}

static void libseat_session_finish(struct wlr_session *session) {
	libseat_close_seat(session->seat_handle);
	wl_event_source_remove(session->libseat_event);
	session->seat_handle = NULL;
	session->libseat_event = NULL;
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_session *session =
		wl_container_of(listener, session, display_destroy);
	wlr_session_destroy(session);
}

struct wlr_session *wlr_session_create(struct wl_display *disp) {
	struct wlr_session *session = calloc(1, sizeof(*session));
	if (!session) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	wl_signal_init(&session->events.active);
	wl_signal_init(&session->events.add_drm_card);
	wl_signal_init(&session->events.destroy);
	wl_list_init(&session->devices);

	if (libseat_session_init(session, disp) == -1) {
		wlr_log(WLR_ERROR, "Failed to load session backend");
		goto error_open;
	}

	session->dev_handle = dev_create();
	if (!session->dev_handle) {
		wlr_log_errno(WLR_ERROR, "Failed to create dev context");
		goto error_session;
	}

	struct wl_event_loop *event_loop = wl_display_get_event_loop(disp);
	int fd = dev_get_fd(session->dev_handle);

	session->dev_event = wl_event_loop_add_fd(event_loop, fd,
		WL_EVENT_READABLE, dev_handle_event, session);
	if (!session->dev_event) {
		wlr_log_errno(WLR_ERROR, "Failed to create dev event source");
		goto error_dev;
	}

	session->display = disp;

	session->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(disp, &session->display_destroy);

	return session;

error_dev:
	dev_destroy(session->dev_handle);
error_session:
	libseat_session_finish(session);
error_open:
	free(session);
	return NULL;
}

void wlr_session_destroy(struct wlr_session *session) {
	if (!session) {
		return;
	}

	wlr_signal_emit_safe(&session->events.destroy, session);
	wl_list_remove(&session->display_destroy.link);

	wl_event_source_remove(session->dev_event);
	dev_destroy(session->dev_handle);

	struct wlr_device *dev, *tmp_dev;
	wl_list_for_each_safe(dev, tmp_dev, &session->devices, link) {
		wlr_session_close_file(session, dev);
	}

	libseat_session_finish(session);
	free(session);
}

struct wlr_device *wlr_session_open_file(struct wlr_session *session,
		const char *path) {
	int fd;
	int device_id = libseat_open_device(session->seat_handle, path, &fd);
	if (device_id == -1) {
		wlr_log_errno(WLR_ERROR, "Failed to open device: '%s'", path);
		return NULL;
	}

	struct wlr_device *dev = malloc(sizeof(*dev));
	if (!dev) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		goto error;
	}

	struct stat st;
	if (fstat(fd, &st) < 0) {
		wlr_log_errno(WLR_ERROR, "Stat failed");
		goto error;
	}

	dev->fd = fd;
	dev->dev = st.st_rdev;
	dev->device_id = device_id;
	wl_signal_init(&dev->events.change);
	wl_signal_init(&dev->events.remove);
	wl_list_insert(&session->devices, &dev->link);

	return dev;

error:
	libseat_close_device(session->seat_handle, device_id);
	free(dev);
	close(fd);
	return NULL;
}

void wlr_session_close_file(struct wlr_session *session,
		struct wlr_device *dev) {
	if (libseat_close_device(session->seat_handle, dev->device_id) == -1) {
		wlr_log_errno(WLR_ERROR, "Failed to close device %d", dev->device_id);
	}
	close(dev->fd);
	wl_list_remove(&dev->link);
	free(dev);
}

bool wlr_session_change_vt(struct wlr_session *session, unsigned vt) {
	if (!session) {
		return false;
	}
	return libseat_switch_session(session->seat_handle, vt) == 0;
}

/* Tests if 'path' is KMS compatible by trying to open it. Returns the opened
 * device on success. */
struct wlr_device *session_open_if_kms(struct wlr_session *restrict session,
		const char *restrict path) {
	if (!path) {
		return NULL;
	}

	struct wlr_device *dev = wlr_session_open_file(session, path);
	if (!dev) {
		return NULL;
	}

	if (!drmIsKMS(dev->fd)) {
		wlr_log(WLR_DEBUG, "Ignoring '%s': not a KMS device", path);
		wlr_session_close_file(session, dev);
		return NULL;
	}

	return dev;
}

static ssize_t explicit_find_gpus(struct wlr_session *session,
		size_t ret_len, struct wlr_device *ret[static ret_len], const char *str) {
	char *gpus = strdup(str);
	if (!gpus) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return -1;
	}

	size_t i = 0;
	char *save;
	char *ptr = strtok_r(gpus, ":", &save);
	do {
		if (i >= ret_len) {
			break;
		}

		ret[i] = session_open_if_kms(session, ptr);
		if (!ret[i]) {
			wlr_log(WLR_ERROR, "Unable to open %s as DRM device", ptr);
		} else {
			++i;
		}
	} while ((ptr = strtok_r(NULL, ":", &save)));

	free(gpus);
	return i;
}

/* Tries to find the primary GPU by checking for the "boot_vga" attribute.
 * If it's not found, it returns the first valid GPU it finds.
 */
ssize_t wlr_session_find_gpus(struct wlr_session *session,
		size_t ret_len, struct wlr_device **ret) {
	const char *explicit = getenv("WLR_DRM_DEVICES");
	if (explicit) {
		return explicit_find_gpus(session, ret_len, ret, explicit);
	}

#if WLR_HAS_UDEV || WLR_HAS_DEMI
	return dev_find_gpus(session, ret_len, ret);
#endif

	return -1;
}
