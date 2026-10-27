//
//  DisplayViewController.m
//  iSH
//

#import "DisplayViewController.h"
#import "VNCClient.h"

static NSString *const kDisplayHostKey = @"DisplayVNCHost";
static NSString *const kDisplayPortKey = @"DisplayVNCPort";
static NSString *const kDisplayPasswordKey = @"DisplayVNCPassword";
static NSString *const kDisplayAutoResizeKey = @"DisplayAutoResize";

// X11 keysyms
#define XK_BackSpace 0xff08
#define XK_Tab 0xff09
#define XK_Return 0xff0d
#define XK_Escape 0xff1b
#define XK_Delete 0xffff
#define XK_Home 0xff50
#define XK_Left 0xff51
#define XK_Up 0xff52
#define XK_Right 0xff53
#define XK_Down 0xff54
#define XK_Page_Up 0xff55
#define XK_Page_Down 0xff56
#define XK_End 0xff57
#define XK_Insert 0xff63
#define XK_F1 0xffbe
#define XK_Shift_L 0xffe1
#define XK_Shift_R 0xffe2
#define XK_Control_L 0xffe3
#define XK_Control_R 0xffe4
#define XK_Caps_Lock 0xffe5
#define XK_Alt_L 0xffe9
#define XK_Alt_R 0xffea
#define XK_Super_L 0xffeb
#define XK_Super_R 0xffec

typedef NS_OPTIONS(NSUInteger, StickyModifier) {
    StickyModifierControl = 1 << 0,
    StickyModifierAlt = 1 << 1,
    StickyModifierSuper = 1 << 2,
};

static uint32_t keysym_for_character(unichar c) {
    switch (c) {
        case '\n': case '\r': return XK_Return;
        case '\t': return XK_Tab;
        case '\b': return XK_BackSpace;
        case 0x1b: return XK_Escape;
    }
    if (c >= 0x20 && c <= 0x7e)
        return c;
    if (c >= 0xa0 && c <= 0xff)
        return c;
    return 0x01000000 | c;
}

#pragma mark - DisplayView

@interface DisplayView : UIView <UIKeyInput>
@property (weak) VNCClient *client;
@property (nonatomic) CGSize framebufferSize;
@property (nonatomic) StickyModifier stickyModifiers;
@property (copy) void (^stickyModifiersChanged)(void);
@property (nonatomic) UIKeyboardAppearance keyboardAppearance;
- (BOOL)handlePresses:(NSSet<UIPress *> *)presses down:(BOOL)down;
- (void)typeKeysym:(uint32_t)keysym;
@end

@implementation DisplayView {
    uint8_t _buttons;
    CGPoint _lastPoint;
    CGPoint _scrollRemainder;
    NSMutableDictionary<NSNumber *, NSNumber *> *_pressedKeys;
}

@synthesize keyboardAppearance;

- (instancetype)initWithFrame:(CGRect)frame {
    if (self = [super initWithFrame:frame]) {
        self.backgroundColor = UIColor.blackColor;
        self.layer.contentsGravity = kCAGravityResizeAspect;
        self.layer.magnificationFilter = kCAFilterLinear;
        self.multipleTouchEnabled = YES;
        _pressedKeys = [NSMutableDictionary new];

        UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tap:)];
        [self addGestureRecognizer:tap];

        UITapGestureRecognizer *twoFingerTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(rightClick:)];
        twoFingerTap.numberOfTouchesRequired = 2;
        [self addGestureRecognizer:twoFingerTap];

        UIPanGestureRecognizer *drag = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(drag:)];
        drag.maximumNumberOfTouches = 1;
        [self addGestureRecognizer:drag];

        UIPanGestureRecognizer *scroll = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(scroll:)];
        scroll.minimumNumberOfTouches = 2;
        scroll.maximumNumberOfTouches = 2;
        if (@available(iOS 13.4, *))
            scroll.allowedScrollTypesMask = UIScrollTypeMaskAll;
        [self addGestureRecognizer:scroll];

        if (@available(iOS 13.4, *)) {
            UITapGestureRecognizer *secondaryClick = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(rightClick:)];
            secondaryClick.buttonMaskRequired = UIEventButtonMaskSecondary;
            [self addGestureRecognizer:secondaryClick];
        }
        if (@available(iOS 13, *)) {
            UIHoverGestureRecognizer *hover = [[UIHoverGestureRecognizer alloc] initWithTarget:self action:@selector(hover:)];
            [self addGestureRecognizer:hover];
        }
    }
    return self;
}

