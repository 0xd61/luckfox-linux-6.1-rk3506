# OverlayFS Lower Layer Restoration ioctl

## Overview

This patch adds a new ioctl `OVL_IOC_RESTORE_LOWER` to the overlayfs filesystem that allows userspace to restore visibility of lower layer files by removing whiteouts from the upper layer.

## Motivation

In dual-layer package management systems (like Calculinux), a common scenario arises:

1. User installs a package into the overlay layer (e.g., SDL library)
2. A system update includes a newer version of that package in the base image
3. During reconciliation, the old overlay package is removed with `opkg remove`
4. OverlayFS creates whiteout files (character device whiteout markers) for each removed file
5. These whiteouts persist, blocking access to the newer base image version

Previously, the only solution was to:
- Manually delete whiteout files from the upper layer
- Remount the entire filesystem to refresh the dentry cache

This patch provides a cleaner, more surgical approach.

## Implementation

The patch consists of four main components:

### 1. UAPI Header (`include/uapi/linux/overlayfs.h`)

Defines the ioctl interface:

```c
struct ovl_restore_lower_args {
    __aligned_u64 path_ptr;  /* Pointer to path string */
    __u32 path_len;          /* Length of path string */
    __u32 flags;             /* Reserved for future use */
};

#define OVL_IOC_RESTORE_LOWER _IOW('O', 1, struct ovl_restore_lower_args)
```

### 2. Kernel Implementation (`fs/overlayfs/ioctl.c`)

Two main functions:

- `ovl_restore_lower_by_path()` - Core logic to remove whiteout and invalidate dentry
- `ovl_ioctl()` - ioctl handler that validates arguments and calls restoration function

Key features:
- Validates path is within the overlay
- Verifies upper dentry exists and is actually a whiteout
- Uses proper credentials (`ovl_override_creds()`)
- Locks parent directory during removal
- Invalidates dentry cache with `d_drop()` - no remount needed!

### 3. Userspace Tool (`tools/ovl-restore/`)

Simple command-line tool to use the ioctl:

```bash
ovl-restore / /usr/bin/foo /usr/lib/libbar.so
```

### 4. Integration (`fs/overlayfs/file.c` and `Makefile`)

- Added `.unlocked_ioctl = ovl_ioctl` to `ovl_file_operations`
- Updated Makefile to compile `ioctl.o`

## Benefits

✅ **Surgical precision** - Restore specific lower layer files without affecting others  
✅ **No remount required** - Dentry invalidation happens automatically  
✅ **Better performance** - Faster than full filesystem remount  
✅ **Safer operation** - Kernel validates all operations atomically  
✅ **User-friendly semantics** - Explicit "restore lower layer file" operation

## Usage Example

### From C code:

```c
#include <linux/overlayfs.h>

int fd = open("/", O_RDONLY | O_DIRECTORY);
struct ovl_restore_lower_args args = {
    .path_ptr = (uint64_t)"/usr/bin/foo",
    .path_len = strlen("/usr/bin/foo"),  /* Length without null terminator */
    .flags = 0
};

if (ioctl(fd, OVL_IOC_RESTORE_LOWER, &args) < 0)
    perror("Failed to restore lower layer file");
```

### From shell (using ovl-restore tool):

```bash
# Restore a single file
ovl-restore / /usr/bin/foo

# Restore multiple files  
ovl-restore / /usr/bin/foo /usr/lib/libbar.so

# Can also be used in scripts
for file in $(cat whiteout-list.txt); do
    ovl-restore / "$file"
done
```

### From Python (for calculinux-update):

