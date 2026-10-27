// Exercises the syscalls Wayland clients, compositors and waypipe rely on:
// memfd_create + seals, SCM_RIGHTS fd passing, accept4, signalfd,
// timerfd_gettime, close_range and mremap of shared file mappings.
// Build e.g. with: zig cc -target x86-linux-musl -static -o wayland wayland.c
// Run it on a fakefs root (realfs does not support creating unix sockets).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/syscall.h>
#include <sys/stat.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #cond, errno, strerror(errno)); failures++; } } while (0)

static int send_fds(int sock, const char *data, int *fds, int nfds) {
    struct iovec iov = {.iov_base = (void *) data, .iov_len = strlen(data)};
    char buf[CMSG_SPACE(sizeof(int) * 16)];
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    if (nfds) {
        msg.msg_control = buf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
    }
    return sendmsg(sock, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
}

static int recv_fds(int sock, char *data, size_t len, int *fds, int *nfds, size_t ctl, int *msgflags) {
    struct iovec iov = {.iov_base = data, .iov_len = len};
    char buf[CMSG_SPACE(sizeof(int) * 64)];
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = buf, .msg_controllen = ctl};
    int r = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    *nfds = 0;
    if (r < 0) return r;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            memcpy(fds + *nfds, CMSG_DATA(c), n * sizeof(int));
            *nfds += n;
        }
    }
    if (msgflags) *msgflags = msg.msg_flags;
    return r;
}

static void test_memfd(void) {
    int fd = memfd_create("wl_shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(fd >= 0);
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    CHECK(ftruncate(fd, 8192) == 0);
    struct stat st; CHECK(fstat(fd, &st) == 0); CHECK(st.st_size == 8192); CHECK(S_ISREG(st.st_mode));
    char *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED);
    strcpy(p, "hello");
    char buf[6] = {0}; CHECK(pread(fd, buf, 5, 0) == 5); CHECK(strcmp(buf, "hello") == 0);
    CHECK(fcntl(fd, F_GET_SEALS) == 0);
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == 0);
    CHECK(fcntl(fd, F_GET_SEALS) == (F_SEAL_SHRINK | F_SEAL_GROW));
    CHECK(ftruncate(fd, 4096) < 0 && errno == EPERM);
    CHECK(ftruncate(fd, 16384) < 0 && errno == EPERM);
    CHECK(pwrite(fd, "x", 1, 8192) < 0 && errno == EPERM);
    CHECK(pwrite(fd, "x", 1, 100) == 1);
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL) == 0);
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) < 0 && errno == EPERM);
    char link[128] = {0}, path[64];
    sprintf(path, "/proc/self/fd/%d", fd);
    readlink(path, link, sizeof(link) - 1);
    printf("memfd link: %s\n", link);
    int fd2 = memfd_create("noseal", 0);
    CHECK(fd2 >= 0);
    CHECK(fcntl(fd2, F_GET_SEALS) == F_SEAL_SEAL);
    CHECK(fcntl(fd2, F_ADD_SEALS, F_SEAL_GROW) < 0 && errno == EPERM);
    int fd3 = memfd_create("ws", MFD_ALLOW_SEALING);
    CHECK(ftruncate(fd3, 4096) == 0);
    CHECK(fcntl(fd3, F_ADD_SEALS, F_SEAL_WRITE) == 0);
    CHECK(write(fd3, "x", 1) < 0 && errno == EPERM);
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd3, 0) == MAP_FAILED && errno == EPERM);
    CHECK(mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd3, 0) != MAP_FAILED);
    close(fd2); close(fd3);

    // mremap a shared file mapping (wl_shm_pool_resize)
    int fd4 = memfd_create("pool", 0);
    CHECK(ftruncate(fd4, 4096) == 0);
    char *a = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd4, 0);
    char *blocker = mmap(a + 4096, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(blocker == a + 4096);
    strcpy(a, "pool");
    CHECK(ftruncate(fd4, 3 * 4096) == 0);
    CHECK(pwrite(fd4, "tail", 5, 2 * 4096) == 5);
    char *b = mremap(a, 4096, 3 * 4096, MREMAP_MAYMOVE);
    CHECK(b != MAP_FAILED);
    if (b != MAP_FAILED) {
        CHECK(strcmp(b, "pool") == 0);
        CHECK(strcmp(b + 2 * 4096, "tail") == 0);
        b[4096] = 'z';
        char c; CHECK(pread(fd4, &c, 1, 4096) == 1 && c == 'z');
    }
    close(fd4);
    munmap(p, 8192);
    close(fd);
}

