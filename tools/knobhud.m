// The knob's own on-screen volume overlay.
//
// This replaces posting synthetic media-key events (the old volhud), which
// turned out to be structurally fragile: CGEventPost of an NX_KEYTYPE_SOUND_UP
// needs an Accessibility grant, the grant binds to the binary's code signature,
// and every clang link on arm64 produces a fresh ad-hoc CDHash - so REBUILDING
// THE TOOL SILENTLY VOIDS ITS OWN GRANT. It worked exactly once, right after
// the user granted it, and never again after the next rebuild. In a repo where
// the binary gets rebuilt during development, that is a booby trap, not a
// feature.
//
// Drawing a window needs NO permission at all. And unlike the native HUD, this
// one can grow: mute state now, mic and Teams status later, which is the whole
// direction this device is headed.
//
// Runs as a PERSISTENT child of the helper, reading commands from stdin, one
// per line:
//
//   volume 62      show the panel with the bar at 62%
//   muted 1        subsequent panels render as muted
//
// One long-lived process, not one per click: a process per click would stack
// overlapping windows during a fast spin, which is the flicker this design
// exists to avoid. The panel hides itself 1.4s after the last update, and the
// process exits when stdin closes - so it cannot outlive the helper.
//
// Build:  clang -framework Cocoa -o tools/knobhud tools/knobhud.m

#import <Cocoa/Cocoa.h>

@interface HudView : NSView
@property(nonatomic) int volume;
@property(nonatomic) BOOL muted;
@end

@implementation HudView
- (void)drawRect:(NSRect)dirty {
  NSRect b = self.bounds;
  NSBezierPath *panel =
      [NSBezierPath bezierPathWithRoundedRect:b xRadius:14 yRadius:14];
  [[NSColor colorWithWhite:0.08 alpha:0.86] set];
  [panel fill];

  NSString *label = self.muted
                        ? @"Muted"
                        : [NSString stringWithFormat:@"Volume  %d%%", self.volume];
  NSDictionary *attrs = @{
    NSFontAttributeName : [NSFont systemFontOfSize:13 weight:NSFontWeightMedium],
    NSForegroundColorAttributeName :
        (self.muted ? [NSColor colorWithWhite:0.75 alpha:1] : [NSColor whiteColor])
  };
  [label drawAtPoint:NSMakePoint(18, 32) withAttributes:attrs];

  // The track, then the fill. Grey when muted: the level is still true, it is
  // just not what you are hearing.
  NSRect track = NSMakeRect(18, 16, b.size.width - 36, 6);
  [[NSColor colorWithWhite:0.35 alpha:1] set];
  [[NSBezierPath bezierPathWithRoundedRect:track xRadius:3 yRadius:3] fill];
  NSRect fill = track;
  fill.size.width = track.size.width * (self.volume / 100.0);
  [(self.muted ? [NSColor colorWithWhite:0.55 alpha:1] : [NSColor whiteColor]) set];
  [[NSBezierPath bezierPathWithRoundedRect:fill xRadius:3 yRadius:3] fill];
}
@end

@interface Hud : NSObject
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) HudView *view;
@property(nonatomic, strong) NSTimer *hide;
@end

@implementation Hud
- (instancetype)init {
  self = [super init];
  NSScreen *screen = [NSScreen mainScreen];
  NSRect vis = screen.visibleFrame;
  NSRect frame = NSMakeRect(NSMidX(vis) - 120, vis.origin.y + 90, 240, 58);
  _window = [[NSWindow alloc] initWithContentRect:frame
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.opaque = NO;
  _window.backgroundColor = [NSColor clearColor];
  _window.hasShadow = YES;
  _window.ignoresMouseEvents = YES;
  // Above normal windows, on every Space, and allowed next to a full-screen
  // app - a volume overlay that hides behind the thing playing the sound would
  // be furniture.
  _window.level = NSScreenSaverWindowLevel;
  _window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                               NSWindowCollectionBehaviorFullScreenAuxiliary;
  _view = [[HudView alloc] initWithFrame:NSMakeRect(0, 0, 240, 58)];
  _window.contentView = _view;
  return self;
}

- (void)show {
  fprintf(stderr, "[knobhud] show vol=%d muted=%d\n", self.view.volume,
          self.view.muted);
  fflush(stderr);
  [self.window orderFrontRegardless];
  [self.hide invalidate];
  self.hide = [NSTimer scheduledTimerWithTimeInterval:1.4
                                              repeats:NO
                                                block:^(NSTimer *t) {
                                                  fprintf(stderr,
                                                          "[knobhud] hide\n");
                                                  fflush(stderr);
                                                  [self.window orderOut:nil];
                                                }];
}

- (void)handleLine:(NSString *)line {
  // Every received line, to stderr - which the helper's launchd redirect sends
  // to /tmp/knob-maclink.log. This is the receive-side half of a ledger whose
  // send side the helper already writes, so the next real knob turn documents
  // itself end to end with no timing choreography: wrote / received / painted.
  fprintf(stderr, "[knobhud] recv %s\n", line.UTF8String);
  fflush(stderr);
  NSArray<NSString *> *parts =
      [line componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
  if (parts.count < 2) return;
  if ([parts[0] isEqualToString:@"volume"]) {
    int v = parts[1].intValue;
    self.view.volume = MAX(0, MIN(100, v));
    [self.view setNeedsDisplay:YES];
    [self show];
  } else if ([parts[0] isEqualToString:@"muted"]) {
    // State only; showing waits for the next volume line, so a beat updating
    // the mute flag does not flash the panel while nobody is touching the knob.
    self.view.muted = parts[1].intValue != 0;
    [self.view setNeedsDisplay:YES];
  }
  // Unknown first words are ignored, same contract as the wire protocol: a
  // newer helper talking to an older overlay degrades instead of breaking.
}
@end

int main(void) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    // Accessory: no Dock icon, no menu bar, no focus stealing.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    Hud *hud = [[Hud alloc] init];

    dispatch_async(
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
          char buf[128];
          while (fgets(buf, sizeof(buf), stdin)) {
            NSString *line = [[NSString stringWithUTF8String:buf]
                stringByTrimmingCharactersInSet:[NSCharacterSet
                                                    whitespaceAndNewlineCharacterSet]];
            if (line.length == 0) continue;
            dispatch_async(dispatch_get_main_queue(), ^{
              [hud handleLine:line];
            });
          }
          // stdin closed: the helper is gone, and an overlay that outlives its
          // owner is an orphan nobody can dismiss.
          dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp terminate:nil];
          });
        });

    [NSApp run];
  }
  return 0;
}
