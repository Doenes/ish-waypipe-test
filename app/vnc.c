#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonCryptor.h>
#endif
#include "vnc.h"

#define ENCODING_RAW 0
#define ENCODING_COPYRECT 1
#define ENCODING_DESKTOP_SIZE -223
#define ENCODING_EXTENDED_DESKTOP_SIZE -308

#define MAX_FB_SIZE 8192

struct vnc_client {
    int fd;
    int minor_version;
    char *name;

    pthread_mutex_t write_lock;
    pthread_mutex_t fb_lock;
    uint32_t *fb;
    int width, height;

    bool supports_resize;
    uint32_t screen_id;
    uint32_t screen_flags;

    volatile bool shutting_down;
};

static void set_error(char *err, size_t err_size, const char *fmt, ...) {
    if (err == NULL || err_size == 0)
        return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(err, err_size, fmt, args);
    va_end(args);
}

static int read_all(struct vnc_client *c, void *buf, size_t size) {
    char *p = buf;
    while (size > 0) {
        ssize_t n = recv(c->fd, p, size, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        size -= n;
    }
    return 0;
}

static int skip(struct vnc_client *c, size_t size) {
    char buf[4096];
    while (size > 0) {
        size_t chunk = size < sizeof(buf) ? size : sizeof(buf);
        if (read_all(c, buf, chunk) < 0)
            return -1;
        size -= chunk;
    }
    return 0;
}

static int write_all(struct vnc_client *c, const void *buf, size_t size) {
    const char *p = buf;
    pthread_mutex_lock(&c->write_lock);
    int res = 0;
    while (size > 0) {
        ssize_t n = send(c->fd, p, size, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            res = -1;
            break;
        }
        p += n;
        size -= n;
    }
    pthread_mutex_unlock(&c->write_lock);
    return res;
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t) (p[0] << 8 | p[1]);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}
static void put16(uint8_t *p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v;
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static int read32(struct vnc_client *c, uint32_t *v) {
    uint8_t buf[4];
    if (read_all(c, buf, 4) < 0)
        return -1;
    *v = get32(buf);
    return 0;
}

// Reads a length-prefixed reason string after a failure
static void read_reason(struct vnc_client *c, char *err, size_t err_size, const char *prefix) {
    uint32_t len;
    if (read32(c, &len) < 0 || len > 4096) {
        set_error(err, err_size, "%s", prefix);
        return;
    }
    char reason[len + 1];
    if (read_all(c, reason, len) < 0) {
        set_error(err, err_size, "%s", prefix);
        return;
    }
    reason[len] = '\0';
    set_error(err, err_size, "%s: %s", prefix, reason);
}

static int resize_fb(struct vnc_client *c, int width, int height) {
    if (width <= 0 || height <= 0 || width > MAX_FB_SIZE || height > MAX_FB_SIZE)
        return -1;
    uint32_t *fb = calloc((size_t) width * height, sizeof(uint32_t));
    if (fb == NULL)
        return -1;
    free(c->fb);
    c->fb = fb;
    c->width = width;
    c->height = height;
    return 0;
}

static int vnc_auth(struct vnc_client *c, const char *password, char *err, size_t err_size) {
    uint8_t challenge[16];
    if (read_all(c, challenge, sizeof(challenge)) < 0) {
        set_error(err, err_size, "Connection closed during authentication");
        return -1;
    }
#ifdef __APPLE__
    // The key is the password padded to 8 bytes, with the bits of each byte
    // reversed (a historical accident that is now part of the protocol)
    uint8_t key[8] = {0};
    for (int i = 0; i < 8 && password[i] != '\0'; i++) {
        uint8_t b = password[i], r = 0;
        for (int bit = 0; bit < 8; bit++)
            if (b & (1 << bit))
                r |= 1 << (7 - bit);
        key[i] = r;
    }
    uint8_t response[16];
    size_t moved;
    CCCryptorStatus status = CCCrypt(kCCEncrypt, kCCAlgorithmDES, kCCOptionECBMode,
            key, kCCKeySizeDES, NULL, challenge, sizeof(challenge),
            response, sizeof(response), &moved);
    if (status != kCCSuccess) {
        set_error(err, err_size, "DES encryption failed");
        return -1;
    }
    if (write_all(c, response, sizeof(response)) < 0) {
        set_error(err, err_size, "Connection closed during authentication");
        return -1;
    }
    return 0;
#else
    (void) password;
    set_error(err, err_size, "VNC password authentication isn't supported on this platform");
    return -1;
#endif
}

static int read_security_result(struct vnc_client *c, char *err, size_t err_size) {
    uint32_t result;
    if (read32(c, &result) < 0) {
        set_error(err, err_size, "Connection closed during authentication");
        return -1;
    }
    if (result != 0) {
        if (c->minor_version >= 8)
            read_reason(c, err, err_size, "Authentication failed");
        else
            set_error(err, err_size, "Authentication failed");
        return -1;
    }
    return 0;
}

static int handshake(struct vnc_client *c, const char *password, char *err, size_t err_size) {
    char version[13] = {0};
    if (read_all(c, version, 12) < 0) {
        set_error(err, err_size, "Connection closed before the server said hello");
        return -1;
    }
    int major, minor;
    if (sscanf(version, "RFB %3d.%3d\n", &major, &minor) != 2 || major != 3) {
        set_error(err, err_size, "Not a VNC server");
        return -1;
    }
    if (minor >= 8)
        minor = 8;
    else if (minor >= 7)
        minor = 7;
    else
        minor = 3;
    c->minor_version = minor;
    char reply[13];
    snprintf(reply, sizeof(reply), "RFB 003.%03d\n", minor);
    if (write_all(c, reply, 12) < 0) {
        set_error(err, err_size, "Connection closed during handshake");
        return -1;
    }

    // Security
    uint8_t type = 0;
    if (minor == 3) {
        uint32_t server_type;
        if (read32(c, &server_type) < 0) {
            set_error(err, err_size, "Connection closed during handshake");
            return -1;
        }
        if (server_type == 0) {
            read_reason(c, err, err_size, "Connection refused");
            return -1;
        }
        if (server_type != 1 && server_type != 2) {
            set_error(err, err_size, "Unsupported security type %u", server_type);
            return -1;
        }
        type = server_type;
    } else {
        uint8_t num_types;
        if (read_all(c, &num_types, 1) < 0) {
            set_error(err, err_size, "Connection closed during handshake");
            return -1;
        }
        if (num_types == 0) {
            read_reason(c, err, err_size, "Connection refused");
            return -1;
        }
        uint8_t types[num_types];
        if (read_all(c, types, num_types) < 0) {
            set_error(err, err_size, "Connection closed during handshake");
            return -1;
        }
        bool has_none = false, has_vnc_auth = false;
        for (int i = 0; i < num_types; i++) {
            if (types[i] == 1)
                has_none = true;
            if (types[i] == 2)
                has_vnc_auth = true;
        }
        if (has_none)
            type = 1;
        else if (has_vnc_auth)
            type = 2;
        else {
            set_error(err, err_size, "The server requires an unsupported authentication method "
                    "(TLS or similar). Disable it, e.g. by not setting enable_auth in wayvnc's config.");
            return -1;
        }
        if (write_all(c, &type, 1) < 0) {
            set_error(err, err_size, "Connection closed during handshake");
            return -1;
        }
    }

    if (type == 2) {
        if (password == NULL || password[0] == '\0') {
            set_error(err, err_size, "The server requires a password");
            return -1;
        }
        if (vnc_auth(c, password, err, err_size) < 0)
            return -1;
        if (read_security_result(c, err, err_size) < 0)
            return -1;
    } else if (minor >= 8) {
        if (read_security_result(c, err, err_size) < 0)
            return -1;
    }

    // ClientInit, asking to share the desktop with other clients
    uint8_t shared = 1;
    if (write_all(c, &shared, 1) < 0) {
        set_error(err, err_size, "Connection closed during handshake");
        return -1;
    }

    // ServerInit
    uint8_t init[24];
    if (read_all(c, init, sizeof(init)) < 0) {
        set_error(err, err_size, "Connection closed during handshake");
        return -1;
    }
    int width = get16(init), height = get16(init + 2);
    uint32_t name_len = get32(init + 20);
    if (name_len > 4096) {
        set_error(err, err_size, "Invalid server name");
        return -1;
    }
    c->name = malloc(name_len + 1);
    if (c->name == NULL || read_all(c, c->name, name_len) < 0) {
        set_error(err, err_size, "Connection closed during handshake");
        return -1;
    }
    c->name[name_len] = '\0';
    if (resize_fb(c, width, height) < 0) {
        set_error(err, err_size, "Invalid framebuffer size %dx%d", width, height);
        return -1;
    }

    // SetPixelFormat: 32bpp, little endian, true color, xRGB
    uint8_t pixel_format[20] = {0};
    pixel_format[0] = 0; // message type
    pixel_format[4] = 32; // bits per pixel
    pixel_format[5] = 24; // depth
    pixel_format[6] = 0; // big endian
    pixel_format[7] = 1; // true color
    put16(pixel_format + 8, 255);
    put16(pixel_format + 10, 255);
    put16(pixel_format + 12, 255);
    pixel_format[14] = 16; // red shift
    pixel_format[15] = 8; // green shift
    pixel_format[16] = 0; // blue shift

    // SetEncodings
    static const int32_t encodings[] = {
        ENCODING_COPYRECT,
        ENCODING_RAW,
        ENCODING_EXTENDED_DESKTOP_SIZE,
        ENCODING_DESKTOP_SIZE,
    };
    const int num_encodings = sizeof(encodings) / sizeof(encodings[0]);
    uint8_t set_encodings[4 + 4 * num_encodings];
    set_encodings[0] = 2;
    set_encodings[1] = 0;
    put16(set_encodings + 2, num_encodings);
    for (int i = 0; i < num_encodings; i++)
        put32(set_encodings + 4 + 4 * i, (uint32_t) encodings[i]);

    if (write_all(c, pixel_format, sizeof(pixel_format)) < 0 ||
            write_all(c, set_encodings, sizeof(set_encodings)) < 0) {
        set_error(err, err_size, "Connection closed during handshake");
        return -1;
    }
    return 0;
}

static int request_update(struct vnc_client *c, bool incremental) {
    uint8_t msg[10];
    msg[0] = 3;
    msg[1] = incremental;
    pthread_mutex_lock(&c->fb_lock);
    put16(msg + 2, 0);
    put16(msg + 4, 0);
    put16(msg + 6, c->width);
    put16(msg + 8, c->height);
    pthread_mutex_unlock(&c->fb_lock);
    return write_all(c, msg, sizeof(msg));
}

struct vnc_client *vnc_connect(const char *host, int port, const char *password, char *err, size_t err_size) {
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *addrs;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    int gai_err = getaddrinfo(host, port_str, &hints, &addrs);
    if (gai_err != 0) {
        set_error(err, err_size, "%s: %s", host, gai_strerror(gai_err));
        return NULL;
    }
    int fd = -1;
    int saved_errno = 0;
    for (struct addrinfo *ai = addrs; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addrs);
    if (fd < 0) {
        set_error(err, err_size, "Can't connect to %s:%d: %s", host, port, strerror(saved_errno));
        return NULL;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    struct vnc_client *c = calloc(1, sizeof(struct vnc_client));
    if (c == NULL) {
        close(fd);
        set_error(err, err_size, "Out of memory");
        return NULL;
    }
    c->fd = fd;
    pthread_mutex_init(&c->write_lock, NULL);
    pthread_mutex_init(&c->fb_lock, NULL);
    if (handshake(c, password, err, err_size) < 0) {
        vnc_free(c);
        return NULL;
    }
    return c;
}

static int read_raw(struct vnc_client *c, int x, int y, int w, int h) {
    size_t row_size = (size_t) w * 4;
    uint8_t *row = malloc(row_size);
    if (row == NULL)
        return -1;
    for (int j = 0; j < h; j++) {
        if (read_all(c, row, row_size) < 0) {
            free(row);
            return -1;
        }
        pthread_mutex_lock(&c->fb_lock);
        if (y + j < c->height && x < c->width) {
            int n = x + w <= c->width ? w : c->width - x;
            uint32_t *dst = c->fb + (size_t) (y + j) * c->width + x;
            for (int i = 0; i < n; i++) {
                const uint8_t *p = row + i * 4;
                dst[i] = (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16;
            }
        }
        pthread_mutex_unlock(&c->fb_lock);
    }
    free(row);
    return 0;
}

static int read_copyrect(struct vnc_client *c, int x, int y, int w, int h) {
    uint8_t src[4];
    if (read_all(c, src, sizeof(src)) < 0)
        return -1;
    int sx = get16(src), sy = get16(src + 2);
    pthread_mutex_lock(&c->fb_lock);
    if (x + w <= c->width && y + h <= c->height && sx + w <= c->width && sy + h <= c->height) {
        // copy through a temporary buffer since the rects can overlap
        uint32_t *tmp = malloc((size_t) w * h * 4);
        if (tmp != NULL) {
            for (int j = 0; j < h; j++)
                memcpy(tmp + (size_t) j * w, c->fb + (size_t) (sy + j) * c->width + sx, (size_t) w * 4);
            for (int j = 0; j < h; j++)
                memcpy(c->fb + (size_t) (y + j) * c->width + x, tmp + (size_t) j * w, (size_t) w * 4);
            free(tmp);
        }
    }
    pthread_mutex_unlock(&c->fb_lock);
    return 0;
}

static int handle_resize(struct vnc_client *c, const struct vnc_callbacks *cb, int w, int h) {
    pthread_mutex_lock(&c->fb_lock);
    int res = 0;
    if (w != c->width || h != c->height) {
        res = resize_fb(c, w, h);
        if (res == 0 && cb->resized != NULL)
            cb->resized(cb->ctx, w, h);
    }
    pthread_mutex_unlock(&c->fb_lock);
    return res;
}

static int read_extended_desktop_size(struct vnc_client *c, const struct vnc_callbacks *cb, int reason, int status, int w, int h) {
    uint8_t header[4];
    if (read_all(c, header, sizeof(header)) < 0)
        return -1;
    int num_screens = header[0];
    for (int i = 0; i < num_screens; i++) {
        uint8_t screen[16];
        if (read_all(c, screen, sizeof(screen)) < 0)
            return -1;
        if (i == 0) {
            c->screen_id = get32(screen);
            c->screen_flags = get32(screen + 12);
        }
    }
    c->supports_resize = true;
    // reason 1 means this is a reply to our own request, a nonzero status
    // means the request failed and the size didn't change
    if (reason == 1 && status != 0)
        return 0;
    return handle_resize(c, cb, w, h);
}

static int read_framebuffer_update(struct vnc_client *c, const struct vnc_callbacks *cb, char *err, size_t err_size) {
    uint8_t header[3];
    if (read_all(c, header, sizeof(header)) < 0)
        return -1;
    int num_rects = get16(header + 1);
    for (int i = 0; i < num_rects; i++) {
        uint8_t rect[12];
        if (read_all(c, rect, sizeof(rect)) < 0)
            return -1;
        int x = get16(rect), y = get16(rect + 2), w = get16(rect + 4), h = get16(rect + 6);
        int32_t encoding = (int32_t) get32(rect + 8);
        int res;
        switch (encoding) {
            case ENCODING_RAW:
                res = read_raw(c, x, y, w, h);
                break;
            case ENCODING_COPYRECT:
                res = read_copyrect(c, x, y, w, h);
                break;
            case ENCODING_DESKTOP_SIZE:
                res = handle_resize(c, cb, w, h);
                break;
            case ENCODING_EXTENDED_DESKTOP_SIZE:
                res = read_extended_desktop_size(c, cb, x, y, w, h);
                break;
            default:
                set_error(err, err_size, "Server sent unsupported encoding %d", encoding);
                return -1;
        }
        if (res < 0) {
            set_error(err, err_size, "Connection lost");
            return -1;
        }
    }
    if (cb->updated != NULL)
        cb->updated(cb->ctx);
    return 0;
}

int vnc_run(struct vnc_client *c, const struct vnc_callbacks *cb, char *err, size_t err_size) {
    set_error(err, err_size, "Connection lost");
    if (request_update(c, false) < 0)
        goto out;
    for (;;) {
        uint8_t type;
        if (read_all(c, &type, 1) < 0)
            goto out;
        switch (type) {
            case 0: // FramebufferUpdate
                if (read_framebuffer_update(c, cb, err, err_size) < 0)
                    goto out;
                if (request_update(c, true) < 0)
                    goto out;
                break;
            case 1: { // SetColourMapEntries, we use true color so ignore it
                uint8_t header[5];
                if (read_all(c, header, sizeof(header)) < 0)
                    goto out;
                if (skip(c, (size_t) get16(header + 3) * 6) < 0)
                    goto out;
                break;
            }
            case 2: // Bell
                break;
            case 3: { // ServerCutText
                uint8_t header[7];
                if (read_all(c, header, sizeof(header)) < 0)
                    goto out;
                uint32_t len = get32(header + 3);
                if (len > 16 * 1024 * 1024)
                    goto out;
                char *text = malloc(len + 1);
                if (text == NULL)
                    goto out;
                if (read_all(c, text, len) < 0) {
                    free(text);
                    goto out;
                }
                text[len] = '\0';
                if (cb->clipboard != NULL)
                    cb->clipboard(cb->ctx, text, len);
                free(text);
                break;
            }
            default:
                set_error(err, err_size, "Server sent unknown message type %d", type);
                goto out;
        }
    }
out:
    return c->shutting_down ? 0 : -1;
}

void vnc_shutdown(struct vnc_client *c) {
    c->shutting_down = true;
    shutdown(c->fd, SHUT_RDWR);
}

void vnc_free(struct vnc_client *c) {
    close(c->fd);
    pthread_mutex_destroy(&c->write_lock);
    pthread_mutex_destroy(&c->fb_lock);
    free(c->fb);
    free(c->name);
    free(c);
}

const char *vnc_desktop_name(struct vnc_client *c) {
    return c->name;
}

bool vnc_supports_resize(struct vnc_client *c) {
    return c->supports_resize;
}

void vnc_lock(struct vnc_client *c) {
    pthread_mutex_lock(&c->fb_lock);
}

void vnc_unlock(struct vnc_client *c) {
    pthread_mutex_unlock(&c->fb_lock);
}

const uint32_t *vnc_framebuffer(struct vnc_client *c, int *width, int *height) {
    *width = c->width;
    *height = c->height;
    return c->fb;
}

int vnc_send_pointer(struct vnc_client *c, int x, int y, uint8_t buttons) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    uint8_t msg[6];
    msg[0] = 5;
    msg[1] = buttons;
    put16(msg + 2, x);
    put16(msg + 4, y);
    return write_all(c, msg, sizeof(msg));
}

int vnc_send_key(struct vnc_client *c, uint32_t keysym, bool down) {
    uint8_t msg[8] = {4, down};
    put32(msg + 4, keysym);
    return write_all(c, msg, sizeof(msg));
}

int vnc_send_clipboard(struct vnc_client *c, const char *text, size_t len) {
    uint8_t header[8] = {6};
    put32(header + 4, (uint32_t) len);
    // send both parts while holding the lock so they don't get split up
    pthread_mutex_lock(&c->write_lock);
    int res = 0;
    const char *parts[2] = {(const char *) header, text};
    size_t sizes[2] = {sizeof(header), len};
    for (int i = 0; i < 2 && res == 0; i++) {
        const char *p = parts[i];
        size_t size = sizes[i];
        while (size > 0) {
            ssize_t n = send(c->fd, p, size, 0);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                res = -1;
                break;
            }
            p += n;
            size -= n;
        }
    }
    pthread_mutex_unlock(&c->write_lock);
    return res;
}

int vnc_request_desktop_size(struct vnc_client *c, int width, int height) {
    if (!c->supports_resize)
        return -1;
    uint8_t msg[8 + 16] = {251};
    put16(msg + 2, width);
    put16(msg + 4, height);
    msg[6] = 1; // number of screens
    put32(msg + 8, c->screen_id);
    put16(msg + 12, 0);
    put16(msg + 14, 0);
    put16(msg + 16, width);
    put16(msg + 18, height);
    put32(msg + 20, c->screen_flags);
    return write_all(c, msg, sizeof(msg));
}
