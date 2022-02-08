#include <stdint.h>
#include <sys/types.h>
#include <wlr/backend/session.h>

#ifndef BACKEND_SESSION_DEV_H
#define BACKEND_SESSION_DEV_H

struct dev;

struct dev *dev_create(void);
void dev_destroy(struct dev *context);

int dev_handle_event(int fd, uint32_t mask, void *data);
ssize_t dev_find_gpus(struct wlr_session *session, size_t ret_len,
		struct wlr_device **ret);

int dev_get_fd(struct dev *context);

#endif
