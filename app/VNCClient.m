//
//  VNCClient.m
//  iSH
//

#import "VNCClient.h"
#include "vnc.h"

@interface VNCClient ()
@property (nullable) NSString *password;
@property (readwrite) BOOL connected;
@property (readwrite, nullable) NSString *desktopName;
@property BOOL updatePending;
@end

@implementation VNCClient {
    struct vnc_client *_client;
    // Protects _client from being freed while it's being used
    NSLock *_clientLock;
}

- (instancetype)initWithHost:(NSString *)host port:(int)port password:(NSString *)password {
    if (self = [super init]) {
        _host = host;
        _port = port;
        _password = password;
        _clientLock = [NSLock new];
    }
    return self;
}

static void vnc_resized(void *ctx, int width, int height) {
    // the framebuffer lock is held here, nothing to do since copyFramebufferImage
    // reads the size every time
    (void) ctx; (void) width; (void) height;
}

static void vnc_updated(void *ctx) {
    VNCClient *self = (__bridge VNCClient *) ctx;
    // Coalesce updates, so the main thread doesn't get flooded if it can't
    // keep up with the server
    @synchronized (self) {
        if (self.updatePending)
            return;
        self.updatePending = YES;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        @synchronized (self) {
            self.updatePending = NO;
        }
        [self.delegate vncClientFramebufferDidChange:self];
    });
}

static void vnc_clipboard(void *ctx, const char *text, size_t len) {
    VNCClient *self = (__bridge VNCClient *) ctx;
    NSString *string = [[NSString alloc] initWithBytes:text length:len encoding:NSISOLatin1StringEncoding];
    if (string == nil)
        return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if ([self.delegate respondsToSelector:@selector(vncClient:didReceiveClipboardText:)])
            [self.delegate vncClient:self didReceiveClipboardText:string];
    });
}

- (void)connect {
    NSThread *thread = [[NSThread alloc] initWithTarget:self selector:@selector(run) object:nil];
    thread.name = @"VNC";
    thread.qualityOfService = NSQualityOfServiceUserInteractive;
    [thread start];
}

- (void)run {
    char err[512] = "";
    struct vnc_client *client = vnc_connect(self.host.UTF8String, self.port, self.password.UTF8String, err, sizeof(err));
    if (client == NULL) {
        NSString *error = @(err);
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.delegate vncClient:self didDisconnectWithError:error];
        });
        return;
    }

    [_clientLock lock];
    _client = client;
    [_clientLock unlock];
    NSString *name = @(vnc_desktop_name(client));
    dispatch_async(dispatch_get_main_queue(), ^{
        self.desktopName = name;
        self.connected = YES;
        [self.delegate vncClientDidConnect:self];
    });

    struct vnc_callbacks callbacks = {
        .ctx = (__bridge void *) self,
        .resized = vnc_resized,
        .updated = vnc_updated,
        .clipboard = vnc_clipboard,
    };
    int res = vnc_run(client, &callbacks, err, sizeof(err));

    [_clientLock lock];
    _client = NULL;
    vnc_free(client);
    [_clientLock unlock];

    NSString *error = res < 0 ? @(err) : nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        self.connected = NO;
        [self.delegate vncClient:self didDisconnectWithError:error];
    });
}

- (void)disconnect {
    [_clientLock lock];
    if (_client != NULL)
        vnc_shutdown(_client);
    [_clientLock unlock];
}

- (void)dealloc {
    [self disconnect];
}

- (void)withClient:(void (^)(struct vnc_client *client))block {
    [_clientLock lock];
    if (_client != NULL)
        block(_client);
    [_clientLock unlock];
}

- (int)width {
    __block int width = 0;
    [self withClient:^(struct vnc_client *client) {
        int height;
        vnc_lock(client);
        vnc_framebuffer(client, &width, &height);
        vnc_unlock(client);
    }];
    return width;
}

- (int)height {
    __block int height = 0;
    [self withClient:^(struct vnc_client *client) {
        int width;
        vnc_lock(client);
        vnc_framebuffer(client, &width, &height);
        vnc_unlock(client);
    }];
    return height;
}

- (BOOL)supportsDesktopResize {
    __block BOOL supported = NO;
    [self withClient:^(struct vnc_client *client) {
        supported = vnc_supports_resize(client);
    }];
    return supported;
}

- (CGImageRef)copyFramebufferImage {
    __block CGImageRef image = NULL;
    [self withClient:^(struct vnc_client *client) {
        vnc_lock(client);
        int width, height;
        const uint32_t *fb = vnc_framebuffer(client, &width, &height);
        CFDataRef data = NULL;
        if (fb != NULL && width > 0 && height > 0)
            data = CFDataCreate(NULL, (const UInt8 *) fb, (CFIndex) width * height * 4);
        vnc_unlock(client);
        if (data == NULL)
            return;

        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        image = CGImageCreate(width, height, 8, 32, width * 4, colorSpace,
                              kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst,
                              provider, NULL, false, kCGRenderingIntentDefault);
        CGColorSpaceRelease(colorSpace);
        CGDataProviderRelease(provider);
        CFRelease(data);
    }];
    return image;
}

- (void)sendPointerX:(int)x y:(int)y buttons:(uint8_t)buttons {
    [self withClient:^(struct vnc_client *client) {
        vnc_send_pointer(client, x, y, buttons);
    }];
}

- (void)sendKeysym:(uint32_t)keysym down:(BOOL)down {
    [self withClient:^(struct vnc_client *client) {
        vnc_send_key(client, keysym, down);
    }];
}

- (void)sendClipboardText:(NSString *)text {
    NSData *data = [text dataUsingEncoding:NSISOLatin1StringEncoding allowLossyConversion:YES];
    [self withClient:^(struct vnc_client *client) {
        vnc_send_clipboard(client, data.bytes, data.length);
    }];
}

- (void)requestDesktopSizeWidth:(int)width height:(int)height {
    [self withClient:^(struct vnc_client *client) {
        vnc_request_desktop_size(client, width, height);
    }];
}

@end
