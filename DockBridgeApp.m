#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <dispatch/dispatch.h>
#include <dlfcn.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "bridge_ipc.h"

extern void dockbridge_app_audio_update(const db_reply *reply, BOOL available);
extern void dockbridge_app_audio_backgrounded(void);

static BOOL exchange(uint8_t command, db_reply *reply) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NO;
    int ignore = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &ignore, sizeof(ignore));
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    strcpy(address.sun_path, DB_SOCKET_PATH);
    address.sun_len = SUN_LEN(&address);
    BOOL success = NO;
    if (!connect(fd, (struct sockaddr *)&address, address.sun_len) && write(fd, &command, 1) == 1) {
        struct pollfd response = { .fd = fd, .events = POLLIN };
        size_t received = 0;
        while (received < sizeof(*reply) && poll(&response, 1, 800) > 0) {
            ssize_t count = read(fd, (uint8_t *)reply + received, sizeof(*reply) - received);
            if (count <= 0) break;
            received += (size_t)count;
        }
        success = received == sizeof(*reply) && reply->phase <= DB_FAILED;
    }
    close(fd);
    return success;
}

// Query the legacy settings service when the private API is available.
static BOOL nativeBluetoothIsOff(void) {
    static BOOL checked;
    if (!checked) {
        checked = YES;
        dlopen("/System/Library/PrivateFrameworks/BluetoothManager.framework/BluetoothManager", RTLD_LAZY);
    }
    Class managerClass = NSClassFromString(@"BluetoothManager");
    if (![managerClass respondsToSelector:@selector(sharedInstance)]) return NO;
    id manager = [managerClass performSelector:@selector(sharedInstance)];
    SEL enabled = NSSelectorFromString(@"enabled");
    SEL powered = NSSelectorFromString(@"powered");
    if (![manager respondsToSelector:enabled] || ![manager respondsToSelector:powered]) return NO;
    return !((BOOL (*)(id, SEL))[manager methodForSelector:enabled])(manager, enabled) &&
           !((BOOL (*)(id, SEL))[manager methodForSelector:powered])(manager, powered);
}

@interface DockBridgeArtwork : UIView
@end

@implementation DockBridgeArtwork
- (void)drawRect:(CGRect)rect {
    CGContextRef context = UIGraphicsGetCurrentContext();
    CGContextSaveGState(context);
    CGContextScaleCTM(context, self.bounds.size.width, self.bounds.size.height);
    const CGFloat colors[] = { 0.07, 0.35, 0.40, 1, 0.055, 0.16, 0.28, 1 };
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGGradientRef gradient = CGGradientCreateWithColorComponents(space, colors, NULL, 2);
    CGContextDrawLinearGradient(context, gradient, CGPointMake(0, 0), CGPointMake(1, 1), 0);
    CGGradientRelease(gradient);
    CGColorSpaceRelease(space);

    CGContextSetRGBFillColor(context, 0.02, 0.11, 0.19, 0.64);
    CGContextFillEllipseInRect(context, CGRectMake(0.15, 0.15, 0.70, 0.70));
    CGContextSetRGBStrokeColor(context, 0.43, 0.85, 0.82, 0.28);
    CGContextSetLineWidth(context, 0.004);
    for (CGFloat radius = 0.23; radius <= 0.35; radius += 0.04) {
        CGContextStrokeEllipseInRect(context, CGRectMake(0.5 - radius, 0.5 - radius, radius * 2, radius * 2));
    }
    CGContextSetRGBFillColor(context, 0.54, 0.90, 0.87, 1);
    CGContextFillEllipseInRect(context, CGRectMake(0.397, 0.55, 0.124, 0.094));
    CGContextSetRGBStrokeColor(context, 0.54, 0.90, 0.87, 1);
    CGContextSetLineWidth(context, 0.03);
    CGContextSetLineCap(context, kCGLineCapRound);
    CGContextMoveToPoint(context, 0.505, 0.593);
    CGContextAddLineToPoint(context, 0.505, 0.366);
    CGContextAddCurveToPoint(context, 0.55, 0.404, 0.60, 0.398, 0.618, 0.455);
    CGContextStrokePath(context);
    CGContextRestoreGState(context);
}
@end