```python
import os
import fcntl
import ctypes

OVL_IOC_RESTORE_LOWER = 0x40104F01  # _IOW('O', 1, struct ovl_restore_lower_args)

class OvlRestoreLowerArgs(ctypes.Structure):
    _fields_ = [
        ("path_ptr", ctypes.c_uint64),
        ("path_len", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
    ]

def restore_lower(mount_point, path):
    with open(mount_point, 'r') as f:
        # Encode string to bytes (does not include null terminator)
        path_bytes = path.encode('utf-8')
        
        # Allocate a NUL-terminated C string buffer that stays alive during ioctl
        path_buf = ctypes.create_string_buffer(path_bytes + b'\0')
        
        # Prepare the ioctl argument structure
        args = OvlRestoreLowerArgs()
        args.path_ptr = ctypes.addressof(path_buf)
        args.path_len = len(path_bytes)  # strlen, not including null
        args.flags = 0
        
        # Convert the structure to a bytes buffer for ioctl
        arg_buf = ctypes.string_at(ctypes.byref(args), ctypes.sizeof(args))
        fcntl.ioctl(f.fileno(), OVL_IOC_RESTORE_LOWER, arg_buf)
```

## Error Codes

- `0` - Success
- `-ENOENT` - No whiteout exists at the specified path
- `-EINVAL` - Path is invalid or doesn't point to a whiteout
- `-EPERM` - Caller lacks permissions
- `-EROFS` - Overlay is read-only (no upper layer)
- `-EFAULT` - Bad address in arguments
- `-ENOMEM` - Out of memory
- `-ENOTTY` - Invalid ioctl command

## Testing

The implementation can be tested with:

```bash
# Mount an overlayfs
mkdir -p /tmp/lower /tmp/upper /tmp/work /tmp/merged
mount -t overlay overlay -o lowerdir=/tmp/lower,upperdir=/tmp/upper,workdir=/tmp/work /tmp/merged

# Create a file in lower
echo "lower" > /tmp/lower/test.txt

# Create a whiteout in upper (simulate package removal)
rm /tmp/merged/test.txt

# Verify whiteout exists
ls -la /tmp/upper/  # Should show c--------- 1 root root 0, 0 test.txt

# Restore the file
./ovl-restore /tmp/merged /tmp/merged/test.txt

# Verify file is visible again
cat /tmp/merged/test.txt  # Should print "lower"
```

## Future Enhancements

Potential additions for future versions:

1. **Batch operation** - Restore multiple files in one ioctl call
2. **Recursive restoration** - Restore all files under a directory
3. **Statistics** - Return number of files restored

## Integration with Calculinux-Update

The calculinux-update Python package can be updated to use this ioctl when available, with fallback to the current remount approach for older kernels:

```python
def cleanup_whiteouts(packages):
    """Restore lower layer files for removed packages."""
    try:
        # Try new ioctl method first
        for pkg in packages:
            for file in get_package_files(pkg):
                try:
                    restore_lower("/", file)
                except OSError:
                    pass  # File might not have whiteout
    except (OSError, IOError):
        # Fall back to old method: manual removal + remount
        remove_whiteouts_manually(packages)
        remount_overlayfs("/")
```

## Upstream Submission

This patch could potentially be submitted to the Linux kernel mailing list with:

- Use case documentation emphasizing dual-layer package management
- Container ecosystem benefits (Docker, Podman could use this too)
- Proper testing and validation
- Performance benchmarks vs remount approach

Contact: linux-fsdevel@vger.kernel.org, linux-unionfs@vger.kernel.org  
Maintainers: Miklos Szeredi <miklos@szeredi.hu>, Amir Goldstein <amir73il@gmail.com>

## Files Modified

- `include/uapi/linux/overlayfs.h` - New file, UAPI header
- `fs/overlayfs/ioctl.c` - New file, ioctl implementation  
- `fs/overlayfs/overlayfs.h` - Added function declaration
- `fs/overlayfs/file.c` - Added .unlocked_ioctl handler
- `fs/overlayfs/Makefile` - Added ioctl.o to build
- `tools/ovl-restore/` - New directory with userspace tool

## License

All code is licensed under GPL-2.0 to match the kernel's licensing.
