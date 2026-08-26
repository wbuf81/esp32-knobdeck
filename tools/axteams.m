// Read Teams' in-call state from its own buttons, via the accessibility tree.
//
// Teams' WebView withholds its content from the AX tree until an assistive
// client sets AXManualAccessibility on the app - AppleScript cannot, this can.
// Once set, every button is visible with its full label ("Video call",
// "Mute mic (Cmd+Shift+M)", ...), measured at ~1500 nodes idle. The mute
// button's label flips between Mute and Unmute, which makes it the ONLY source
// on this machine that knows Teams' soft-mute: the local device API never
// binds (Jamf tenant), and the hardware cannot tell - Teams keeps capturing
// while muted.
//
// Output, one line:   in_call=0|1 muted=-1|0|1 camera=-1|0|1
// -1 means "the tree did not say", and the device renders that as unknown -
// a guessed mute over a hot microphone is the lie this project forbids.
//
// English UI labels only; any other locale degrades to unknown, not to wrong.
// Bounded walk (depth, nodes, wall clock): an overrun returns unknown too.
//
// Needs Accessibility for THIS binary when run under launchd. The grant binds
// to the code signature and a rebuild voids it (the volhud lesson), so rebuild
// rarely and re-grant when you do.
//
// Build: clang -framework Cocoa -framework ApplicationServices \
//              -o tools/axteams tools/axteams.m

#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>

static int nodes = 0;
static double deadline = 0;

static double now(void) {
  return [NSDate timeIntervalSinceReferenceDate];
}

// What the walk is allowed to conclude.
static int g_muted = -1;    // from the Mute/Unmute button label
static int g_camera = -1;   // from the camera button label
static int g_in_call = 0;   // a Leave button is the call's fingerprint

static void classify(NSString *label) {
  // Order matters: "Unmute" contains "Mute".
  if ([label rangeOfString:@"Unmute"].location != NSNotFound) {
    g_muted = 1;
    return;
  }
  if ([label rangeOfString:@"Mute mic"].location != NSNotFound ||
      ([label rangeOfString:@"Mute"].location == 0 &&
       [label rangeOfString:@"mic"].location != NSNotFound)) {
    g_muted = 0;
    return;
  }
  if ([label rangeOfString:@"Turn camera off"].location != NSNotFound) {
    g_camera = 1;
    return;
  }
  if ([label rangeOfString:@"Turn camera on"].location != NSNotFound) {
    g_camera = 0;
    return;
  }
  if ([label rangeOfString:@"Leave"].location == 0 ||
      [label rangeOfString:@"Hang up"].location != NSNotFound) {
    g_in_call = 1;
  }
}

static void walk(AXUIElementRef el, int depth) {
  if (depth > 30 || nodes > 25000 || now() > deadline) return;
  nodes++;
  CFTypeRef role = NULL, desc = NULL, title = NULL;
  AXUIElementCopyAttributeValue(el, kAXRoleAttribute, &role);
  NSString *r = (__bridge NSString *)role ?: @"";
  if ([r isEqualToString:@"AXButton"] || [r isEqualToString:@"AXCheckBox"] ||
      [r isEqualToString:@"AXToggle"]) {
    AXUIElementCopyAttributeValue(el, kAXDescriptionAttribute, &desc);
    AXUIElementCopyAttributeValue(el, kAXTitleAttribute, &title);
    NSString *d = (__bridge NSString *)desc ?: @"";
    NSString *t = (__bridge NSString *)title ?: @"";
    classify(d.length ? d : t);
    if (desc) CFRelease(desc);
    if (title) CFRelease(title);
  }
  if (role) CFRelease(role);
  CFTypeRef kids = NULL;
  if (AXUIElementCopyAttributeValue(el, kAXChildrenAttribute, &kids) == 0 &&
      kids) {
    CFArrayRef a = (CFArrayRef)kids;
    for (CFIndex i = 0; i < CFArrayGetCount(a); ++i)
      walk((AXUIElementRef)CFArrayGetValueAtIndex(a, i), depth + 1);
    CFRelease(kids);
  }
}

int main(void) {
  if (!AXIsProcessTrusted()) {
    // Distinguishable from "in a call but tree said nothing": the caller treats
    // a non-zero exit as "grant missing" and logs which grant.
    fprintf(stderr, "axteams: not trusted for Accessibility\n");
    return 3;
  }
  pid_t pid = 0;
  for (NSRunningApplication *a in
       [NSWorkspace sharedWorkspace].runningApplications)
    if ([a.bundleIdentifier isEqualToString:@"com.microsoft.teams2"])
      pid = a.processIdentifier;
  if (!pid) {
    printf("in_call=0 muted=-1 camera=-1\n");
    return 0;
  }
  AXUIElementRef app = AXUIElementCreateApplication(pid);
  AXUIElementSetAttributeValue(app, CFSTR("AXManualAccessibility"),
                               kCFBooleanTrue);
  AXUIElementSetAttributeValue(app, CFSTR("AXEnhancedUserInterface"),
                               kCFBooleanTrue);
  deadline = now() + 0.9;  // the beat budget owns us; overrun degrades to unknown
  walk(app, 0);
  printf("in_call=%d muted=%d camera=%d\n", g_in_call, g_muted, g_camera);
  return 0;
}