@interface DockBridgeController : UIViewController <UIAlertViewDelegate> {
    UILabel *statusLabel, *detailLabel, *deviceLabel;
    UILabel *trackTitle, *trackArtist, *trackAlbum, *trackTime, *trackFormat;
    UIView *settingsPanel, *playerPanel, *mediaCard;
    UIProgressView *trackProgress;
    UIButton *startButton, *stopButton, *pairButton;
    UISwitch *autoSwitch;
    NSTimer *timer;
    BOOL pending;
    BOOL playerAvailable, settingsRequested;
    uint8_t queued;
    uint8_t autoMode, lastPhase;
}
@end

@implementation DockBridgeController

- (UILabel *)labelAt:(CGRect)rect size:(CGFloat)size bold:(BOOL)bold {
    UILabel *label = [[[UILabel alloc] initWithFrame:rect] autorelease];
    label.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    label.backgroundColor = [UIColor clearColor];
    label.textColor = [UIColor colorWithRed:0.10 green:0.19 blue:0.27 alpha:1.0];
    label.font = bold ? [UIFont boldSystemFontOfSize:size] : [UIFont systemFontOfSize:size];
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 0;
    [settingsPanel addSubview:label];
    return label;
}

- (UIButton *)buttonAt:(CGFloat)y title:(NSString *)title action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeRoundedRect];
    button.frame = CGRectMake(22, y, self.view.bounds.size.width - 44, 39);
    button.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [button setTitle:title forState:UIControlStateNormal];
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    [settingsPanel addSubview:button];
    return button;
}

- (UILabel *)mediaLabelWithSize:(CGFloat)size bold:(BOOL)bold {
    UILabel *label = [[[UILabel alloc] initWithFrame:CGRectZero] autorelease];
    label.backgroundColor = [UIColor clearColor];
    label.font = bold ? [UIFont boldSystemFontOfSize:size] : [UIFont systemFontOfSize:size];
    label.textColor = bold ? [UIColor whiteColor] : [UIColor colorWithRed:0.66 green:0.81 blue:0.85 alpha:1.0];
    label.textAlignment = NSTextAlignmentCenter;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    [mediaCard addSubview:label];
    return label;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"DockBridge 4S";
    self.view.backgroundColor = [UIColor colorWithRed:0.92 green:0.95 blue:0.96 alpha:1.0];
    CGFloat width = self.view.bounds.size.width;
    settingsPanel = [[[UIView alloc] initWithFrame:self.view.bounds] autorelease];
    settingsPanel.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:settingsPanel];
    statusLabel = [self labelAt:CGRectMake(20, 7, width - 40, 34) size:23 bold:YES];
    detailLabel = [self labelAt:CGRectMake(20, 43, width - 40, 38) size:14 bold:NO];
    statusLabel.text = @"正在连接服务";
    deviceLabel = [self labelAt:CGRectMake(20, 82, width - 40, 27) size:15 bold:YES];
    deviceLabel.numberOfLines = 1;
    deviceLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    deviceLabel.text = @"蓝牙设备 · 未连接";
    startButton = [self buttonAt:112 title:@"开启蓝牙接收" action:@selector(startReceiver)];
    stopButton = [self buttonAt:154 title:@"停止接收 · 切回系统" action:@selector(stopReceiver)];
    pairButton = [self buttonAt:196 title:@"重新开放配对" action:@selector(openPairing)];
    autoSwitch = [[[UISwitch alloc] initWithFrame:CGRectZero] autorelease];
    [autoSwitch sizeToFit];
    CGRect toggle = autoSwitch.frame;
    toggle.origin = CGPointMake(width - 22 - toggle.size.width, 240);
    autoSwitch.frame = toggle;
    autoSwitch.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin;
    autoSwitch.accessibilityLabel = @"开机自动开启蓝牙接收";
    autoSwitch.enabled = NO;
    [autoSwitch addTarget:self action:@selector(autoChanged) forControlEvents:UIControlEventValueChanged];
    [settingsPanel addSubview:autoSwitch];
    UILabel *autoLabel = [self labelAt:CGRectMake(22, 238, toggle.origin.x - 30, 33) size:14 bold:NO];
    autoLabel.text = @"开机自动开启蓝牙接收";
    autoLabel.textAlignment = NSTextAlignmentLeft;
    UILabel *noteLabel = [self labelAt:CGRectMake(22, 283, width - 44, 60) size:13 bold:NO];
    noteLabel.text = @"保持 iPhone 插在 Bose 底座上。\n安卓从系统蓝牙选择 DockBridge 4S。";
    playerPanel = [[[UIView alloc] initWithFrame:self.view.bounds] autorelease];
    playerPanel.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    playerPanel.backgroundColor = [UIColor colorWithRed:0.04 green:0.10 blue:0.17 alpha:1.0];
    playerPanel.hidden = YES;
    [self.view addSubview:playerPanel];
    DockBridgeArtwork *artwork = [[[DockBridgeArtwork alloc] initWithFrame:CGRectMake((width - 170) / 2, 24, 170, 170)] autorelease];
    artwork.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
    artwork.layer.cornerRadius = 18;
    artwork.layer.masksToBounds = YES;
    [playerPanel addSubview:artwork];
    mediaCard = [[[UIView alloc] initWithFrame:CGRectMake(22, 210, width - 44, 170)] autorelease];
    mediaCard.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [playerPanel addSubview:mediaCard];
    UILabel *heading = [self mediaLabelWithSize:13 bold:NO];
    heading.frame = CGRectMake(12, 0, width - 68, 19);
    heading.text = @"正在播放";
    trackTitle = [self mediaLabelWithSize:23 bold:YES];
    trackTitle.numberOfLines = 2;
    trackArtist = [self mediaLabelWithSize:15 bold:NO];
    trackAlbum = [self mediaLabelWithSize:13 bold:NO];
    trackTime = [self mediaLabelWithSize:12 bold:NO];
    trackFormat = [self mediaLabelWithSize:12 bold:NO];
    trackProgress = [[[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault] autorelease];
    trackProgress.progressTintColor = [UIColor colorWithRed:0.40 green:0.85 blue:0.80 alpha:1.0];
    trackProgress.trackTintColor = [UIColor colorWithRed:0.17 green:0.29 blue:0.35 alpha:1.0];
    [mediaCard addSubview:trackProgress];
    startButton.enabled = stopButton.enabled = pairButton.enabled = NO;
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self refreshStatus];
    if (!timer) timer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                            selector:@selector(refreshStatus) userInfo:nil repeats:YES];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [timer invalidate];
    timer = nil;
}

