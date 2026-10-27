#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/mman.h>
#include <sys/syscall.h>
#endif
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/memfd.h"
#include "fs/real.h"

// memfd_create is implemented with an unlinked temporary file on the host.
// That gives us a real file descriptor which can be read, written, truncated
// and mmapped with MAP_SHARED, so mappings stay coherent between processes
// that got the fd passed over a unix socket (which is exactly what Wayland
// clients, compositors and waypipe do with wl_shm buffers).

static const struct fd_ops memfd_fdops;
static struct mount memfd_mount;

// fd->lock is held around pread/pwrite/lseek, so seals need their own lock
static lock_t seals_lock = LOCK_INITIALIZER;

static int host_tmpfile(void) {
#if defined(__linux__) && defined(SYS_memfd_create)
    int memfd = syscall(SYS_memfd_create, "ish-memfd", 1 /* MFD_CLOEXEC */);
    if (memfd >= 0)
        return memfd;
#endif
    const char *dir = getenv("TMPDIR");
    if (dir == NULL || *dir == '\0')
        dir = "/tmp";
    char path[MAX_PATH];
    if (snprintf(path, sizeof(path), "%s/ish-memfd.XXXXXX", dir) >= (int) sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

fd_t sys_memfd_create(addr_t name_addr, uint_t flags) {
    char name[MEMFD_NAME_MAX + 1];
    if (user_read_string(name_addr, name, sizeof(name)))
        return _EFAULT;
    STRACE("memfd_create(\"%s\", %#x)", name, flags);
    if (strnlen(name, sizeof(name)) > MEMFD_NAME_MAX)
        return _EINVAL;
    if (flags & MFD_HUGETLB_)
        return _EINVAL;
    if (flags & ~(MFD_CLOEXEC_ | MFD_ALLOW_SEALING_ | MFD_NOEXEC_SEAL_ | MFD_EXEC_))
        return _EINVAL;

    int real_fd = host_tmpfile();
    if (real_fd < 0)
        return errno_map();

    struct fd *fd = fd_create(&memfd_fdops);
    if (fd == NULL) {
        close(real_fd);
        return _ENOMEM;
    }
    mount_retain(&memfd_mount);
    fd->mount = &memfd_mount;
    fd->real_fd = real_fd;
    fd->type = S_IFREG;
    fd->flags = O_RDWR_;
    fd->stat = (struct statbuf) {
        .mode = S_IFREG | 0777,
        .uid = current->euid,
        .gid = current->egid,
    };
    if (flags & MFD_NOEXEC_SEAL_)
        fd->stat.mode &= ~0111;
    // the name is only cosmetic (it shows up in /proc/pid/fd), so truncate it
    snprintf(fd->memfd.name, sizeof(fd->memfd.name), "%s", name);
    fd->memfd.seals = 0;
    if (!(flags & (MFD_ALLOW_SEALING_ | MFD_NOEXEC_SEAL_)))
        fd->memfd.seals |= F_SEAL_SEAL_;
    if (flags & MFD_NOEXEC_SEAL_)
        fd->memfd.seals |= F_SEAL_EXEC_;

    return f_install(fd, flags & MFD_CLOEXEC_ ? O_CLOEXEC_ : 0);
}

bool is_memfd(struct fd *fd) {
    return fd->ops == &memfd_fdops;
}

int memfd_add_seals(struct fd *fd, unsigned seals) {
    if (!is_memfd(fd))
        return _EINVAL;
    if (seals & ~F_SEAL_ALL_)
        return _EINVAL;
    lock(&seals_lock);
    int err = 0;
    if (fd->memfd.seals & F_SEAL_SEAL_)
        err = _EPERM;
    else
        fd->memfd.seals |= seals;
    unlock(&seals_lock);
    return err;
}

int memfd_get_seals(struct fd *fd) {
    if (!is_memfd(fd))
        return _EINVAL;
    lock(&seals_lock);
    int seals = fd->memfd.seals;
    unlock(&seals_lock);
    return seals;
}

static unsigned memfd_seals(struct fd *fd) {
    lock(&seals_lock);
    unsigned seals = fd->memfd.seals;
    unlock(&seals_lock);
    return seals;
}

static int memfd_check_write(struct fd *fd, size_t size, off_t off) {
    unsigned seals = memfd_seals(fd);
    if (seals & (F_SEAL_WRITE_ | F_SEAL_FUTURE_WRITE_))
        return _EPERM;
    if (seals & F_SEAL_GROW_) {
        struct stat real_stat;
        if (fstat(fd->real_fd, &real_stat) < 0)
            return errno_map();
        if (off < 0) {
            off = lseek(fd->real_fd, 0, SEEK_CUR);
            if (off < 0)
                return errno_map();
        }
        if ((uint64_t) off + size > (uint64_t) real_stat.st_size)
            return _EPERM;
    }
    return 0;
}

static ssize_t memfd_write(struct fd *fd, const void *buf, size_t size) {
    int err = memfd_check_write(fd, size, -1);
    if (err < 0)
        return err;
    return realfs_write(fd, buf, size);
}

static ssize_t memfd_pwrite(struct fd *fd, const void *buf, size_t size, off_t off) {
    int err = memfd_check_write(fd, size, off);
    if (err < 0)
        return err;
    return realfs_pwrite(fd, buf, size, off);
}

static int memfd_mmap(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags) {
    if ((flags & MMAP_SHARED) && (prot & P_WRITE) &&
            (memfd_seals(fd) & (F_SEAL_WRITE_ | F_SEAL_FUTURE_WRITE_)))
        return _EPERM;
    return realfs_mmap(fd, mem, start, pages, offset, prot, flags);
}

static int memfd_fstat(struct fd *fd, struct statbuf *stat) {
    int err = realfs_fstat(fd, stat);
    if (err < 0)
        return err;
    stat->mode = fd->stat.mode;
    stat->uid = fd->stat.uid;
    stat->gid = fd->stat.gid;
    stat->nlink = 1;
    return 0;
}

static int memfd_fsetattr(struct fd *fd, struct attr attr) {
    switch (attr.type) {
        case attr_uid:
            fd->stat.uid = attr.uid;
            return 0;
        case attr_gid:
            fd->stat.gid = attr.gid;
            return 0;
        case attr_mode:
            if ((memfd_seals(fd) & F_SEAL_EXEC_) && ((attr.mode ^ fd->stat.mode) & 0111))
                return _EPERM;
            fd->stat.mode = (fd->stat.mode & S_IFMT) | (attr.mode & ~S_IFMT);
            return 0;
        case attr_size: {
            unsigned seals = memfd_seals(fd);
            struct stat real_stat;
            if (fstat(fd->real_fd, &real_stat) < 0)
                return errno_map();
            if ((off_t) attr.size < real_stat.st_size && (seals & F_SEAL_SHRINK_))
                return _EPERM;
            if ((off_t) attr.size > real_stat.st_size && (seals & F_SEAL_GROW_))
                return _EPERM;
            if (ftruncate(fd->real_fd, attr.size) < 0)
                return errno_map();
            return 0;
        }
    }
    return _EINVAL;
}

static int memfd_getpath(struct fd *fd, char *buf) {
    sprintf(buf, "/memfd:%s (deleted)", fd->memfd.name);
    return 0;
}

static int memfd_getflags(struct fd *fd) {
    return fd->flags;
}

static int memfd_setflags(struct fd *fd, dword_t flags) {
    // only O_APPEND and O_NONBLOCK can be changed, and neither changes
    // anything for a regular file that's only ever accessed with pread/pwrite
    // style calls on our side
    fd->flags = (fd->flags & ~(O_APPEND_ | O_NONBLOCK_)) | (flags & (O_APPEND_ | O_NONBLOCK_));
    return 0;
}

static const struct fd_ops memfd_fdops = {
    .read = realfs_read,
    .write = memfd_write,
    .pread = realfs_pread,
    .pwrite = memfd_pwrite,
    .lseek = realfs_lseek,
    .mmap = memfd_mmap,
    .poll = realfs_poll,
    .fsync = realfs_fsync,
    .close = realfs_close,
    .getflags = memfd_getflags,
    .setflags = memfd_setflags,
};

static const struct fs_ops memfd_fs = {
    .name = "tmpfs", .magic = 0x01021994, // TMPFS_MAGIC
    .fstat = memfd_fstat,
    .fsetattr = memfd_fsetattr,
    .getpath = memfd_getpath,
};

static struct mount memfd_mount = {
    .fs = &memfd_fs,
    .point = "",
};
