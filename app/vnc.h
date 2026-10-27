// Minimal RFB (VNC) client protocol implementation.
//
// Plain C so it doesn't depend on UIKit and can be tested anywhere. Supports
// RFB 3.3/3.7/3.8, no auth and VNC password auth, the Raw and CopyRect
// encodings and the DesktopSize/ExtendedDesktopSize pseudo-encodings. That's
// enough for a local server like wayvnc, where bandwidth doesn't matter.
#ifndef APP_VNC_H
#define APP_VNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vnc_client;

struct vnc_callbacks {
    void *ctx;
    // The framebuffer changed size. Called with the framebuffer lock held.
    void (*resized)(void *ctx, int width, int height);
    // A framebuffer update was completely received.
    void (*updated)(void *ctx);
    // The server sent clipboard text (Latin-1 according to the spec).
    void (*clipboard)(void *ctx, const char *text, size_t len);
};

// Connects and does the handshake. On error returns NULL and puts a message
// in err.
struct vnc_client *vnc_connect(const char *host, int port, const char *password, char *err, size_t err_size);
// Reads messages from the server until the connection is closed or
// vnc_shutdown is called. Returns 0 if vnc_shutdown was called, -1 with a
// message in err otherwise.
int vnc_run(struct vnc_client *c, const struct vnc_callbacks *callbacks, char *err, size_t err_size);
// Makes vnc_run return. Can be called from any thread.
void vnc_shutdown(struct vnc_client *c);
// Frees everything, must only be called once vnc_run has returned (or if it
// was never called).
void vnc_free(struct vnc_client *c);

const char *vnc_desktop_name(struct vnc_client *c);
bool vnc_supports_resize(struct vnc_client *c);

// The framebuffer is 32 bit pixels with red in bits 16-23, green in 8-15 and
// blue in 0-7. Hold the lock while accessing it.
void vnc_lock(struct vnc_client *c);
void vnc_unlock(struct vnc_client *c);
const uint32_t *vnc_framebuffer(struct vnc_client *c, int *width, int *height);

// These can be called from any thread. They return -1 if the write failed.
int vnc_send_pointer(struct vnc_client *c, int x, int y, uint8_t buttons);
int vnc_send_key(struct vnc_client *c, uint32_t keysym, bool down);
int vnc_send_clipboard(struct vnc_client *c, const char *text, size_t len);
// Only works if vnc_supports_resize
int vnc_request_desktop_size(struct vnc_client *c, int width, int height);

#endif