// The part of the view the framebuffer is drawn into
- (CGRect)imageRect {
    CGSize fb = self.framebufferSize;
    CGRect bounds = self.bounds;
    if (fb.width <= 0 || fb.height <= 0 || bounds.size.width <= 0 || bounds.size.height <= 0)
        return bounds;
    CGFloat scale = MIN(bounds.size.width / fb.width, bounds.size.height / fb.height);
    CGSize size = CGSizeMake(fb.width * scale, fb.height * scale);
    return CGRectMake(bounds.origin.x + (bounds.size.width - size.width) / 2,
                      bounds.origin.y + (bounds.size.height - size.height) / 2,
                      size.width, size.height);
}

- (CGPoint)framebufferPointForPoint:(CGPoint)point {
    CGRect rect = [self imageRect];
    CGSize fb = self.framebufferSize;
    if (rect.size.width <= 0 || rect.size.height <= 0)
        return CGPointZero;
    CGFloat x = (point.x - rect.origin.x) / rect.size.width * fb.width;
    CGFloat y = (point.y - rect.origin.y) / rect.size.height * fb.height;
    x = MAX(0, MIN(fb.width - 1, x));
    y = MAX(0, MIN(fb.height - 1, y));
    return CGPointMake(x, y);
}

- (void)sendPointer:(CGPoint)point buttons:(uint8_t)buttons {
    _lastPoint = [self framebufferPointForPoint:point];
    [self.client sendPointerX:(int) _lastPoint.x y:(int) _lastPoint.y buttons:buttons];
}

- (void)clickAt:(CGPoint)point button:(uint8_t)button {
    [self sendPointer:point buttons:_buttons];
    [self sendPointer:point buttons:_buttons | button];
    [self sendPointer:point buttons:_buttons];
}

- (void)tap:(UITapGestureRecognizer *)recognizer {
    if (recognizer.state == UIGestureRecognizerStateEnded)
        [self clickAt:[recognizer locationInView:self] button:VNC_BUTTON_LEFT];
}

- (void)rightClick:(UITapGestureRecognizer *)recognizer {
    if (recognizer.state == UIGestureRecognizerStateEnded)
        [self clickAt:[recognizer locationInView:self] button:VNC_BUTTON_RIGHT];
}

- (void)drag:(UIPanGestureRecognizer *)recognizer {
    CGPoint point = [recognizer locationInView:self];
    switch (recognizer.state) {
        case UIGestureRecognizerStateBegan:
            // start at the point where the finger went down
            point = CGPointMake(point.x - [recognizer translationInView:self].x,
                                point.y - [recognizer translationInView:self].y);
            [self sendPointer:point buttons:_buttons];
            _buttons |= VNC_BUTTON_LEFT;
            [self sendPointer:point buttons:_buttons];
            break;
        case UIGestureRecognizerStateChanged:
            [self sendPointer:point buttons:_buttons];
            break;
        case UIGestureRecognizerStateEnded:
        case UIGestureRecognizerStateCancelled:
        case UIGestureRecognizerStateFailed:
            _buttons &= ~VNC_BUTTON_LEFT;
            [self sendPointer:point buttons:_buttons];
            break;
        default:
            break;
    }
}