static void test_scm(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    CHECK(fcntl(sv[0], F_GETFD) & FD_CLOEXEC);
    struct ucred cred; socklen_t len = sizeof(cred);
    CHECK(getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0);
    CHECK(cred.pid == getpid());

    int mfd = memfd_create("buf", MFD_CLOEXEC);
    ftruncate(mfd, 4096);
    char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    strcpy(p, "parent");

    pid_t pid = fork();
    if (pid == 0) {
        char data[64]; int fds[64], n;
        int r = recv_fds(sv[1], data, 4, fds, &n, sizeof(char[CMSG_SPACE(sizeof(int) * 64)]), NULL);
        if (r != 4 || n != 1) _exit(10);
        if (!(fcntl(fds[0], F_GETFD) & FD_CLOEXEC)) _exit(11);
        char *q = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
        if (q == MAP_FAILED || strcmp(q, "parent") != 0) _exit(12);
        strcpy(q, "child");
        _exit(0);
    }
    CHECK(send_fds(sv[0], "abcd", &mfd, 1) == 4);
    int status; waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (WIFEXITED(status)) printf("child exit %d\n", WEXITSTATUS(status));
    CHECK(strcmp(p, "child") == 0);

    // several messages with fds, received at once
    int a = dup(mfd), b = dup(mfd), c = dup(mfd);
    CHECK(send_fds(sv[0], "11", &a, 1) == 2);
    int bc[2] = {b, c};
    CHECK(send_fds(sv[0], "22", bc, 2) == 2);
    CHECK(send_fds(sv[0], "33", NULL, 0) == 2);
    usleep(10000);
    char data[64] = {0}; int fds[64], n, flags;
    int r = recv_fds(sv[1], data, sizeof(data), fds, &n, sizeof(char[CMSG_SPACE(sizeof(int) * 64)]), &flags);
    printf("coalesced recv: %d bytes '%s', %d fds\n", r, data, n);
    int total_bytes = r, total_fds = n;
    while (total_bytes < 6) {
        r = recv_fds(sv[1], data, sizeof(data), fds + total_fds, &n, sizeof(char[CMSG_SPACE(sizeof(int) * 64)]), &flags);
        if (r <= 0) break;
        total_bytes += r; total_fds += n;
    }
    CHECK(total_bytes == 6);
    CHECK(total_fds == 3);
    for (int i = 0; i < total_fds; i++) close(fds[i]);

    // read() discards fds but must keep the queue in sync
    CHECK(send_fds(sv[0], "R", &a, 1) == 1);
    char ch; CHECK(read(sv[1], &ch, 1) == 1 && ch == 'R');
    CHECK(send_fds(sv[0], "S", &b, 1) == 1);
    int bfd;
    r = recv_fds(sv[1], data, 1, &bfd, &n, sizeof(char[CMSG_SPACE(sizeof(int) * 64)]), NULL);
    CHECK(r == 1 && data[0] == 'S' && n == 1);
    if (n == 1) {
        struct stat s1, s2; fstat(bfd, &s1); fstat(b, &s2);
        CHECK(s1.st_ino == s2.st_ino);
        close(bfd);
    }

    // control buffer too small -> MSG_CTRUNC
    CHECK(send_fds(sv[0], "T", bc, 2) == 1);
    r = recv_fds(sv[1], data, 1, fds, &n, CMSG_SPACE(sizeof(int)), &flags);
    CHECK(r == 1 && n == 1 && (flags & MSG_CTRUNC));
    if (n) close(fds[0]);

    // bad fd
    int bad = 999;
    CHECK(send_fds(sv[0], "B", &bad, 1) < 0 && errno == EBADF);

    close(a); close(b); close(c);
    close(sv[0]); close(sv[1]);
}

