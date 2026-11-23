/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TEST_H
#define TEST_H

#include <stddef.h>

struct test {
	const char *name;
	void (*fn)(void);
	struct test *next;
};

void test_register(struct test *t);
void test_fail(const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
unsigned int test_hash(const void *data, size_t n);
unsigned int test_rand(unsigned int *state);
void test_check_mem(const char *file, int line, const char *what, const void *got, const void *want, size_t n);

#define TEST(id)								\
	static void test_##id(void);						\
	static struct test test_entry_##id = { #id, test_##id, NULL };		\
	__attribute__((constructor)) static void test_add_##id(void)		\
	{									\
		test_register(&test_entry_##id);				\
	}									\
	static void test_##id(void)

#define CHECK(cond)								\
	do {									\
		if (!(cond))							\
			test_fail(__FILE__, __LINE__, "%s", #cond);		\
	} while (0)

#define CHECK_EQ(got, want)							\
	do {									\
		long long got_ = (long long)(got), want_ = (long long)(want);	\
										\
		if (got_ != want_)						\
			test_fail(__FILE__, __LINE__, "%s = %lld, want %lld", #got, got_, want_);	\
	} while (0)

#define CHECK_MEM(got, want, n)	test_check_mem(__FILE__, __LINE__, #got, got, want, n)

#define ARRAY_LEN(a)	(sizeof(a) / sizeof((a)[0]))

#endif
