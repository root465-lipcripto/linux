// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>

#include "../../kselftest_harness.h"

/* Near XATTR_SIZE_MAX to exercise the largest ea_inode credit estimate. */
#define LARGE_VALUE_SIZE 65296
#define NAMED_ACL_ENTRIES 2500

static const char large_name[] = "user.ext4-credit-large";
static const char small_name[] = "user.ext4-credit-small";

FIXTURE(ext4_xattr_journal_credits)
{
	char path[PATH_MAX];
	char acl_child[PATH_MAX];
	char shared_a[PATH_MAX];
	char shared_b[PATH_MAX];
	char *value;
	char *readback;
};

FIXTURE_SETUP(ext4_xattr_journal_credits)
{
	int len;

	len = snprintf(self->path, sizeof(self->path), "credit-test-%d",
		       getpid());
	ASSERT_GE(len, 0);
	ASSERT_LT(len, (int)sizeof(self->path));
	len = snprintf(self->acl_child, sizeof(self->acl_child), "%s/child",
		       self->path);
	ASSERT_GE(len, 0);
	ASSERT_LT(len, (int)sizeof(self->acl_child));
	len = snprintf(self->shared_a, sizeof(self->shared_a), "shared-a-%d",
		       getpid());
	ASSERT_GE(len, 0);
	ASSERT_LT(len, (int)sizeof(self->shared_a));
	len = snprintf(self->shared_b, sizeof(self->shared_b), "shared-b-%d",
		       getpid());
	ASSERT_GE(len, 0);
	ASSERT_LT(len, (int)sizeof(self->shared_b));

	self->value = malloc(LARGE_VALUE_SIZE);
	ASSERT_NE(NULL, self->value);
	self->readback = malloc(LARGE_VALUE_SIZE);
	ASSERT_NE(NULL, self->readback);
}

FIXTURE_TEARDOWN(ext4_xattr_journal_credits)
{
	unlink(self->path);
	unlink(self->shared_a);
	unlink(self->shared_b);
	rmdir(self->acl_child);
	rmdir(self->path);
	rmdir(self->shared_a);
	rmdir(self->shared_b);
	free(self->readback);
	free(self->value);
}

static void set_acl_entry(struct posix_acl_xattr_entry *entry, uint16_t tag,
			  uint16_t perm, uint32_t id)
{
	entry->e_tag = htole16(tag);
	entry->e_perm = htole16(perm);
	entry->e_id = htole32(id);
}

static void *make_large_default_acl(size_t *size)
{
	struct posix_acl_xattr_header *header;
	struct posix_acl_xattr_entry *entry;
	size_t entry_count = NAMED_ACL_ENTRIES * 2 + 4;
	unsigned int i;

	*size = sizeof(*header) + entry_count * sizeof(*entry);
	header = calloc(1, *size);
	if (!header)
		return NULL;
	header->a_version = htole32(POSIX_ACL_XATTR_VERSION);
	entry = (void *)(header + 1);
	set_acl_entry(entry++, ACL_USER_OBJ, 7, UINT32_MAX);
	for (i = 0; i < NAMED_ACL_ENTRIES; i++)
		set_acl_entry(entry++, ACL_USER, 4, i + 1);
	set_acl_entry(entry++, ACL_GROUP_OBJ, 5, UINT32_MAX);
	for (i = 0; i < NAMED_ACL_ENTRIES; i++)
		set_acl_entry(entry++, ACL_GROUP, 4, i + 1);
	set_acl_entry(entry++, ACL_MASK, 5, UINT32_MAX);
	set_acl_entry(entry, ACL_OTHER, 0, UINT32_MAX);
	return header;
}

static void create_test_file(struct __test_metadata *_metadata,
			     const char *path)
{
	int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);

	ASSERT_GE(fd, 0) {
		TH_LOG("open(%s): %s", path, strerror(errno));
	}
	ASSERT_EQ(0, close(fd));
}

static void check_xattr_value(struct __test_metadata *_metadata,
			      const char *path,
			      const char *name, const void *expected,
			      size_t size, char *buffer)
{
	ssize_t len = getxattr(path, name, buffer, size);

	ASSERT_EQ((ssize_t)size, len) {
		TH_LOG("getxattr(%s): %s", name,
		       len < 0 ? strerror(errno) : "unexpected value size");
	}
	ASSERT_EQ(0, memcmp(buffer, expected, size));
}

