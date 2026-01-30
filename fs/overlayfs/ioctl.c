// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Calculinux Project
 * Author: Ben Klop
 *
 * Overlayfs ioctl operations
 */

#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/uaccess.h>
#include <linux/xattr.h>
#include <uapi/linux/overlayfs.h>
#include "overlayfs.h"

/**
 * ovl_check_restorable - Check if a path has a restorable whiteout
 * @dentry: overlay dentry
 * @pathname: absolute path from the system root (e.g., "/usr/bin/foo")
 * @upper_dentry_out: optional pointer to store upper dentry (caller must dput)
 * @path_out: optional pointer to store path (caller must path_put)
 *
 * Checks if the given path has a whiteout in the upper layer that can be
 * removed to restore the lower layer file. The path must be an absolute path
 * starting with '/' and pointing to a file within the overlay filesystem.
 *
 * Returns 0 if restorable, negative error code otherwise.
 */
static int ovl_check_restorable(struct dentry *dentry, const char *pathname,
				struct dentry **upper_dentry_out,
				struct path *path_out)
{
	struct ovl_fs *ofs = OVL_FS(dentry->d_sb);
	struct dentry *overlay_dentry, *upper_dentry;
	struct path path;
	int err;

	/* Must have a writable upper layer */
	if (!ovl_upper_mnt(ofs))
		return -EROFS;

	/* Lookup the overlay path */
	err = kern_path(pathname, 0, &path);
	if (err)
		return err;

	overlay_dentry = path.dentry;

	/* Verify it's in our overlay */
	if (overlay_dentry->d_sb != dentry->d_sb) {
		err = -EINVAL;
		goto out_path_put;
	}

	/* Get the upper dentry */
	upper_dentry = ovl_dentry_upper(overlay_dentry);
	if (!upper_dentry) {
		/* No upper dentry means no whiteout to remove */
		err = -ENOENT;
		goto out_path_put;
	}

	/* Verify it's actually a whiteout */
	if (!ovl_is_whiteout(upper_dentry)) {
		err = -EINVAL;
		goto out_path_put;
	}

	/* Success - file is restorable */
	if (upper_dentry_out)
		*upper_dentry_out = dget(upper_dentry);
	if (path_out)
		*path_out = path;
	else
		path_put(&path);

	return 0;

out_path_put:
	path_put(&path);
	return err;
}

/**
 * ovl_restore_lower_by_path - Restore visibility of a lower layer file
 * @dentry: overlay dentry
 * @pathname: absolute path from the system root (e.g., "/usr/bin/foo")
 *
 * This function removes a whiteout character device from the upper layer,
 * making the corresponding lower layer file visible again. It also
 * invalidates the dentry cache entry to ensure the change is immediately
 * visible.
 *
 * The path must be an absolute path starting with '/' and pointing to a 
 * file within the overlay filesystem that has a whiteout in the upper layer.
 *
 * Returns 0 on success, negative error code on failure.
 */
static int ovl_restore_lower_by_path(struct dentry *dentry,
					const char *pathname)
{
	struct ovl_fs *ofs = OVL_FS(dentry->d_sb);
	struct dentry *overlay_dentry, *upper_dentry, *upper_parent;
	struct inode *upper_dir;
	struct path path;
	const struct cred *old_cred;
	int err;

	/* Acquire write access to the filesystem */
	err = ovl_want_write(dentry);
	if (err)
		return err;

	/* Check if file is restorable and get the path/upper_dentry */
	err = ovl_check_restorable(dentry, pathname, &upper_dentry, &path);
	if (err)
		goto out_drop_write;

	overlay_dentry = path.dentry;

	/* Get the parent directory */
	upper_parent = dget_parent(upper_dentry);
	upper_dir = d_inode(upper_parent);

	/* Lock the parent directory */
	inode_lock(upper_dir);

	/* Check if user has permission to delete from parent directory
	 * We check this BEFORE elevating credentials to prevent privilege escalation
	 */
	err = inode_permission(ovl_upper_mnt_userns(ofs), upper_dir,
			       MAY_WRITE | MAY_EXEC);
	if (err)
		goto out_unlock;

	/* Re-verify that upper_dentry is still a whiteout after acquiring the lock
	 * to prevent race conditions where the whiteout could have been removed or
	 * replaced between the initial check and lock acquisition
	 */
	if (!ovl_is_whiteout(upper_dentry)) {
		err = -EINVAL;
		goto out_unlock;
	}

	/* Remove the whiteout with proper credentials */
	old_cred = ovl_override_creds(dentry->d_sb);
	err = ovl_do_unlink(ofs, upper_dir, upper_dentry);
	revert_creds(old_cred);

	/*
	 * Invalidate the dentry immediately after successful unlink to prevent
	 * race conditions where another process could observe an inconsistent
	 * state (whiteout removed from disk but still cached)
	 */
	if (!err)
		d_drop(overlay_dentry);

out_unlock:

	inode_unlock(upper_dir);
	dput(upper_parent);
	dput(upper_dentry);

	if (err)
		goto out_path_put;

	pr_debug("restored lower layer file for %s\n", pathname);

out_path_put:
	path_put(&path);

out_drop_write:
	ovl_drop_write(dentry);
	return err;
}

