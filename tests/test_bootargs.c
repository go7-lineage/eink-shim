// SPDX-License-Identifier: GPL-2.0-only
#include "epd_bootargs.h"
#include "test.h"

static int vcom(const char *args, u32 *uv)
{
	*uv = 12345;
	return epd_bootargs_vcom(args, uv);
}

TEST(bootargs_vcom_found)
{
	u32 uv;

	CHECK_EQ(vcom("fp9931:vcom=-1840000", &uv), 0);
	CHECK_EQ(uv, 1840000);
	CHECK_EQ(vcom("console=ttyMSM0 fp9931:vcom=-1840000 androidboot.x=1", &uv), 0);
	CHECK_EQ(uv, 1840000);
	CHECK_EQ(vcom("a fp9931:vcom=1500000", &uv), 0);
	CHECK_EQ(uv, 1500000);
	CHECK_EQ(vcom("fp9931:vcom=+0", &uv), 0);
	CHECK_EQ(uv, 0);
}

TEST(bootargs_vcom_missing)
{
	u32 uv;

	CHECK_EQ(vcom("", &uv), -ENOENT);
	CHECK_EQ(vcom("console=ttyMSM0", &uv), -ENOENT);
	CHECK_EQ(vcom("xfp9931:vcom=-1840000", &uv), -ENOENT);
	CHECK_EQ(vcom("fp9931:vcom", &uv), -ENOENT);
	CHECK_EQ(uv, 12345);
}

TEST(bootargs_vcom_takes_the_first_whole_key)
{
	u32 uv;

	CHECK_EQ(vcom("xfp9931:vcom=-1 fp9931:vcom=-2000000", &uv), 0);
	CHECK_EQ(uv, 2000000);
	CHECK_EQ(vcom("fp9931:vcom=-1000000 fp9931:vcom=-2000000", &uv), 0);
	CHECK_EQ(uv, 1000000);
}

TEST(bootargs_vcom_rejects_bad_numbers)
{
	u32 uv;

	CHECK_EQ(vcom("fp9931:vcom=", &uv), -EINVAL);
	CHECK_EQ(vcom("fp9931:vcom= 1", &uv), -EINVAL);
	CHECK_EQ(vcom("fp9931:vcom=-", &uv), -EINVAL);
	CHECK_EQ(vcom("fp9931:vcom=--1", &uv), -EINVAL);
	CHECK_EQ(vcom("fp9931:vcom=12ab", &uv), -EINVAL);
	CHECK_EQ(vcom("fp9931:vcom=1.5", &uv), -EINVAL);
	CHECK_EQ(vcom("fp9931:vcom=-5000001", &uv), -ERANGE);
	CHECK_EQ(vcom("fp9931:vcom=99999999999999999999", &uv), -ERANGE);
	CHECK_EQ(uv, 12345);
	CHECK_EQ(vcom("fp9931:vcom=-5000000", &uv), 0);
	CHECK_EQ(uv, 5000000);
}
