//
//  VNCClient.h
//  iSH
//
//  A small RFB (VNC) client, used to show the output of a graphical session
//  running inside iSH, e.g. a headless Wayland compositor exported with wayvnc.
//

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

NS_ASSUME_NONNULL_BEGIN

@class VNCClient;

// All delegate methods are called on the main thread.
@protocol VNCClientDelegate <NSObject>
- (void)vncClientDidConnect:(VNCClient *)client;
- (void)vncClientFramebufferDidChange:(VNCClient *)client;
- (void)vncClient:(VNCClient *)client didDisconnectWithError:(nullable NSString *)error;
@optional
- (void)vncClient:(VNCClient *)client didReceiveClipboardText:(NSString *)text;
@end

// Pointer button masks
#define VNC_BUTTON_LEFT (1 << 0)
#define VNC_BUTTON_MIDDLE (1 << 1)
#define VNC_BUTTON_RIGHT (1 << 2)
#define VNC_BUTTON_SCROLL_UP (1 << 3)
#define VNC_BUTTON_SCROLL_DOWN (1 << 4)
#define VNC_BUTTON_SCROLL_LEFT (1 << 5)
#define VNC_BUTTON_SCROLL_RIGHT (1 << 6)

@interface VNCClient : NSObject

- (instancetype)initWithHost:(NSString *)host port:(int)port password:(nullable NSString *)password;

@property (weak) id<VNCClientDelegate> delegate;
@property (readonly) NSString *host;
@property (readonly) int port;

// Connects on a background thread
- (void)connect;
- (void)disconnect;

@property (readonly) BOOL connected;
@property (readonly) int width;
@property (readonly) int height;
@property (readonly, nullable) NSString *desktopName;
// YES if the server supports the ExtendedDesktopSize extension, which means
// requestDesktopSize: will probably work
@property (readonly) BOOL supportsDesktopResize;

// Returns a snapshot of the framebuffer
- (nullable CGImageRef)copyFramebufferImage CF_RETURNS_RETAINED;

- (void)sendPointerX:(int)x y:(int)y buttons:(uint8_t)buttons;
- (void)sendKeysym:(uint32_t)keysym down:(BOOL)down;
- (void)sendClipboardText:(NSString *)text;
- (void)requestDesktopSizeWidth:(int)width height:(int)height;

@end

NS_ASSUME_NONNULL_END