- (void)scroll:(UIPanGestureRecognizer *)recognizer {
    if (recognizer.state == UIGestureRecognizerStateBegan) {
        _scrollRemainder = CGPointZero;
        _lastPoint = [self framebufferPointForPoint:[recognizer locationInView:self]];
    }
    if (recognizer.state != UIGestureRecognizerStateChanged)
        return;
    CGPoint translation = [recognizer translationInView:self];
    [recognizer setTranslation:CGPointZero inView:self];
    _scrollRemainder.x += translation.x;
    _scrollRemainder.y += translation.y;
    static const CGFloat step = 20;
    int x = (int) _lastPoint.x, y = (int) _lastPoint.y;
    // moving the fingers down scrolls up, like everywhere else on iOS
    while (_scrollRemainder.y >= step) {
        [self.client sendPointerX:x y:y buttons:_buttons | VNC_BUTTON_SCROLL_UP];
        [self.client sendPointerX:x y:y buttons:_buttons];
        _scrollRemainder.y -= step;
    }
    while (_scrollRemainder.y <= -step) {
        [self.client sendPointerX:x y:y buttons:_buttons | VNC_BUTTON_SCROLL_DOWN];
        [self.client sendPointerX:x y:y buttons:_buttons];
        _scrollRemainder.y += step;
    }
    while (_scrollRemainder.x >= step) {
        [self.client sendPointerX:x y:y buttons:_buttons | VNC_BUTTON_SCROLL_LEFT];
        [self.client sendPointerX:x y:y buttons:_buttons];
        _scrollRemainder.x -= step;
    }
    while (_scrollRemainder.x <= -step) {
        [self.client sendPointerX:x y:y buttons:_buttons | VNC_BUTTON_SCROLL_RIGHT];
        [self.client sendPointerX:x y:y buttons:_buttons];
        _scrollRemainder.x += step;
    }
}

- (void)hover:(UIGestureRecognizer *)recognizer {
    if (recognizer.state == UIGestureRecognizerStateBegan || recognizer.state == UIGestureRecognizerStateChanged)
        [self sendPointer:[recognizer locationInView:self] buttons:_buttons];
}

#pragma mark Keyboard

- (BOOL)canBecomeFirstResponder {
    return YES;
}

- (void)setStickyModifiers:(StickyModifier)stickyModifiers {
    _stickyModifiers = stickyModifiers;
    if (self.stickyModifiersChanged)
        self.stickyModifiersChanged();
}

- (void)sendStickyModifiersDown:(BOOL)down {
    StickyModifier mods = self.stickyModifiers;
    if (mods & StickyModifierControl)
        [self.client sendKeysym:XK_Control_L down:down];
    if (mods & StickyModifierAlt)
        [self.client sendKeysym:XK_Alt_L down:down];
    if (mods & StickyModifierSuper)
        [self.client sendKeysym:XK_Super_L down:down];
}

- (void)typeKeysym:(uint32_t)keysym {
    [self sendStickyModifiersDown:YES];
    [self.client sendKeysym:keysym down:YES];
    [self.client sendKeysym:keysym down:NO];
    [self sendStickyModifiersDown:NO];
    if (self.stickyModifiers != 0)
        self.stickyModifiers = 0;
}

- (BOOL)hasText {
    return YES;
}

- (void)insertText:(NSString *)text {
    for (NSUInteger i = 0; i < text.length; i++)
        [self typeKeysym:keysym_for_character([text characterAtIndex:i])];
}

- (void)deleteBackward {
    [self typeKeysym:XK_BackSpace];
}

- (UITextAutocorrectionType)autocorrectionType {
    return UITextAutocorrectionTypeNo;
}
- (UITextAutocapitalizationType)autocapitalizationType {
    return UITextAutocapitalizationTypeNone;
}
- (UITextSpellCheckingType)spellCheckingType {
    return UITextSpellCheckingTypeNo;
}
- (UITextSmartQuotesType)smartQuotesType {
    return UITextSmartQuotesTypeNo;
}
- (UITextSmartDashesType)smartDashesType {
    return UITextSmartDashesTypeNo;
}
- (UITextSmartInsertDeleteType)smartInsertDeleteType {
    return UITextSmartInsertDeleteTypeNo;
}
- (UIKeyboardType)keyboardType {
    return UIKeyboardTypeASCIICapable;
}

