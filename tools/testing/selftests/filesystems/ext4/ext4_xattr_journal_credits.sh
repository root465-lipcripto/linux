#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

KSFT_SKIP=4

skip()
{
	printf 'SKIP: %s\n' "$1"
	exit "$KSFT_SKIP"
}

[ "$(id -u)" -eq 0 ] || skip "requires root to mount a disposable EXT4 image"

for command in mkfs.ext4 mount umount truncate e2fsck losetup grep; do
	command -v "$command" >/dev/null 2>&1 || skip "missing $command"
done

grep -qw ext4 /proc/filesystems || skip "EXT4 is not available in this kernel"
losetup -f >/dev/null 2>&1 || skip "no loop device is available"

test_dir=$(mktemp -d) || exit 1
mounted=0

cleanup()
{
	if [ "$mounted" -eq 1 ]; then
		umount "$test_dir/mnt" || true
	fi
	rm -rf "$test_dir"
}
trap cleanup EXIT INT TERM

truncate -s 32M "$test_dir/fs.img" || exit 1
# 128-byte inodes have no ibody xattr space, forcing the external-block path.
if ! mkfs.ext4 -q -F -b 1024 -I 128 -O ea_inode -J size=1 "$test_dir/fs.img" \
		2>"$test_dir/mkfs.err"; then
	cat "$test_dir/mkfs.err" >&2
	skip "mkfs.ext4 does not support the ea_inode test image"
fi
mkdir "$test_dir/mnt" || exit 1
if ! mount -t ext4 -o loop "$test_dir/fs.img" "$test_dir/mnt" \
		2>"$test_dir/mount.err"; then
	cat "$test_dir/mount.err" >&2
	if grep -Eqi 'permission denied|operation not permitted|not permitted' \
			"$test_dir/mount.err"; then
		skip "mounting a loop image is not permitted"
	fi
	exit 1
fi
mounted=1

test_bin="$(cd "$(dirname "$0")" && pwd)/ext4_xattr_journal_credits"
(cd "$test_dir/mnt" && "$test_bin") || exit $?

umount "$test_dir/mnt" || exit 1
mounted=0
e2fsck -fn "$test_dir/fs.img"
