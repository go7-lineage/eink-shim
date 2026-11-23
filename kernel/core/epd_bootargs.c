// SPDX-License-Identifier: GPL-2.0-only
#include "epd_bootargs.h"

#define VCOM_KEY	"fp9931:vcom="
#define VCOM_MAX_UV	5000000u

int epd_bootargs_vcom(const char *args, u32 *uv)
{
	const size_t klen = sizeof(VCOM_KEY) - 1;
	const char *s = args;
	u32 v = 0;
	int digits = 0;

	for (;;) {
		s = strstr(s, VCOM_KEY);
		if (!s)
			return -ENOENT;
		if (s == args || s[-1] == ' ')
			break;
		s += klen;
	}
	s += klen;
	if (*s == '-' || *s == '+')
		s++;
	for (; *s >= '0' && *s <= '9'; s++, digits++) {
		v = v * 10 + (u32)(*s - '0');
		if (v > VCOM_MAX_UV)
			return -ERANGE;
	}
	if (!digits || (*s && *s != ' '))
		return -EINVAL;
	*uv = v;
	return 0;
}