static uint32_t keysym_for_hid_usage(long usage) API_AVAILABLE(ios(13.4));
static uint32_t keysym_for_hid_usage(long usage) {
    switch (usage) {
        case UIKeyboardHIDUsageKeyboardReturnOrEnter:
        case UIKeyboardHIDUsageKeypadEnter: return XK_Return;
        case UIKeyboardHIDUsageKeyboardEscape: return XK_Escape;
        case UIKeyboardHIDUsageKeyboardDeleteOrBackspace: return XK_BackSpace;
        case UIKeyboardHIDUsageKeyboardTab: return XK_Tab;
        case UIKeyboardHIDUsageKeyboardRightArrow: return XK_Right;
        case UIKeyboardHIDUsageKeyboardLeftArrow: return XK_Left;
        case UIKeyboardHIDUsageKeyboardDownArrow: return XK_Down;
        case UIKeyboardHIDUsageKeyboardUpArrow: return XK_Up;
        case UIKeyboardHIDUsageKeyboardHome: return XK_Home;
        case UIKeyboardHIDUsageKeyboardEnd: return XK_End;
        case UIKeyboardHIDUsageKeyboardPageUp: return XK_Page_Up;
        case UIKeyboardHIDUsageKeyboardPageDown: return XK_Page_Down;
        case UIKeyboardHIDUsageKeyboardDeleteForward: return XK_Delete;
        case UIKeyboardHIDUsageKeyboardInsert: return XK_Insert;
        case UIKeyboardHIDUsageKeyboardLeftShift: return XK_Shift_L;
        case UIKeyboardHIDUsageKeyboardRightShift: return XK_Shift_R;
        case UIKeyboardHIDUsageKeyboardLeftControl: return XK_Control_L;
        case UIKeyboardHIDUsageKeyboardRightControl: return XK_Control_R;
        case UIKeyboardHIDUsageKeyboardLeftAlt: return XK_Alt_L;
        case UIKeyboardHIDUsageKeyboardRightAlt: return XK_Alt_R;
        case UIKeyboardHIDUsageKeyboardLeftGUI: return XK_Super_L;
        case UIKeyboardHIDUsageKeyboardRightGUI: return XK_Super_R;
        case UIKeyboardHIDUsageKeyboardCapsLock: return XK_Caps_Lock;
    }
    if (usage >= UIKeyboardHIDUsageKeyboardF1 && usage <= UIKeyboardHIDUsageKeyboardF12)
        return XK_F1 + (uint32_t) (usage - UIKeyboardHIDUsageKeyboardF1);
    return 0;
}

// Handles hardware keyboard events. Returns NO if some of the presses
// weren't keyboard presses.
- (BOOL)handlePresses:(NSSet<UIPress *> *)presses down:(BOOL)down {
    if (@available(iOS 13.4, *)) {
        BOOL handledAll = YES;
        for (UIPress *press in presses) {
            UIKey *key = press.key;
            if (key == nil) {
                handledAll = NO;
                continue;
            }
            NSNumber *code = @(key.keyCode);
            if (!down) {
                NSNumber *keysym = _pressedKeys[code];
                if (keysym != nil) {
                    [self.client sendKeysym:keysym.unsignedIntValue down:NO];
                    [_pressedKeys removeObjectForKey:code];
                }
                continue;
            }
            uint32_t keysym = keysym_for_hid_usage(key.keyCode);
            if (keysym == 0) {
                // With command/control/alt the characters are often useless
                // (e.g. control characters), so use the unmodified ones
                BOOL modified = key.modifierFlags & (UIKeyModifierControl | UIKeyModifierAlternate | UIKeyModifierCommand);
                NSString *chars = modified ? key.charactersIgnoringModifiers : key.characters;
                if (chars.length == 0)
                    chars = key.charactersIgnoringModifiers;
                if (chars.length == 0)
                    continue;
                keysym = keysym_for_character([chars characterAtIndex:0]);
            }
            _pressedKeys[code] = @(keysym);
            [self.client sendKeysym:keysym down:YES];
        }
        return handledAll;
    }
    return NO;
}

