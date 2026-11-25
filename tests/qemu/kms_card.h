/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KMS_CARD_H
#define KMS_CARD_H

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/drm.h>

static inline int open_card(const char *want)
{
	char path[32], name[32];
	int i, fd;

	for (i = 0; i < 8; i++) {
		struct drm_version v = { .name = name, .name_len = sizeof(name) - 1 };

		(void)snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(name, 0, sizeof(name));
		if (!ioctl(fd, DRM_IOCTL_VERSION, &v) && !strcmp(name, want))
			return fd;
		close(fd);
	}
	return -1;
}

static inline int fail(const char *what)
{
	perror(what);
	return 1;
}

#endif