TEST_F(ext4_xattr_journal_credits, large_xattr_lifecycle)
{
	char *expected = malloc(LARGE_VALUE_SIZE);
	int ret;

	ASSERT_NE(NULL, expected);
	create_test_file(_metadata, self->path);
	memset(self->value, 0x5a, LARGE_VALUE_SIZE);
	self->value[0] = 1;

	ASSERT_EQ(0, setxattr(self->path, large_name, self->value,
			      LARGE_VALUE_SIZE, 0));
	check_xattr_value(_metadata, self->path, large_name, self->value,
			  LARGE_VALUE_SIZE, self->readback);

	memset(self->value, 0xa5, LARGE_VALUE_SIZE);
	self->value[0] = 2;
	ASSERT_EQ(0, setxattr(self->path, large_name, self->value,
			      LARGE_VALUE_SIZE, XATTR_REPLACE));
	check_xattr_value(_metadata, self->path, large_name, self->value,
			  LARGE_VALUE_SIZE, self->readback);

	memcpy(expected, self->value, LARGE_VALUE_SIZE);
	memset(self->value, 0x3c, LARGE_VALUE_SIZE);
	self->value[0] = 3;
	errno = 0;
	ret = setxattr(self->path, large_name, self->value,
		       LARGE_VALUE_SIZE, XATTR_CREATE);
	ASSERT_EQ(-1, ret);
	ASSERT_EQ(EEXIST, errno);
	check_xattr_value(_metadata, self->path, large_name, expected,
			  LARGE_VALUE_SIZE, self->readback);

	ASSERT_EQ(0, removexattr(self->path, large_name));
	free(expected);
}

TEST_F(ext4_xattr_journal_credits, shared_block_reuses_same_ea_inode)
{
	/* The test image has 128-byte inodes, so both xattrs use external blocks. */
	create_test_file(_metadata, self->shared_a);
	create_test_file(_metadata, self->shared_b);
	memset(self->value, 0x7e, LARGE_VALUE_SIZE);
	self->value[0] = 0x42;

	ASSERT_EQ(0, setxattr(self->shared_a, large_name, self->value,
			      LARGE_VALUE_SIZE, XATTR_CREATE));
	ASSERT_EQ(0, setxattr(self->shared_b, large_name, self->value,
			      LARGE_VALUE_SIZE, XATTR_CREATE));
	check_xattr_value(_metadata, self->shared_a, large_name, self->value,
			  LARGE_VALUE_SIZE, self->readback);
	check_xattr_value(_metadata, self->shared_b, large_name, self->value,
			  LARGE_VALUE_SIZE, self->readback);

	/* Clone the shared block, then deduplicate back to the original block. */
	ASSERT_EQ(0, setxattr(self->shared_a, large_name, self->value,
			      LARGE_VALUE_SIZE, XATTR_REPLACE));
	check_xattr_value(_metadata, self->shared_a, large_name, self->value,
			  LARGE_VALUE_SIZE, self->readback);
	check_xattr_value(_metadata, self->shared_b, large_name, self->value,
			  LARGE_VALUE_SIZE, self->readback);

	ASSERT_EQ(0, removexattr(self->shared_a, large_name));
	ASSERT_EQ(0, removexattr(self->shared_b, large_name));
}

TEST_F(ext4_xattr_journal_credits, small_xattr_round_trip)
{
	static const char value[] = "small-xattr";
	char readback[sizeof(value)];

	create_test_file(_metadata, self->path);
	ASSERT_EQ(0, setxattr(self->path, small_name, value, sizeof(value), 0));
	ASSERT_EQ((ssize_t)sizeof(value),
		  getxattr(self->path, small_name, readback, sizeof(readback)));
	ASSERT_EQ(0, memcmp(value, readback, sizeof(value)));
	ASSERT_EQ(0, removexattr(self->path, small_name));
}

TEST_F(ext4_xattr_journal_credits, large_default_acl_respects_journal_limit)
{
	static const char small_value[] = "filesystem-still-writable";
	size_t acl_size;
	char *acl = make_large_default_acl(&acl_size);
	int ret;

	ASSERT_NE(NULL, acl);
	ASSERT_EQ(0, mkdir(self->path, 0700));
	ret = setxattr(self->path, "system.posix_acl_default", acl,
		       acl_size, 0);
	if (ret < 0 && errno == EOPNOTSUPP)
		SKIP(return, "POSIX ACLs are not supported");
	ASSERT_EQ(0, ret) {
		TH_LOG("set default ACL: %s", strerror(errno));
	}

	errno = 0;
	ret = mkdir(self->acl_child, 0700);
	ASSERT_EQ(-1, ret);
	ASSERT_EQ(ENOSPC, errno);

	/* The rejected transaction must not abort the journal. */
	ASSERT_EQ(0, setxattr(self->path, small_name, small_value,
			      sizeof(small_value), 0));
	ASSERT_EQ(0, removexattr(self->path, small_name));
	free(acl);
}

TEST_HARNESS_MAIN