- (void)releaseAllKeys {
    for (NSNumber *keysym in _pressedKeys.allValues)
        [self.client sendKeysym:keysym.unsignedIntValue down:NO];
    [_pressedKeys removeAllObjects];
}

- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (![self handlePresses:presses down:YES])
        [super pressesBegan:presses withEvent:event];
}

- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (![self handlePresses:presses down:NO])
        [super pressesEnded:presses withEvent:event];
}

- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (![self handlePresses:presses down:NO])
        [super pressesCancelled:presses withEvent:event];
}

@end

#pragma mark - DisplayViewController

@interface DisplayViewController () <VNCClientDelegate>
@property DisplayView *displayView;
@property UIStackView *toolbar;
@property UIView *toolbarBackground;
@property UITextView *statusView;
@property UIButton *controlButton;
@property UIButton *altButton;
@property UIButton *superButton;
@property NSLayoutConstraint *bottomConstraint;

@property (nullable) VNCClient *client;
@property BOOL visible;
@property (nullable) NSTimer *retryTimer;
@property CGSize requestedSize;
@property (nullable) NSString *lastError;
@end

@implementation DisplayViewController

- (instancetype)init {
    if (self = [super initWithNibName:nil bundle:nil]) {
        self.modalPresentationStyle = UIModalPresentationFullScreen;
        [NSUserDefaults.standardUserDefaults registerDefaults:@{
            kDisplayHostKey: @"127.0.0.1",
            kDisplayPortKey: @5900,
            kDisplayAutoResizeKey: @YES,
        }];
    }
    return self;
}