static NSString *mediaString(const char *value, size_t capacity) {
    size_t length = strnlen(value, capacity);
    NSString *text = [[[NSString alloc] initWithBytes:value length:length
                                            encoding:NSUTF8StringEncoding] autorelease];
    return [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

static NSString *mediaTime(uint32_t ms) {
    unsigned seconds = ms / 1000;
    return [NSString stringWithFormat:@"%u:%02u", seconds / 60, seconds % 60];
}

- (CGFloat)layoutMediaLabel:(UILabel *)label text:(NSString *)text top:(CGFloat)top height:(CGFloat)height {
    label.text = text;
    label.hidden = text.length == 0;
    if (label.hidden) return top;
    label.frame = CGRectMake(12, top, mediaCard.bounds.size.width - 24, height);
    return top + height;
}

- (void)renderMedia:(const db_media_snapshot *)media visible:(BOOL)visible {
    mediaCard.hidden = !visible || !media->sample_rate || media->codec != DB_CODEC_SBC;
    if (mediaCard.hidden) return;
    NSString *title = mediaString(media->title, sizeof(media->title));
    NSString *artist = mediaString(media->artist, sizeof(media->artist));
    NSString *album = mediaString(media->album, sizeof(media->album));
    BOOL hasTrack = title.length || artist.length || album.length;
    BOOL hasPosition = hasTrack && (media->timing & DB_HAS_POSITION);
    BOOL hasDuration = hasTrack && (media->timing & DB_HAS_DURATION) && media->duration_ms;
    CGFloat y = 24;
    CGSize titleSize = [title sizeWithFont:trackTitle.font
                         constrainedToSize:CGSizeMake(mediaCard.bounds.size.width - 24, 56)
                              lineBreakMode:NSLineBreakByWordWrapping];
    y = [self layoutMediaLabel:trackTitle text:title top:y height:MAX(29, MIN(56, ceil(titleSize.height)))];
    y = [self layoutMediaLabel:trackArtist text:artist top:y height:22];
    y = [self layoutMediaLabel:trackAlbum text:album.length ? [@"专辑 · " stringByAppendingString:album] : nil top:y height:19];
    trackProgress.hidden = !hasPosition || !hasDuration;
    if (!trackProgress.hidden) {
        trackProgress.frame = CGRectMake(12, y + 7, mediaCard.bounds.size.width - 24, 9);
        trackProgress.progress = MIN(1.0f, (float)media->position_ms / media->duration_ms);
        y += 16;
    }
    NSString *timing = nil;
    if (hasPosition && hasDuration) {
        timing = [NSString stringWithFormat:@"%@ / %@", mediaTime(media->position_ms), mediaTime(media->duration_ms)];
    } else if (hasPosition) {
        timing = mediaTime(media->position_ms);
    } else if (hasDuration) {
        timing = [@"时长  " stringByAppendingString:mediaTime(media->duration_ms)];
    }
    y = [self layoutMediaLabel:trackTime text:timing top:y height:16];
    NSString *rate = media->sample_rate % 1000
        ? [NSString stringWithFormat:@"%.1f kHz", media->sample_rate / 1000.0]
        : [NSString stringWithFormat:@"%u kHz", (unsigned)(media->sample_rate / 1000)];
    NSString *channels = media->channels == 2 ? @"双声道" : @"单声道";
    NSString *format = [NSString stringWithFormat:@"SBC · %@ · %@", rate, channels];
    y = [self layoutMediaLabel:trackFormat text:format top:y + (hasTrack ? 3 : 0) height:19];
    CGRect frame = mediaCard.frame;
    frame.size.height = y + 8;
    mediaCard.frame = frame;
}

- (void)showCurrentPanel {
    BOOL playingView = playerAvailable && !settingsRequested;
    settingsPanel.hidden = playingView;
    playerPanel.hidden = !playingView;
    NSString *buttonTitle = playerAvailable ? (playingView ? @"设置" : @"返回播放") : nil;
    if (!buttonTitle) {
        self.navigationItem.rightBarButtonItem = nil;
    } else if (![self.navigationItem.rightBarButtonItem.title isEqualToString:buttonTitle]) {
        self.navigationItem.rightBarButtonItem = [[[UIBarButtonItem alloc] initWithTitle:buttonTitle
            style:UIBarButtonItemStylePlain target:self action:@selector(togglePlayerSettings)] autorelease];
    }
}

- (void)togglePlayerSettings {
    settingsRequested = !settingsRequested;
    [self showCurrentPanel];
}

- (void)render:(db_reply)reply available:(BOOL)available {
    dockbridge_app_audio_update(&reply, available);
    NSString *device = @"未连接";
    if (!available) device = @"状态未知";
    else if (reply.phase == DB_RECEIVING && reply.media.device_connected) {
        NSString *name = mediaString(reply.media.device_name, sizeof(reply.media.device_name));
        device = name.length ? name : @"已连接（名称未获取）";
    }
    deviceLabel.text = [@"蓝牙设备 · " stringByAppendingString:device];
    if (available) {
        autoMode = reply.auto_mode;
        lastPhase = reply.phase;
    }
    [autoSwitch setOn:autoMode != DB_AUTO_OFF animated:NO];
    autoSwitch.enabled = available && reply.phase != DB_STOPPING;
    startButton.enabled = available && (reply.phase == DB_SYSTEM || reply.phase == DB_FAILED);
    stopButton.enabled = available && (reply.phase == DB_RECEIVING || reply.phase == DB_STARTING ||
                                       reply.phase == DB_FAILED ||
                                       (reply.phase == DB_SYSTEM && reply.auto_mode != DB_AUTO_OFF));
    pairButton.enabled = available && reply.phase == DB_RECEIVING;
    [self renderMedia:&reply.media visible:available && reply.phase == DB_RECEIVING];
    playerAvailable = available && reply.phase == DB_RECEIVING && reply.media.sample_rate &&
                      (reply.audio == 1 || playerAvailable);
    if (!playerAvailable) settingsRequested = NO;
    [self showCurrentPanel];
    if (!available) {
        statusLabel.text = @"服务未连接";
        detailLabel.text = @"请稍等几秒。如果持续无法连接，请查看安装日志。";
        return;
    }
    switch (reply.phase) {
        case DB_SYSTEM:
            statusLabel.text = @"系统模式";
            if (autoMode == DB_AUTO_PAUSED) detailLabel.text = @"已手动停止。点「开启蓝牙接收」可恢复自动接收。";
            else if (autoMode == DB_AUTO_ARMED) detailLabel.text = @"等待自动开启蓝牙接收。";
            else detailLabel.text = @"可用 iPhone 从底座播放。开启接收前，请先关闭系统蓝牙。";
            break;
        case DB_STARTING:
            statusLabel.text = @"正在开启";
            detailLabel.text = @"正在初始化蓝牙控制器。";
            break;
        case DB_STOPPING:
            statusLabel.text = @"正在切回系统";
            detailLabel.text = @"正在关闭接收并恢复系统蓝牙服务。";
            break;
        case DB_RECEIVING:
            statusLabel.text = @"蓝牙接收已开启";
            if (reply.audio == 1) detailLabel.text = @"正在播放蓝牙音频。";
            else if (reply.audio == 2) detailLabel.text = @"收到蓝牙连接，但音频输出启动失败。";
            else if (reply.pairing) detailLabel.text = @"配对已开放。请在安卓上搜索 DockBridge 4S。";
            else detailLabel.text = @"等待播放。新设备请先点「重新开放配对」。";
            break;
        case DB_FAILED:
            statusLabel.text = @"接收启动或运行失败";
            if (reply.diagnostic == DB_DIAGNOSTIC_TRANSPORT) {
                detailLabel.text = @"写入蓝牙控制器失败。";
            } else if (reply.diagnostic == DB_DIAGNOSTIC_TIMEOUT) {
                detailLabel.text = @"蓝牙控制器初始化超时。";
            } else {
                detailLabel.text = @"确认系统蓝牙已关闭，再试一次。";
            }
            if (autoMode == DB_AUTO_ARMED) detailLabel.text = [detailLabel.text stringByAppendingString:@" 将自动重试。"];
            break;
    }
}

- (void)sendCommand:(uint8_t)command {
    if (pending) {
        if (command != DB_STATUS) {
            queued = command;
            startButton.enabled = stopButton.enabled = pairButton.enabled = autoSwitch.enabled = NO;
        }
        return;
    }
    pending = YES;
    if (command != DB_STATUS) startButton.enabled = stopButton.enabled = pairButton.enabled = autoSwitch.enabled = NO;
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        db_reply reply = {0};
        BOOL available = exchange(command, &reply);
        dispatch_async(dispatch_get_main_queue(), ^{
            pending = NO;
            [self render:reply available:available];
            if (available && reply.result == DB_ERROR && !queued &&
                (command == DB_AUTO_ENABLE || command == DB_AUTO_DISABLE ||
                 command == DB_STOP || command == DB_START)) {
                UIAlertView *alert = [[[UIAlertView alloc] initWithTitle:@"操作未完成"
                    message:command == DB_STOP ? @"已停止接收，但无法保存停止状态。请再次点停止。"
                                               : @"请检查界面中的状态，再重试。"
                    delegate:nil cancelButtonTitle:@"好" otherButtonTitles:nil] autorelease];
                [alert show];
            }
            if (queued) {
                uint8_t next = queued;
                queued = 0;
                [self sendCommand:next];
            }
        });
    });
}

