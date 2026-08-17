# AAudioTester

[中文](README.md) | English

An audio testing tool for Android Automotive OS (AAOS) cars, built on the AAudio native API with low-latency playback and recording.

## Features
Top tabs switch between **Playback** / **Recording**; the two features are mutually exclusive (switching tabs stops the current one).

### Playback
- **16 audio scenarios** (media/voice call/call signaling/alarm/notification/ringtone/notification event/accessibility/navigation/system sound/game/voice assistant, plus 4 AAOS system usages: emergency/safety/vehicle status/announcement), configurable via usage/contentType/performanceMode/sharingMode
- Built-in 10s sweep source (`asset://sample/48k_2ch_16bit.wav`), no WAV file needed by default; can also use a `/data/xx.wav` real file
- Full audio support: **1-16 channels**, **8kHz-192kHz**, **16/24/32-bit PCM**
- Audio focus management: auto-stops when focus is taken

### Recording
- **8 audio sources** (generic/camcorder/voice recognition/voice communication/unprocessed/voice performance, plus system echo-reference/hotword)
- Configurable sample rate/channels/bit depth, outputs **valid WAV with correct header**
- Defaults to an auto-named path in the app's private directory (`rec_timestamp_xxk_xch_xbit.wav`); system apps can configure a fixed `/data/` path (needs a pre-created writable directory, see "/data File Access")

## Known Limitations
- Disk I/O is moved off the AAudio real-time callback via a ring buffer + dedicated read/write threads (the callback only does memcpy). The recording callback uses `tryWrite`, dropping whole frames when the ring buffer is full (never blocks); on slow storage frames are dropped (see the `Recording dropped bytes` log). On playback, if the read thread can't keep up, underruns output silence (see the `Playback underruns` log).

## Quick Start

```bash
# View config loading logs
adb logcat -s AAudioConfig

# Check the external config file
adb shell cat /data/aaudio_configs.json

# Playback/recording logs (incl. native layer)
adb logcat -s AAudioPlayer AAudioRecorder
```

## Configuration
`aaudio_configs.json` has two sections: `player` and `recorder` (JSONC, comments allowed). For external hot-reload, place the file at `/data/aaudio_configs.json` (needs root to relax SELinux, see "/data File Access"). Configs marked `[需系统权限]` (needs system privilege) fail on normal install — expected behavior.

## /data File Access
Apps reading config/WAV files under `/data` are blocked by the system security policy (`chmod 644` is not enough). On debug devices, temporarily relax it:

```bash
adb root && setenforce 0
```

After that the app can read the files (still needs 644). To write new files into `/data/`, pre-create a directory writable by the app:

```bash
adb shell mkdir /data/audio && adb shell chown <app_uid> /data/audio
```

For production, allow it in the system policy; or place WAV/output files in the app's private directory (config hot-reload needs extra support).

## Deployment
**Normal install** (`adb install`): core features work (built-in source playback, recording to app-private dir, assets config). The following system-only capabilities are **unavailable** (expected):
- `/data` config hot-reload, `/data/xx.wav` playback, fixed `/data` recording paths
- System audio sources (ECHO_REFERENCE/HOTWORD) → stream creation fails

**System-app deployment** (priv-app, userdebug/eng build):
```bash
adb uninstall com.example.aaudiotester          # 1. uninstall the normal install first
# 2. sign the APK with the platform system key
adb root && adb remount                        # 3. remount system partition for write access
adb push AAudioTester.apk /system/priv-app/AAudioTester/AAudioTester.apk  # 4. filename must match the dir name
# 5. (recommended) add privapp-permissions-com.example.aaudiotester.xml under /system/etc/permissions/
#    include signature permissions: MODIFY_AUDIO_ROUTING / CAPTURE_AUDIO_OUTPUT / CAPTURE_AUDIO_HOTWORD
#    (enables AAOS system usages and system sources ECHO_REFERENCE/HOTWORD)
adb reboot                                      # 6. reboot to apply
```

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

## Related Projects

- [AudioTester](https://github.com/kainan-tek/AudioTester) - audio testing tool based on AudioTrack/AudioRecord
- [audio_test_client](https://github.com/kainan-tek/audio_test_client) - system-level audio testing tool for Android

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

**Note**: This project is for learning and testing purposes only.

## Contact

- **Author**: kainan-tek
- **Email**: kainanos@outlook.com
- **GitHub**: https://github.com/kainan-tek/AAudioTester
- **Issues**: https://github.com/kainan-tek/AAudioTester/issues

---

<div align="center">

**If this project helps you, please give it a ⭐ Star!**

Made with ❤️ by kainan-tek

[⬆ Back to top](#aaudiotester)

</div>