- (UIButton *)toolbarButtonWithSymbol:(NSString *)symbol title:(NSString *)title action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    if (@available(iOS 13, *)) {
        UIImage *image = symbol ? [UIImage systemImageNamed:symbol] : nil;
        if (image != nil)
            [button setImage:image forState:UIControlStateNormal];
        else
            [button setTitle:title forState:UIControlStateNormal];
    } else {
        [button setTitle:title forState:UIControlStateNormal];
    }
    button.accessibilityLabel = title;
    button.tintColor = UIColor.whiteColor;
    button.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    [button.widthAnchor constraintGreaterThanOrEqualToConstant:36].active = YES;
    return button;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;

    self.toolbarBackground = [UIView new];
    self.toolbarBackground.backgroundColor = [UIColor colorWithWhite:0.12 alpha:1];
    self.toolbarBackground.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:self.toolbarBackground];

    self.controlButton = [self toolbarButtonWithSymbol:nil title:@"Ctrl" action:@selector(toggleControl:)];
    self.altButton = [self toolbarButtonWithSymbol:nil title:@"Alt" action:@selector(toggleAlt:)];
    self.superButton = [self toolbarButtonWithSymbol:nil title:@"Super" action:@selector(toggleSuper:)];
    NSArray<UIView *> *buttons = @[
        [self toolbarButtonWithSymbol:@"xmark" title:@"Close" action:@selector(close:)],
        [self toolbarButtonWithSymbol:@"keyboard" title:@"Keyboard" action:@selector(toggleKeyboard:)],
        [self toolbarButtonWithSymbol:@"escape" title:@"Esc" action:@selector(pressEscape:)],
        [self toolbarButtonWithSymbol:@"arrow.right.to.line.alt" title:@"Tab" action:@selector(pressTab:)],
        self.controlButton,
        self.altButton,
        self.superButton,
        [self toolbarButtonWithSymbol:@"doc.on.clipboard" title:@"Send Clipboard" action:@selector(sendClipboard:)],
        [self toolbarButtonWithSymbol:@"gear" title:@"Settings" action:@selector(showSettings:)],
    ];
    self.toolbar = [[UIStackView alloc] initWithArrangedSubviews:buttons];
    self.toolbar.axis = UILayoutConstraintAxisHorizontal;
    self.toolbar.distribution = UIStackViewDistributionEqualSpacing;
    self.toolbar.alignment = UIStackViewAlignmentCenter;
    self.toolbar.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:self.toolbar];

    self.displayView = [DisplayView new];
    self.displayView.translatesAutoresizingMaskIntoConstraints = NO;
    self.displayView.keyboardAppearance = UIKeyboardAppearanceDark;
    __weak typeof(self) weakSelf = self;
    self.displayView.stickyModifiersChanged = ^{
        [weakSelf updateStickyButtons];
    };
    [self.view addSubview:self.displayView];

    self.statusView = [UITextView new];
    self.statusView.editable = NO;
    self.statusView.scrollEnabled = YES;
    self.statusView.backgroundColor = UIColor.clearColor;
    self.statusView.textColor = UIColor.lightGrayColor;
    self.statusView.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightRegular];
    self.statusView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:self.statusView];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    self.bottomConstraint = [self.displayView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor];
    [NSLayoutConstraint activateConstraints:@[
        [self.toolbarBackground.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.toolbarBackground.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.toolbarBackground.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.toolbarBackground.bottomAnchor constraintEqualToAnchor:self.toolbar.bottomAnchor],

        [self.toolbar.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [self.toolbar.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:8],
        [self.toolbar.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-8],
        [self.toolbar.heightAnchor constraintEqualToConstant:40],

        [self.displayView.topAnchor constraintEqualToAnchor:self.toolbar.bottomAnchor],
        [self.displayView.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [self.displayView.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        self.bottomConstraint,

        [self.statusView.topAnchor constraintEqualToAnchor:self.displayView.topAnchor constant:16],
        [self.statusView.bottomAnchor constraintEqualToAnchor:self.displayView.bottomAnchor constant:-16],
        [self.statusView.leadingAnchor constraintEqualToAnchor:self.displayView.leadingAnchor constant:16],
        [self.statusView.trailingAnchor constraintEqualToAnchor:self.displayView.trailingAnchor constant:-16],
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(keyboardWillChange:)
                                               name:UIKeyboardWillChangeFrameNotification
                                             object:nil];
    [self showStatus:@"Connecting…"];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    self.visible = YES;
    [self becomeFirstResponder];
    [self connect];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    self.visible = NO;
    [self.retryTimer invalidate];
    self.retryTimer = nil;
    [self.displayView releaseAllKeys];
    [self.client disconnect];
    self.client.delegate = nil;
    self.client = nil;
}

- (BOOL)prefersStatusBarHidden {
    return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
    return YES;
}

- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures {
    return UIRectEdgeAll;
}

#pragma mark Connection

- (void)connect {
    if (!self.visible || self.client != nil)
        return;
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSString *host = [defaults stringForKey:kDisplayHostKey];
    int port = (int) [defaults integerForKey:kDisplayPortKey];
    NSString *password = [defaults stringForKey:kDisplayPasswordKey];
    self.client = [[VNCClient alloc] initWithHost:host port:port password:password];
    self.client.delegate = self;
    self.displayView.client = self.client;
    self.requestedSize = CGSizeZero;
    [self.client connect];
}

- (void)scheduleRetry {
    [self.retryTimer invalidate];
    if (!self.visible)
        return;
    __weak typeof(self) weakSelf = self;
    self.retryTimer = [NSTimer scheduledTimerWithTimeInterval:2 repeats:NO block:^(NSTimer *timer) {
        [weakSelf connect];
    }];
}

- (void)showStatus:(NSString *)status {
    self.statusView.text = status;
    self.statusView.hidden = NO;
}

- (NSString *)helpText {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    return [NSString stringWithFormat:
            @"Waiting for a VNC server on %@:%ld…\n"
            @"\n"
            @"Start a Wayland compositor with a VNC server inside iSH, for example:\n"
            @"\n"
            @"apk add sway wayvnc foot\n"
            @"export XDG_RUNTIME_DIR=/tmp/xdg\n"
            @"mkdir -p -m 700 $XDG_RUNTIME_DIR\n"
            @"WLR_BACKENDS=headless WLR_RENDERER=pixman \\\n"
            @"  WLR_LIBINPUT_NO_DEVICES=1 sway &\n"
            @"WAYLAND_DISPLAY=wayland-1 wayvnc 127.0.0.1 5900 &\n"
            @"\n"
            @"Apps started with WAYLAND_DISPLAY=wayland-1 (or forwarded with waypipe) then show up here.\n"
            @"\n"
            @"Touch: tap = click, drag = drag with the left button, two finger tap = right click, two finger drag = scroll.",
            [defaults stringForKey:kDisplayHostKey], (long) [defaults integerForKey:kDisplayPortKey]];
}

- (void)vncClientDidConnect:(VNCClient *)client {
    if (client != self.client)
        return;
    self.lastError = nil;
    [self showStatus:@"Connected, waiting for the first frame…"];
}

- (void)vncClientFramebufferDidChange:(VNCClient *)client {
    if (client != self.client)
        return;
    CGImageRef image = [client copyFramebufferImage];
    if (image == NULL)
        return;
    self.displayView.framebufferSize = CGSizeMake(CGImageGetWidth(image), CGImageGetHeight(image));
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self.displayView.layer.contents = (__bridge id) image;
    [CATransaction commit];
    CGImageRelease(image);
    self.statusView.hidden = YES;
    [self resizeDesktopIfNeeded];
}

- (void)vncClient:(VNCClient *)client didDisconnectWithError:(NSString *)error {
    if (client != self.client)
        return;
    self.client = nil;
    self.displayView.client = nil;
    self.displayView.layer.contents = nil;
    self.lastError = error;
    NSString *status = self.helpText;
    if (error != nil)
        status = [NSString stringWithFormat:@"%@\n\n%@", error, status];
    [self showStatus:status];
    [self scheduleRetry];
}

- (void)vncClient:(VNCClient *)client didReceiveClipboardText:(NSString *)text {
    UIPasteboard.generalPasteboard.string = text;
}

// Ask the server to make the desktop the same size as the view, if it
// supports that (wayvnc does for headless outputs)
- (void)resizeDesktopIfNeeded {
    if (![NSUserDefaults.standardUserDefaults boolForKey:kDisplayAutoResizeKey])
        return;
    VNCClient *client = self.client;
    if (client == nil || !client.supportsDesktopResize)
        return;
    CGSize size = self.displayView.bounds.size;
    int width = ((int) size.width) & ~1;
    int height = ((int) size.height) & ~1;
    if (width < 64 || height < 64)
        return;
    CGSize wanted = CGSizeMake(width, height);
    if (CGSizeEqualToSize(wanted, self.requestedSize))
        return;
    self.requestedSize = wanted;
    if (client.width == width && client.height == height)
        return;
    [client requestDesktopSizeWidth:width height:height];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [self resizeDesktopIfNeeded];
}

#pragma mark Keyboard

- (BOOL)canBecomeFirstResponder {
    return YES;
}

- (void)keyboardWillChange:(NSNotification *)notification {
    CGRect frame = [notification.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    frame = [self.view convertRect:frame fromView:nil];
    CGFloat overlap = MAX(0, CGRectGetMaxY(self.view.bounds) - CGRectGetMinY(frame));
    // a hardware keyboard only shows a small bar, if anything
    self.bottomConstraint.constant = -overlap;
    NSTimeInterval duration = [notification.userInfo[UIKeyboardAnimationDurationUserInfoKey] doubleValue];
    [UIView animateWithDuration:duration animations:^{
        [self.view layoutIfNeeded];
    }];
}

- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (![self.displayView handlePresses:presses down:YES])
        [super pressesBegan:presses withEvent:event];
}

- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (![self.displayView handlePresses:presses down:NO])
        [super pressesEnded:presses withEvent:event];
}

- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (![self.displayView handlePresses:presses down:NO])
        [super pressesCancelled:presses withEvent:event];
}

#pragma mark Toolbar

- (void)close:(id)sender {
    [self.displayView resignFirstResponder];
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)toggleKeyboard:(id)sender {
    if (self.displayView.isFirstResponder) {
        [self.displayView resignFirstResponder];
        [self becomeFirstResponder];
    } else {
        [self.displayView becomeFirstResponder];
    }
}

- (void)pressEscape:(id)sender {
    [self.displayView typeKeysym:XK_Escape];
}

- (void)pressTab:(id)sender {
    [self.displayView typeKeysym:XK_Tab];
}

- (void)toggleModifier:(StickyModifier)modifier {
    self.displayView.stickyModifiers ^= modifier;
}

- (void)toggleControl:(id)sender {
    [self toggleModifier:StickyModifierControl];
}

- (void)toggleAlt:(id)sender {
    [self toggleModifier:StickyModifierAlt];
}

- (void)toggleSuper:(id)sender {
    [self toggleModifier:StickyModifierSuper];
}

- (void)updateStickyButtons {
    StickyModifier mods = self.displayView.stickyModifiers;
    UIColor *on = UIColor.systemBlueColor, *off = UIColor.whiteColor;
    self.controlButton.tintColor = mods & StickyModifierControl ? on : off;
    self.altButton.tintColor = mods & StickyModifierAlt ? on : off;
    self.superButton.tintColor = mods & StickyModifierSuper ? on : off;
}

- (void)sendClipboard:(id)sender {
    NSString *text = UIPasteboard.generalPasteboard.string;
    if (text != nil)
        [self.client sendClipboardText:text];
}

- (void)showSettings:(id)sender {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"VNC Server"
                                                                   message:@"The display connects to a VNC server, e.g. wayvnc running inside iSH."
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.placeholder = @"Host";
        field.text = [defaults stringForKey:kDisplayHostKey];
        field.autocorrectionType = UITextAutocorrectionTypeNo;
        field.autocapitalizationType = UITextAutocapitalizationTypeNone;
        field.keyboardType = UIKeyboardTypeURL;
    }];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.placeholder = @"Port";
        field.text = [NSString stringWithFormat:@"%ld", (long) [defaults integerForKey:kDisplayPortKey]];
        field.keyboardType = UIKeyboardTypeNumberPad;
    }];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.placeholder = @"Password (optional)";
        field.text = [defaults stringForKey:kDisplayPasswordKey];
        field.secureTextEntry = YES;
    }];
    BOOL autoResize = [defaults boolForKey:kDisplayAutoResizeKey];
    __weak typeof(self) weakSelf = self;
    void (^save)(void) = ^{
        NSString *host = [alert.textFields[0].text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        int port = alert.textFields[1].text.intValue;
        [defaults setObject:host.length > 0 ? host : @"127.0.0.1" forKey:kDisplayHostKey];
        [defaults setInteger:port > 0 && port < 65536 ? port : 5900 forKey:kDisplayPortKey];
        [defaults setObject:alert.textFields[2].text forKey:kDisplayPasswordKey];
    };
    [alert addAction:[UIAlertAction actionWithTitle:@"Connect" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        save();
        [weakSelf reconnect];
    }]];
    NSString *resizeTitle = autoResize ? @"Don't Resize Desktop to Fit" : @"Resize Desktop to Fit";
    [alert addAction:[UIAlertAction actionWithTitle:resizeTitle style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        [defaults setBool:!autoResize forKey:kDisplayAutoResizeKey];
        weakSelf.requestedSize = CGSizeZero;
        [weakSelf resizeDesktopIfNeeded];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)reconnect {
    [self.retryTimer invalidate];
    self.retryTimer = nil;
    VNCClient *client = self.client;
    self.client = nil;
    client.delegate = nil;
    [client disconnect];
    self.displayView.layer.contents = nil;
    [self showStatus:@"Connecting…"];
    [self connect];
}

@end
