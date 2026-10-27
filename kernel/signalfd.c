#include <string.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/poll.h"

// signalfd is used by Wayland compositors (wl_event_loop_add_signal) and lots
// of other event loop based programs.

static const struct fd_ops signalfd_ops;

// All open signalfds, so that sending a signal can wake up anyone polling one.
static struct list signalfds = LIST_INITIALIZER(signalfds);
static lock_t signalfd_lock = LOCK_INITIALIZER;

struct signalfd_siginfo_ {
    uint32_t signo;
    int32_t errno_;
    int32_t code;
    uint32_t pid;
    uint32_t uid;
    int32_t fd;
    uint32_t tid;
    uint32_t band;
    uint32_t overrun;
    uint32_t trapno;
    int32_t status;
    int32_t int_;
    uint64_t ptr;
    uint64_t utime;
    uint64_t stime;
    uint64_t addr;
    uint16_t addr_lsb;
    uint16_t pad2;
    int32_t syscall;
    uint64_t call_addr;
    uint32_t arch;
    uint8_t pad[28];
};
static_assert(sizeof(struct signalfd_siginfo_) == 128, "signalfd_siginfo size");

#define SFD_CLOEXEC_ O_CLOEXEC_
#define SFD_NONBLOCK_ O_NONBLOCK_

static sigset_t_ signalfd_sanitize_mask(sigset_t_ mask) {
    // SIGKILL and SIGSTOP can't be read from a signalfd
    return mask & ~(sig_mask(SIGKILL_) | sig_mask(SIGSTOP_));
}

int_t sys_signalfd4(fd_t f, addr_t mask_addr, uint_t mask_size, int_t flags) {
    STRACE("signalfd4(%d, %#x, %u, %#x)", f, mask_addr, mask_size, flags);
    if (mask_size != sizeof(sigset_t_))
        return _EINVAL;
    if (flags & ~(SFD_CLOEXEC_ | SFD_NONBLOCK_))
        return _EINVAL;
    sigset_t_ mask;
    if (user_get(mask_addr, mask))
        return _EFAULT;
    mask = signalfd_sanitize_mask(mask);

    if (f != -1) {
        struct fd *fd = f_get(f);
        if (fd == NULL)
            return _EBADF;
        if (fd->ops != &signalfd_ops)
            return _EINVAL;
        lock(&fd->lock);
        fd->signalfd.mask = mask;
        unlock(&fd->lock);
        // the new mask might make it readable
        poll_wakeup(fd, POLL_READ);
        return f;
    }

    struct fd *fd = adhoc_fd_create(&signalfd_ops);
    if (fd == NULL)
        return _ENOMEM;
    fd->signalfd.mask = mask;
    lock(&signalfd_lock);
    list_add(&signalfds, &fd->signalfd.signalfds);
    unlock(&signalfd_lock);
    return f_install(fd, flags);
}

int_t sys_signalfd(fd_t f, addr_t mask_addr, uint_t mask_size) {
    return sys_signalfd4(f, mask_addr, mask_size, 0);
}

static sigset_t_ signalfd_get_mask(struct fd *fd) {
    lock(&fd->lock);
    sigset_t_ mask = fd->signalfd.mask;
    unlock(&fd->lock);
    return mask;
}

static void siginfo_to_signalfd(struct siginfo_ *info, struct signalfd_siginfo_ *out) {
    memset(out, 0, sizeof(*out));
    out->signo = info->sig;
    out->errno_ = info->sig_errno;
    out->code = info->code;
    switch (info->sig) {
        case SIGCHLD_:
            out->pid = info->child.pid;
            out->uid = info->child.uid;
            out->status = info->child.status;
            out->utime = info->child.utime;
            out->stime = info->child.stime;
            break;
        case SIGSEGV_: case SIGBUS_: case SIGILL_: case SIGFPE_: case SIGTRAP_:
            out->addr = info->fault.addr;
            break;
        default:
            if (info->code == SI_TIMER_) {
                out->tid = info->timer.timer;
                out->overrun = info->timer.overrun;
                out->ptr = info->timer.value.sv_ptr;
                out->int_ = info->timer.value.sv_int;
            } else {
                out->pid = info->kill.pid;
                out->uid = info->kill.uid;
            }
            break;
    }
}

// Must be called with current->sighand->lock held. Returns the number of
// signals dequeued into infos.
static size_t signalfd_dequeue(sigset_t_ mask, struct siginfo_ *infos, size_t max) {
    size_t n = 0;
    struct sigqueue *sigqueue, *tmp;
    list_for_each_entry_safe(&current->queue, sigqueue, tmp, queue) {
        if (n >= max)
            break;
        int sig = sigqueue->info.sig;
        if (!sigset_has(mask, sig))
            continue;
        list_remove(&sigqueue->queue);
        sigset_del(&current->pending, sig);
        infos[n++] = sigqueue->info;
        free(sigqueue);
    }
    return n;
}

static ssize_t signalfd_read(struct fd *fd, void *buf, size_t bufsize) {
    size_t max = bufsize / sizeof(struct signalfd_siginfo_);
    if (max == 0)
        return _EINVAL;
    if (max > NUM_SIGS)
        max = NUM_SIGS;
    struct siginfo_ infos[NUM_SIGS];
    sigset_t_ mask = signalfd_get_mask(fd);

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock);
    size_t n;
    while ((n = signalfd_dequeue(mask, infos, max)) == 0) {
        if (fd->flags & O_NONBLOCK_) {
            unlock(&sighand->lock);
            return _EAGAIN;
        }
        // Setting waiting makes signals in the mask wake us up even if
        // they're blocked, same as sigtimedwait.
        current->waiting = mask;
        int err = wait_for(&current->pause, &sighand->lock, NULL);
        current->waiting = 0;
        if (err == _EINTR) {
            n = signalfd_dequeue(mask, infos, max);
            if (n == 0) {
                unlock(&sighand->lock);
                return _EINTR;
            }
            break;
        }
    }
    unlock(&sighand->lock);

    struct signalfd_siginfo_ *out = buf;
    for (size_t i = 0; i < n; i++)
        siginfo_to_signalfd(&infos[i], &out[i]);
    return n * sizeof(struct signalfd_siginfo_);
}

static int signalfd_poll(struct fd *fd) {
    sigset_t_ mask = signalfd_get_mask(fd);
    lock(&current->sighand->lock);
    bool ready = !!(current->pending & mask);
    unlock(&current->sighand->lock);
    return ready ? POLL_READ : 0;
}

static int signalfd_close(struct fd *fd) {
    lock(&signalfd_lock);
    list_remove(&fd->signalfd.signalfds);
    unlock(&signalfd_lock);
    return 0;
}

void signalfd_notify(int sig) {
    lock(&signalfd_lock);
    struct fd *fd;
    list_for_each_entry(&signalfds, fd, signalfd.signalfds) {
        if (sigset_has(signalfd_get_mask(fd), sig))
            poll_wakeup(fd, POLL_READ);
    }
    unlock(&signalfd_lock);
}

static const struct fd_ops signalfd_ops = {
    .read = signalfd_read,
    .poll = signalfd_poll,
    .close = signalfd_close,
};