static void test_accept4(void) {
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    CHECK(s >= 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, "/tmp/wayland-test-0");
    unlink(addr.sun_path);
    CHECK(bind(s, (void *) &addr, sizeof(addr)) == 0);
    CHECK(listen(s, 8) == 0);
    CHECK(accept4(s, NULL, NULL, SOCK_CLOEXEC) < 0 && errno == EAGAIN);
    pid_t pid = fork();
    if (pid == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (connect(c, (void *) &addr, sizeof(addr)) < 0) _exit(20);
        int mfd = memfd_create("x", 0);
        if (send_fds(c, "hi", &mfd, 1) != 2) _exit(21);
        char ch; read(c, &ch, 1);
        _exit(0);
    }
    struct pollfd pfd = {.fd = s, .events = POLLIN};
    CHECK(poll(&pfd, 1, 5000) == 1);
    int c = accept4(s, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    CHECK(c >= 0);
    CHECK(fcntl(c, F_GETFD) & FD_CLOEXEC);
    CHECK(fcntl(c, F_GETFL) & O_NONBLOCK);
    struct ucred cred; socklen_t len = sizeof(cred);
    CHECK(getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.pid == pid);
    pfd.fd = c;
    CHECK(poll(&pfd, 1, 5000) == 1);
    char data[8] = {0}; int fds[4], n;
    CHECK(recv_fds(c, data, sizeof(data), fds, &n, sizeof(char[CMSG_SPACE(sizeof(int) * 64)]), NULL) == 2 && n == 1);
    write(c, "x", 1);
    int status; waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(c); close(s);
    unlink(addr.sun_path);
}

static void test_signalfd(void) {
    sigset_t mask; sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1); sigaddset(&mask, SIGCHLD);
    CHECK(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    CHECK(sfd >= 0);
    struct signalfd_siginfo si;
    CHECK(read(sfd, &si, sizeof(si)) < 0 && errno == EAGAIN);
    kill(getpid(), SIGUSR1);
    struct pollfd pfd = {.fd = sfd, .events = POLLIN};
    CHECK(poll(&pfd, 1, 1000) == 1);
    CHECK(read(sfd, &si, sizeof(si)) == sizeof(si) && si.ssi_signo == SIGUSR1);
    CHECK(si.ssi_pid == (uint32_t) getpid());

    pid_t pid = fork();
    if (pid == 0) { usleep(50000); _exit(7); }
    CHECK(poll(&pfd, 1, 5000) == 1);
    CHECK(read(sfd, &si, sizeof(si)) == sizeof(si) && si.ssi_signo == SIGCHLD);
    printf("sigchld pid %u (child %d) status %d\n", si.ssi_pid, pid, si.ssi_status);
    waitpid(pid, NULL, 0);

    // blocking read woken by a signal from another process
    int bsfd = signalfd(-1, &mask, 0);
    pid_t parent = getpid();
    pid = fork();
    if (pid == 0) { usleep(50000); kill(parent, SIGUSR1); _exit(0); }
    CHECK(read(bsfd, &si, sizeof(si)) == sizeof(si));
    printf("blocking signalfd got %d\n", si.ssi_signo);
    waitpid(pid, NULL, 0);
    // drain a possible SIGCHLD
    read(sfd, &si, sizeof(si));
    close(bsfd);
    close(sfd);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

static void test_timerfd(void) {
    int t = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    CHECK(t >= 0);
    struct itimerspec its = {.it_value = {.tv_sec = 5}, .it_interval = {.tv_sec = 1}};
    CHECK(timerfd_settime(t, 0, &its, NULL) == 0);
    struct itimerspec cur;
    CHECK(timerfd_gettime(t, &cur) == 0);
    printf("timerfd remaining %ld.%09ld interval %ld\n", (long) cur.it_value.tv_sec, cur.it_value.tv_nsec, (long) cur.it_interval.tv_sec);
    CHECK(cur.it_value.tv_sec >= 4 && cur.it_value.tv_sec <= 5 && cur.it_interval.tv_sec == 1);
    close(t);
}

static void test_close_range(void) {
    int a = dup(0), b = dup(0), c = dup(0);
    CHECK(syscall(SYS_close_range, b, ~0U, 4 /* CLOSE_RANGE_CLOEXEC */) == 0);
    CHECK(fcntl(b, F_GETFD) & FD_CLOEXEC);
    CHECK(!(fcntl(a, F_GETFD) & FD_CLOEXEC));
    CHECK(syscall(SYS_close_range, b, c, 0) == 0);
    CHECK(fcntl(b, F_GETFD) < 0 && errno == EBADF);
    CHECK(fcntl(c, F_GETFD) < 0 && errno == EBADF);
    close(a);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_memfd(); printf("memfd done\n");
    test_scm(); printf("scm done\n");
    test_accept4(); printf("accept4 done\n");
    test_signalfd(); printf("signalfd done\n");
    test_timerfd(); printf("timerfd done\n");
    test_close_range(); printf("close_range done\n");
    printf("%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
    return failures != 0;
}
