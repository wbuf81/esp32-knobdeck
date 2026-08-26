// Post the same event the physical volume keys post, so macOS draws its own
// volume HUD.
//
// This exists because there is no other way. Measured, not assumed:
//   - `osascript -e "set volume output volume N"` changes the volume SILENTLY.
//     macOS deliberately does not draw the HUD for a programmatic change.
//   - AppleScript `key code 72 / 73 / 74 / 103 / 111` all execute without error
//     and do nothing at all. The volume keys are not key codes; they are
//     NSEventTypeSystemDefined events with subtype 8, which AppleScript cannot
//     express.
//
// So: fifteen lines of Objective-C and one Accessibility grant.
//
// CGEventPost to the HID tap is SILENTLY DROPPED without that grant - no error,
// no output, the volume simply does not move. If this binary appears to do
// nothing, that is the reason, and it is why it must live at a stable path:
// the grant is bound to the path, so rebuilding somewhere else needs a new one.
//
// Build:  clang -framework Cocoa -o tools/volhud tools/volhud.m
// Use:    volhud up   |   volhud down   |   volhud mute

#import <Cocoa/Cocoa.h>

// From IOKit's hidsystem/ev_keymap.h. Spelled out rather than included so this
// stays a single file with one framework.
enum { kSoundUp = 0, kSoundDown = 1, kMute = 7 };

// A media key is TWO events, down then up. Sending only the down event leaves
// the key logically held: the HUD appears and the volume keeps stepping.
static void tap(int key, BOOL down) {
  NSEvent *e = [NSEvent otherEventWithType:NSEventTypeSystemDefined
                                  location:NSZeroPoint
                             modifierFlags:(down ? 0xa00 : 0xb00)
                                 timestamp:0
                              windowNumber:0
                                   context:nil
                                   subtype:8
                                     data1:((key << 16) | ((down ? 0xa : 0xb) << 8))
                                     data2:-1];
  CGEventPost(kCGHIDEventTap, [e CGEvent]);
}

int main(int argc, char **argv) {
  // Report whether we are actually TRUSTED, because CGEventPost gives no error
  // when it is dropped - it returns void, the process exits 0, and the volume
  // simply does not move. Exiting 0 in that state made the helper log "(HUD)"
  // for an event that went nowhere, which is a lie that looks like success.
  //
  // Exit 3 means "built fine, not permitted": the caller can then fall back to
  // a silent change rather than pretending.
  if (!AXIsProcessTrusted()) {
    fprintf(stderr, "volhud: not trusted for Accessibility; event would be "
                    "dropped\n");
    return 3;
  }
  if (argc < 2) {
    fprintf(stderr, "usage: volhud up|down|mute\n");
    return 2;
  }
  int key;
  if (strcmp(argv[1], "up") == 0) {
    key = kSoundUp;
  } else if (strcmp(argv[1], "down") == 0) {
    key = kSoundDown;
  } else if (strcmp(argv[1], "mute") == 0) {
    key = kMute;
  } else {
    fprintf(stderr, "usage: volhud up|down|mute\n");
    return 2;
  }
  tap(key, YES);
  tap(key, NO);
  return 0;
}