/**
 * ovl_ioctl_validate_and_copy_path - Validate and copy path from userspace
 * @path_ptr: userspace pointer to null-terminated path string
 * @path_len: length of path string (excluding null terminator, as from strlen)
 * @flags: flags field (must be 0)
 * @pathname_out: pointer to store allocated pathname (caller must kfree)
 *
 * Validates ioctl arguments and copies a null-terminated path string from
 * userspace. The path_len should be the result of strlen() - i.e., it should
 * NOT include the null terminator. The kernel will allocate path_len + 1 bytes
 * and copy the string including its null terminator from userspace.
 *
 * Returns 0 on success, negative error code on failure.
 */
static int ovl_ioctl_validate_and_copy_path(__u64 path_ptr, __u32 path_len,
					     __u32 flags, char **pathname_out)
{
	char *pathname;

	/* Validate flags (must be 0 for now) */
	if (flags != 0)
		return -EINVAL;

	/* Validate path length (strlen, not including null terminator) */
	if (path_len == 0 || path_len >= PATH_MAX)
		return -EINVAL;

	/* Allocate buffer for the path string plus null terminator */
	pathname = kmalloc(path_len + 1, GFP_KERNEL);
	if (!pathname)
		return -ENOMEM;

	/* Copy the string including null terminator from userspace */
	if (copy_from_user(pathname, (char __user *)(uintptr_t)path_ptr,
			   path_len + 1)) {
		kfree(pathname);
		return -EFAULT;
	}

	/* Verify the string is properly null-terminated at the expected position */
	if (pathname[path_len] != '\0') {
		kfree(pathname);
		return -EINVAL;
	}

	/* Verify no embedded nulls - actual length should equal path_len */
	if (strnlen(pathname, path_len) != path_len) {
		kfree(pathname);
		return -EINVAL;
	}

	*pathname_out = pathname;
	return 0;
}

/**
 * ovl_ioctl - Handle overlay filesystem ioctl commands
 * @file: file pointer
 * @cmd: ioctl command
 * @arg: ioctl argument
 *
 * Currently supports:
 *   OVL_IOC_RESTORE_LOWER - Restore lower layer file visibility
 *   OVL_IOC_IS_RESTORABLE - Check if file can be restored
 */
long ovl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct dentry *dentry = file->f_path.dentry;
	void __user *argp = (void __user *)arg;
	struct ovl_restore_lower_args restore_args;
	struct ovl_is_restorable_args restorable_args;
	char *pathname;
	long err;

	switch (cmd) {
	case OVL_IOC_RESTORE_LOWER:
		/* Copy arguments from userspace */
		if (copy_from_user(&restore_args, argp, sizeof(restore_args)))
			return -EFAULT;

		/* Validate and copy path */
		err = ovl_ioctl_validate_and_copy_path(restore_args.path_ptr,
						       restore_args.path_len,
						       restore_args.flags,
						       &pathname);
		if (err)
			return err;

		/* Perform the operation */
		err = ovl_restore_lower_by_path(dentry, pathname);

		kfree(pathname);
		return err;

	case OVL_IOC_IS_RESTORABLE:
		/* Copy arguments from userspace */
		if (copy_from_user(&restorable_args, argp, sizeof(restorable_args)))
			return -EFAULT;

		/* Validate and copy path */
		err = ovl_ioctl_validate_and_copy_path(restorable_args.path_ptr,
						       restorable_args.path_len,
						       restorable_args.flags,
						       &pathname);
		if (err)
			return err;

		/* Check if file is restorable */
		err = ovl_check_restorable(dentry, pathname, NULL, NULL);

		kfree(pathname);
		return err;

	default:
		return -ENOTTY;
	}
}
