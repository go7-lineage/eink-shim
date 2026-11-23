// SPDX-License-Identifier: GPL-2.0-only
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

static struct test *tests;
static int failures;

void test_register(struct test *t)
{
	struct test **pp = &tests;

	while (*pp && strcmp((*pp)->name, t->name) < 0)
		pp = &(*pp)->next;
	t->next = *pp;
	*pp = t;
}

void test_fail(const char *file, int line, const char *fmt, ...)
{
	va_list ap;

	failures++;
	if (failures > 20)
		return;
	(void)fprintf(stderr, "  %s:%d: ", file, line);
	va_start(ap, fmt);
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void)fputc('\n', stderr);
}

void test_check_mem(const char *file, int line, const char *what, const void *got, const void *want, size_t n)
{
	const unsigned char *g = got, *w = want;
	size_t i;

	for (i = 0; i < n; i++)
		if (g[i] != w[i]) {
			test_fail(file, line, "%s[%zu] = 0x%02x, want 0x%02x", what, i, g[i], w[i]);
			return;
		}
}

unsigned int test_hash(const void *data, size_t n)
{
	const unsigned char *d = data;
	unsigned int h = 2166136261u;
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= d[i];
		h *= 16777619u;
	}
	return h;
}

unsigned int test_rand(unsigned int *state)
{
	*state = *state * 1664525u + 1013904223u;
	return *state >> 8;
}

int main(int argc, char **argv)
{
	const char *filter = argc > 1 ? argv[1] : "";
	int run = 0, failed = 0;
	struct test *t;

	for (t = tests; t; t = t->next) {
		int before = failures;

		if (!strstr(t->name, filter))
			continue;
		t->fn();
		run++;
		if (failures != before) {
			failed++;
			(void)fprintf(stderr, "FAIL %s\n", t->name);
		}
	}
	printf("%d tests, %d failed\n", run, failed);
	return failed || !run;
}
