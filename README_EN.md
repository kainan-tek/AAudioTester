# AAudioTester

[中文](README.md) | English

An audio testing tool for Android Automotive OS (AAOS) cars, built on the AAudio native API with low-latency playback and recording.

## Features
Top tabs switch between **Playback** / **Recording**; the two features are mutually exclusive (switching tabs stops the current one).

### Playback
- **12 audio scenarios** (media/voice call/call signaling/alarm/notification/ringtone/notification event/accessibility/navigation/system sound/game/voice assistant), configurable via usage/contentType/performanceMode/sharingMode
- Built-in 10s sweep source (`asset://sample/48k_2ch_16bit.wav`), no WAV file needed by default; can also use a `/data/xx.wav` real file
- Full audio support: **1-16 channels**, **8kHz-192kHz**, **16/24/32-bit PCM**
- Audio focus management: auto-stops when focus is taken

### Recording
- **8 audio sources** (generic/camcorder/voice recognition/voice communication/unprocessed/voice performance, plus system echo-reference/hotword)
- Configurable sample rate/channels/bit depth, outputs **valid WAV with correct header**
- Defaults to an auto-named path in the app's private directory (`rec_timestamp_xxk_xch_xbit.wav`); system apps can configure a fixed `/data/` path

## Configuration
`aaudio_configs.json` has two sections: `player` and `recorder` (JSONC, comments allowed). For external hot-reload, place the file at `/data/aaudio_configs.json` (requires system privileges/root). Configs marked `[需系统权限]` (needs system privilege) fail on normal install — expected behavior.

## Deployment
**Normal install** (`adb install`): core features work (built-in source playback, recording to app-private dir, assets config). The following system-only capabilities are **unavailable** (expected):
- `/data` config hot-reload, `/data/xx.wav` playback, fixed `/data` recording paths
- System audio sources (ECHO_REFERENCE/HOTWORD) → stream creation fails

**System-app deployment** (priv-app, userdebug/eng build):
```bash
adb uninstall com.example.aaudiotester          # 1. uninstall the normal install first
# 2. sign the APK with the platform system key
adb root && adb remount                        # 3. remount system partition for write access
adb push AAudioTester.apk /system/priv-app/AAudioTester/AAudioTester.apk
# 4. (recommended) add privapp-permissions-com.example.aaudiotester.xml under /system/etc/permissions/
adb reboot                                      # 5. reboot to apply
```
> For existing system deploys using the old `/data/aaudio_player_configs.json` / `/data/aaudio_recorder_configs.json`, merge them into the new `/data/aaudio_configs.json`.

## Build & Install
JDK 21 required; adjust `org.gradle.java.home` in `gradle.properties` or set `JAVA_HOME` when building elsewhere.
```bash
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## Replace the Built-in Source
Edit `tools/gen_sample_wav.py` and re-run: `python tools/gen_sample_wav.py`

## Manual Verification Checklist (device/emulator)
1. Both tabs switch correctly, each loads its own section configs
2. Default playback works out of the box (built-in source extracted to the app-private dir); configs pointing to `/data/xx.wav` can play real files
3. Switching to the Recording tab while playing stops playback; switching back stops recording; cannot play and record simultaneously
4. Recording produces a valid WAV (path/header/duration correct)
   - Tip: output defaults to the app-private dir; check the `WAV file closed` logcat line for the path
5. Long-press Spinner reload works (including JSONC comments)
6. `RECORD_AUDIO` is requested on Start tap; clear feedback when denied
7. System-only configs fail on normal install without affecting other configs
