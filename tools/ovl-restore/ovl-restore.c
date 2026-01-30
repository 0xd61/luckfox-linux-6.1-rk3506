/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ovl-restore - Restore visibility of overlayfs lower layer files
 *
 * Copyright (C) 2025 Calculinux Project
 * Author: Ben Klop
 *
 * This tool uses the OVL_IOC_RESTORE_LOWER ioctl to remove whiteout
 * files from an overlayfs upper layer, making the corresponding files
 * in the lower layer visible again.
 *
 * Usage:
 *   ovl-restore <overlay-mount-point> <path>...
 *
 * Example:
 *   # Restore a single file
 *   ovl-restore / /usr/bin/foo
 *
 *   # Restore multiple files
 *   ovl-restore / /usr/bin/foo /usr/lib/libbar.so
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>

/*
 * Overlayfs ioctl definitions
 * (extracted from linux/overlayfs.h to avoid kernel header dependencies)
 */
#ifndef __aligned_u64
#define __aligned_u64 __attribute__((aligned(8))) uint64_t
#endif

struct ovl_restore_lower_args {
	__aligned_u64 path_ptr;		/* Pointer to path string */
	uint32_t path_len;		/* Length of path string */
	uint32_t flags;			/* Reserved for future use, must be 0 */
};

struct ovl_is_restorable_args {
	__aligned_u64 path_ptr;		/* Pointer to path string */
	uint32_t path_len;		/* Length of path string */
	uint32_t flags;			/* Reserved for future use, must be 0 */
};

#define OVL_IOC_RESTORE_LOWER _IOW('O', 1, struct ovl_restore_lower_args)
#define OVL_IOC_IS_RESTORABLE _IOW('O', 2, struct ovl_is_restorable_args)

static void usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [OPTIONS] <overlay-mount-point> <path>...\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "Restore visibility of overlayfs lower layer files.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  --test, -t       Test if files are restorable without restoring them\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Arguments:\n");
	fprintf(stderr, "  overlay-mount-point  Path to the overlay filesystem mount\n");
	fprintf(stderr, "  path                 One or more paths to restore (absolute paths)\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Example:\n");
	fprintf(stderr, "  %s / /usr/bin/foo /usr/lib/libbar.so\n", progname);
	fprintf(stderr, "  %s --test / /usr/bin/foo\n", progname);
	exit(1);
}

static int restore_lower(int fd, const char *path)
{
	struct ovl_restore_lower_args args;
	int ret;

	args.path_ptr = (uint64_t)(uintptr_t)path;
	args.path_len = strlen(path);  /* String length, not including null terminator */
	args.flags = 0;

	ret = ioctl(fd, OVL_IOC_RESTORE_LOWER, &args);
	if (ret < 0) {
		perror("OVL_IOC_RESTORE_LOWER");
		return -1;
	}

	return 0;
}

static int is_restorable(int fd, const char *path)
{
	struct ovl_is_restorable_args args;
	int ret;

	args.path_ptr = (uint64_t)(uintptr_t)path;
	args.path_len = strlen(path);  /* String length, not including null terminator */
	args.flags = 0;

	ret = ioctl(fd, OVL_IOC_IS_RESTORABLE, &args);
	/* Return 1 if restorable (ioctl succeeded), 0 if not (ioctl failed) */
	/* When returning 0, errno is set by the failed ioctl */
	return (ret >= 0) ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *mount_point;
	int fd, i, start_index;
	int errors = 0;
	int test_mode = 0;

	/* Parse options */
	start_index = 1;
	if (argc > 1 && (strcmp(argv[1], "--test") == 0 || strcmp(argv[1], "-t") == 0)) {
		test_mode = 1;
		start_index = 2;
	}

	if (argc < start_index + 2) {
		usage(argv[0]);
	}

	mount_point = argv[start_index];

	/* Open the overlay mount point */
	fd = open(mount_point, O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		fprintf(stderr, "Error: Cannot open mount point '%s': %s\n",
			mount_point, strerror(errno));
		return 1;
	}

	/* Process each path */
	for (i = start_index + 1; i < argc; i++) {
		const char *path = argv[i];

		if (test_mode) {
			/* Test mode - check if restorable */
			if (is_restorable(fd, path)) {
				printf("Restorable: %s\n", path);
			} else {
				printf("Not restorable: %s (%s)\n", path, strerror(errno));
				errors++;
			}
		} else {
			/* Normal mode - restore the file */
			printf("Restoring: %s\n", path);

			if (restore_lower(fd, path) < 0) {
				fprintf(stderr, "Error: Failed to restore '%s': %s\n",
					path, strerror(errno));
				errors++;
			} else {
				printf("Successfully restored: %s\n", path);
			}
		}
	}

	close(fd);

	if (errors > 0) {
		fprintf(stderr, "\nCompleted with %d error(s)\n", errors);
		return 1;
	}

	if (test_mode) {
		printf("\nAll files are restorable\n");
	} else {
		printf("\nAll files restored successfully\n");
	}
	return 0;
}