- (void)refreshStatus { [self sendCommand:DB_STATUS]; }
- (void)stopReceiver { [self sendCommand:DB_STOP]; }
- (void)openPairing { [self sendCommand:DB_PAIR]; }

- (void)startWithCommand:(uint8_t)command {
    if (nativeBluetoothIsOff()) {
        [self sendCommand:command];
        return;
    }
    UIAlertView *alert = [[UIAlertView alloc] initWithTitle:@"先关闭系统蓝牙"
        message:@"在 iPhone 的「设置 → 蓝牙」中关闭蓝牙，再回来开启接收。"
        delegate:self cancelButtonTitle:@"取消" otherButtonTitles:@"已关闭，启动", nil];
    alert.tag = command;
    [alert show];
    [alert release];
}

- (void)startReceiver { [self startWithCommand:DB_START]; }

- (void)autoChanged {
    BOOL enable = autoSwitch.on;
    [autoSwitch setOn:autoMode != DB_AUTO_OFF animated:NO];
    if (!enable) [self sendCommand:DB_AUTO_DISABLE];
    else if (lastPhase == DB_RECEIVING || lastPhase == DB_STARTING) [self sendCommand:DB_AUTO_ENABLE];
    else [self startWithCommand:DB_AUTO_ENABLE];
}

- (void)alertView:(UIAlertView *)alertView clickedButtonAtIndex:(NSInteger)index {
    if (index != alertView.cancelButtonIndex) [self sendCommand:(uint8_t)alertView.tag];
}

@end

@interface DockBridgeDelegate : UIResponder <UIApplicationDelegate> {
    UIWindow *window;
}
@end

@implementation DockBridgeDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    DockBridgeController *controller = [[[DockBridgeController alloc] init] autorelease];
    UINavigationController *navigation = [[[UINavigationController alloc] initWithRootViewController:controller] autorelease];
    window.rootViewController = navigation;
    [window makeKeyAndVisible];
    return YES;
}
- (void)applicationDidEnterBackground:(UIApplication *)application {
    dockbridge_app_audio_backgrounded();
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([DockBridgeDelegate class]));
    }
}
