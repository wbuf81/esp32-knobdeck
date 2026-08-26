// Is the camera on? Is anything capturing the microphone?
//
// Asked of the HARDWARE, not of any app: CoreMediaIO and CoreAudio expose
// "is this device running somewhere" - the same flags the menu-bar orange and
// green dots are built on - and reading them needs NO permission at all.
//
// This exists because Teams' local device API (the Stream Deck socket on 8124)
// would not bind on this machine even with enable_third_party_devices_service
// forced true - tenant policy or a macOS build without the server. Camera and
// mic truth do not need Teams' cooperation:
//
//   cam=1  something (in a Teams call: your video) has the camera running
//   mic=1  something is capturing the mic - in-call detection when combined
//          with "is Teams running", which the caller checks
//
// What this deliberately CANNOT say: whether Teams' soft-mute is on. Teams
// keeps capturing while muted and gates the audio in software, so mic=1 while
// muted is expected and mute state stays UNKNOWN until a source that actually
// knows provides it.
//
// Build:  clang -framework CoreMediaIO -framework CoreAudio \
//               -framework CoreFoundation -o tools/avstate tools/avstate.m

#import <CoreAudio/CoreAudio.h>
#import <CoreMediaIO/CMIOHardware.h>
#import <CoreFoundation/CoreFoundation.h>
#import <stdio.h>

static int cameraRunning(void) {
  CMIOObjectPropertyAddress addr = {
      kCMIOHardwarePropertyDevices, kCMIOObjectPropertyScopeGlobal,
      kCMIOObjectPropertyElementMain};
  UInt32 size = 0;
  if (CMIOObjectGetPropertyDataSize(kCMIOObjectSystemObject, &addr, 0, NULL,
                                    &size) != 0)
    return -1;
  UInt32 count = size / sizeof(CMIOObjectID);
  if (count == 0) return 0;
  CMIOObjectID devices[64];
  if (count > 64) count = 64;
  UInt32 used = 0;
  if (CMIOObjectGetPropertyData(kCMIOObjectSystemObject, &addr, 0, NULL,
                                count * sizeof(CMIOObjectID), &used,
                                devices) != 0)
    return -1;
  CMIOObjectPropertyAddress running = {
      kCMIODevicePropertyDeviceIsRunningSomewhere,
      kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain};
  for (UInt32 i = 0; i < used / sizeof(CMIOObjectID); ++i) {
    UInt32 on = 0, sz = sizeof(on);
    if (CMIOObjectGetPropertyData(devices[i], &running, 0, NULL, sizeof(on),
                                  &sz, &on) == 0 &&
        on)
      return 1;
  }
  return 0;
}

static int micRunning(void) {
  AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDevices,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL,
                                     &size) != 0)
    return -1;
  UInt32 count = size / sizeof(AudioObjectID);
  AudioObjectID devices[64];
  if (count > 64) count = 64;
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL,
                                 &size, devices) != 0)
    return -1;
  for (UInt32 i = 0; i < count; ++i) {
    // Inputs only: a device with input streams.
    AudioObjectPropertyAddress streams = {kAudioDevicePropertyStreams,
                                          kAudioDevicePropertyScopeInput,
                                          kAudioObjectPropertyElementMain};
    UInt32 ssz = 0;
    if (AudioObjectGetPropertyDataSize(devices[i], &streams, 0, NULL, &ssz) !=
            0 ||
        ssz == 0)
      continue;
    AudioObjectPropertyAddress running = {
        kAudioDevicePropertyDeviceIsRunningSomewhere,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 on = 0, osz = sizeof(on);
    if (AudioObjectGetPropertyData(devices[i], &running, 0, NULL, &osz, &on) ==
            0 &&
        on)
      return 1;
  }
  return 0;
}

int main(void) {
  printf("mic=%d cam=%d\n", micRunning(), cameraRunning());
  return 0;
}
