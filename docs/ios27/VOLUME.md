# iOS 27 on the Galaxy S9: the volume buttons, the volume HUD, and the Settings Wi-Fi row

Written 17 November 2026 from phone 1's console logs of that day, SpringBoard's own os_log of two boots (captured by
`sbmtlc`, see METAL.md), and the restore image's shared cache (build 24A446), read on the build Mac (names and structure
only). The level meter at the end has **not been run on a phone**.

## What is seen

- Boots up to 18:51 on 17 November: the owner presses the physical volume keys. The kernel reports each press
  (`A5XELTE KEYS: key=2 down=1`, then `SWEET HID: key 0xea down (1 clients)`). No volume HUD appears. None of these
  boots ran an audio server: their consoles have no `exec audiomxd`.
- Boots of 20:58 and 21:30 with the audio step (`tools/ios27/fullroot/steps.d/92-audio-bt.sh`): the volume keys were
  injected from the console (`usb> button up`, then `STARLTE TOUCH: console button up for 80 ms`). A `shot` taken a
  second or more later shows no HUD.
- Boot of 22:22 (`hands2`), audio step on: the owner pressed the physical keys, and the HUD showed and the volume changed
  (the owner's report). The console is the same as in the first case, now with `exec audiomxd` earlier in the boot.
- The HAL plug-in's level (`hakios-audio: the volume: 0.000 dB under the loudest`) is set once, when audiomxd takes the
  speaker, and never again. That is expected, not a fault: see "Where the volume is applied".
- Control Centre's volume slider shows no level and cannot be moved (SpringBoard: `MPVolumeController
  volumeControlAvailableDidChange available: 0`).

## The path of a press

| Link | What | Where it is logged |
|---|---|---|
| 1. GPIO | the key's GPIO interrupt (GPA0) | `A5XELTE KEYS: key=N down=D` (1 up, 2 down) |
| 2. Kernel HID | `SweetHIDButtons` (xnu fork, `iokit/Kernel/arm/hid/SweetHID.cpp`): a keyboard event, consumer page 0x0C, usage 0xE9 (up) or 0xEA (down), from the same service as the side key (0x30) and Home (0x40) | `SWEET HID: key 0xe9 down (1 clients)` (the client is backboardd); `SWEET HID: queued event N` (the first five and every 100th) |
| 3. backboardd | `BKButtonHIDEventProcessor` hands the press to SpringBoard | not logged on this root |
| 4. SpringBoard, UIKit | `SBVolumeHardwareButton`'s press recognizers | `RECEIVED volumeIncreasePress down:1`, `SBVolumeControl -- volume increment -- button state: down` |
| 5. SpringBoard, who takes it | `SBVolumeHardwareButtonActions`: an app that captures the buttons (Camera registers `_UIPhysicalButtonInteraction` for both while its viewfinder is shown), a "bandit", or `SBVolumeControl` | `Hardware Volume increase press handled by ...` |
| 6. SpringBoard, may it | `SBVolumeControl changeVolumeByDelta:` asks MediaExperience for the active category and the route | `(...) Attempting to change volume by delta ...`, then `... was Accepted` or `... was Denied; reason: ...` |
| 7. MediaExperience (audiomxd) | `AVSystemController changeActiveCategoryVolume:`; the new volume comes back as a notification, and only then does SpringBoard show the HUD (`_effectiveVolumeChanged:` → `_presentVolumeHUDWithVolume:`) | audiomxd's stderr with `AUDIO_BT_LOG=1` |
| 8. VirtualAudio, HAL | the speaker port's volume, applied to the samples | the level meter below |

SpringBoard refuses at link 6 when the device is locked, no app is hosted on the lock screen and nothing plays ("the
device is locked ..."); when the current route has no volume control and the device is not fully muted; and when the
active category is not one it manages (Audio/Video, Alarm, Ringtone, PhoneCall, VoiceCommand). On phone 1, SpringBoard
starts with `activeCategoryName` Audio/Video and `activeAudioRoute` Speaker (its log, 21:25 boot).

## What was wrong

- **No audio server (certain).** Without audiomxd nothing answers link 7. The presses reach SpringBoard and stop there.
  The 92-audio-bt step, on by default in the root build since 17 November, brings audiomxd and HakiosAudio.driver.
  With it the physical keys work (22:22 boot).
- **The injected tests (not proven either way).** A console press makes the same kernel event as a physical one (link
  2). The no-HUD result of the 20:58 and 21:30 boots has two possible explanations, and neither is proven. (a) The shot
  came after the HUD had gone: it fades on its own within about two seconds of the last press, and the shot is taken
  over USB after the press. (b) At 21:31 the phone had just been locked and woken (`button lock` twice, `button home`
  three times), so SpringBoard may still have been on the cover sheet, where it refuses with nothing playing.
- **Control Centre's slider (open).** MediaRemote's local endpoint finds no output device to control. The shared audio
  context lists the Speaker, but the volume controller in audiomxd (`com.apple.coremedia.volumecontroller.xpc`) reports
  none: SpringBoard logs `VolumeClientOutputDevices: (null)`, then `ResolvedOutputDevices` empty and "Endpoint volume
  control is only possible if the endpoint is picked or remote controllable". MediaRemote keeps only the shared context's
  devices that the volume client also has (`outputDevicesByMergingBase:withOverrides:`), so the slider has nothing to
  act on. Why audiomxd's volume client has no route configuration is not known. Its os_log (`AUDIO_BT_LOG=1`) is the next
  thing to read.

## Where the volume is applied

VirtualAudio keeps the iPhone's speaker in software volume mode. It sets the HAL plug-in's volume control once, to the
top, and then scales the samples itself (`userland/src/audio_plugin.c`, first comment; seen on the phone on 15 October).
So neither the plug-in's control nor the kernel's amplifier level moves with the buttons. The plug-in sees the volume
only in the size of its samples. The level meter (`userland/src/hakios_audio_level.h`) shows it:

    echo log > /private/var/local/hakios-audio-level   # measure and play
    echo dry > /private/var/local/hakios-audio-level   # measure, play nothing: the amplifiers stay off

The plug-in reads the switch each time the speaker starts. While the speaker plays, the console then has one line a
second (at most 120 a run):

    hakios-audio-level: the speaker starts; dry run: its samples are measured and NOT played
    hakios-audio-level: second 1: peak -12.4 dBFS, average -24.0 dBFS

A volume step moves both numbers by the step. Without the file nothing changes.

## Tests (for the phone agent; no sound is needed)

1. **Keys, with SpringBoard's word for it.** `SpringBoard` listed in `/private/var/local/oslog-clients`, and
   `SpringBoard.log` emptied on the root just before the boot. Its capture stops at 8 MiB, about a minute after
   SpringBoard starts, so press within that minute. Unlock to the home screen, Camera not in front. Press volume up
   (physical, or `button up`) and take `shot` AT ONCE. Then press volume down and take a shot at once. Expected in
   `SpringBoard.log`: `RECEIVED volumeIncreasePress down:1`, `Hardware Volume increase press handled by
   SBVolumeControl` (or the name of whoever took it), and `Volume Change by delta ... was Accepted`. The shot shows the
   HUD. A `Denied; reason:` line names the refusal.
2. **The sound follows, silently.** `dry` in `/private/var/local/hakios-audio-level`. In Settings > Sounds & Haptics, set
   the ringer slider to its middle and tap a ringtone, then set it to the top and tap the ringtone again. Expected: two
   runs of `hakios-audio-level` lines, the second clearly higher (by however many dB the slider moved). For the
   buttons (media volume): play anything with sound (a video in Photos or Safari) with `dry` on, and press volume down twice. Expected: the levels drop by two steps
   while it plays, with `Accepted` lines in SpringBoard.log. Remove the file afterwards.
3. **Control Centre.** Open Control Centre and drag the volume slider. Expected today: it does not move, and
   SpringBoard logs `volume control not available` (the open item above). With `AUDIO_BT_LOG=1`, send
   `grep -i -E "volume|endpoint|output ?device|route" audiomxd.err`.

## The Settings Wi-Fi row

The root list of Settings (iOS 27's SwiftUI `SettingsApp`, `WiFiListItemProvider`) takes the network's name from
WiFiKit's `WFWiFiStateMonitor`. Its current network comes from `-[WFInterface asyncCurrentNetwork:]`, which asks CoreWiFi
for `currentScanResult`. The Wi-Fi page (WiFiSettingsKit) shows the joined network from the known-network profile
(`currentKnownNetworkProfile`), which is why it is ticked there while the row has no name. The kernel's Apple80211
stand-in (`drivers/wifi/hakios.apple80211.c`) answers CURRENT_NETWORK (103) once joined: `wifid gets 103
(CURRENT_NETWORK) ... answered`, about 170 times in the 20:58 boot, with the elements room IO80211 asks for (2048 bytes). What
wifid makes of it for CoreWiFi's `currentScanResult` is not known. SpringBoard's own Wi-Fi manager starts before the
radio does (`wifi_delay=` 60 s) and logs "WiFi device not found ... until a device is attached"; that is expected.

To find out, capture Preferences' os_log: add `Preferences` to m.dylib's sweetinject group (as was done for Sileo) and
to `oslog-clients`, open Settings with Wi-Fi joined, and grep `Preferences.log` for `infraScanResult`, `network changed
to`, `no change to network`, `updating state`, `interface is unavailable` (WiFiKit's lines). A nil `infraScanResult`
confirms the empty `currentScanResult`. The next step would then be wifid's side.
