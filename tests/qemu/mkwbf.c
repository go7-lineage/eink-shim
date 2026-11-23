// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>

#include "fixture.h"

int main(int argc, char **argv)
{
	const struct wbf_blob *b = fixture_wbf();
	FILE *f;

	if (argc != 2)
		return 2;
	f = fopen(argv[1], "wb");
	if (!f || fwrite(b->data, 1, b->size, f) != b->size || fclose(f))
		return 1;
	return 0;
}
