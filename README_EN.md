# AAudioTester

[中文](README.md) | English

An audio testing tool for Android Automotive OS (AAOS) cars, built on the AAudio native API with low-latency playback and recording.

## Features

Top tabs switch between **Playback** / **Recording**; the two features are mutually exclusive (switching tabs stops the current one).

### Playback

- **16 audio scenarios** (media/voice call/call signaling/alarm/notification/ringtone/notification event/accessibility/navigation/system sound/game/voice assistant, plus 4 AAOS system usages: emergency/safety/vehicle status/announcement), configurable via usage/contentType/performanceMode/sharingMode
- Built-in 20s pink noise source (`asset://sample/48k_2ch_16bit.wav`), no WAV file needed by default; can also use a `/data/xx.wav` real file
- Full audio support: **1-16 channels**, **8kHz-192kHz**, **16/24/32-bit PCM**
- Audio focus management: auto-stops when focus is taken

### Recording

- **8 audio sources** (generic/camcorder/voice recognition/voice communication/unprocessed/voice performance, plus system echo-reference/hotword)
- Configurable sample rate/channels/bit depth, outputs **valid WAV with correct header**
- Defaults to an auto-named path in the app's private directory (`rec_timestamp_xxk_xch_xbit.wav`); system apps can configure a fixed `/data/` path (see "Advanced: System-level Deployment")

## Requirements

- Device: Android 12L (API 32)+; an AAOS head unit or AAOS emulator is recommended
- Build: JDK 21 + Android SDK (compileSdk 37) + NDK; adjust `org.gradle.java.home` in `gradle.properties` or set `JAVA_HOME` when building elsewhere
- AAOS system usages and the low-latency (exclusive / MMAP) path depend on the device audio framework

## Quick Start

```bash
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

1. Open the app, pick a config on the **Playback** tab → Start (the built-in source works out of the box, extracted to the private dir automatically)
2. **Recording** tab → Start → grant the microphone permission → Stop; the WAV lands in the app-private directory by default

> **Permission tip**: the recording permission is requested only on the first Start tap; if you deny it with "don't ask again", subsequent attempts just fail with no dialog. Re-enable it via system Settings → Apps → AAudioTester → Permissions.

## Logging & Debugging

```bash
# Config loading logs
adb logcat -s AAudioConfig

# Playback/recording logs (incl. native layer)
adb logcat -s AAudioPlayer AAudioRecorder
```

## Configuration

`aaudio_configs.xml` (bundled in assets) has two sections: `player` and `recorder` — element-style XML, one `<config>` per entry, comments supported natively. Fields are optional (defaults apply); a single invalid entry is skipped without affecting the rest.

### Field Reference

| Field | Section | Default | Notes |
| --- | --- | --- | --- |
| `usage` | player | `AAUDIO_USAGE_MEDIA` | audio usage, incl. 4 AAOS system usages (need system privilege) |
| `contentType` | player | `AAUDIO_CONTENT_TYPE_MUSIC` | content type |
| `performanceMode` | both | `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` | low latency / power saving |
| `sharingMode` | both | `AAUDIO_SHARING_MODE_SHARED` | shared / exclusive (exclusive recommended for low latency) |
| `inputPreset` | recorder | `AAUDIO_INPUT_PRESET_GENERIC` | input preset, 8 available |
| `sampleRate` | recorder | `48000` | sample rate |
| `channelCount` | recorder | `1` | channel count |
| `format` | recorder | `16` | bit depth 16/24/32 |
| `audioFilePath` | both | empty | playback: empty = built-in source (extracted to private dir), or `/data/xx.wav`; recording: empty = auto-named private dir |
| `description` | both | `Default Configuration` | name shown in the Spinner |

### Example

```xml
<player>
  <config>
    <usage>AAUDIO_USAGE_GAME</usage>
    <contentType>AAUDIO_CONTENT_TYPE_MUSIC</contentType>
    <performanceMode>AAUDIO_PERFORMANCE_MODE_LOW_LATENCY</performanceMode>
    <sharingMode>AAUDIO_SHARING_MODE_EXCLUSIVE</sharingMode>
    <description>My Game Scene</description>
  </config>
</player>
```

External hot-reload: place the file at `/data/aaudio_configs.xml` (takes priority over assets, needs system privilege, see "Advanced: System-level Deployment"). Configs marked `[Requires system permission]` fail on normal install — expected behavior.

## Known Limitations

This section covers user-visible product limitations; implementation-level design trade-offs (notification degradation policy, RT thread constraints, etc.) are documented in [docs/known-limitations.md](docs/known-limitations.md).

- Disk I/O is moved off the AAudio real-time callback via a ring buffer + dedicated read/write threads (the callback only does memcpy). The recording callback uses `tryWrite`, dropping whole frames when the ring buffer is full (never blocks); on slow storage frames are dropped (see the `Recording dropped bytes` log). On playback, if the read thread can't keep up, underruns output silence (see the `Playback underruns` log).
- AAOS emulator: shortly after restoring from a snapshot, audio focus requests may be rejected (Start reports an error); retry after a few minutes

## Advanced: System-level Deployment

### Capability Matrix

| Capability | Normal install | System app (priv-app) |
| --- | --- | --- |
| Built-in source playback, recording to private dir, assets config | ✅ | ✅ |
| `/data` config hot-reload, `/data` WAV playback, fixed recording paths | ❌ | ✅ (still gated by SELinux/DAC) |
| AAOS system usages (emergency/safety/vehicle status/announcement) & system sources (ECHO_REFERENCE/HOTWORD) | ❌ stream creation fails | depends on device framework |

### /data File Access

Apps reading config/WAV files under `/data` are blocked by the system security policy (`chmod 644` is not enough). On debug devices, temporarily relax it:

```bash
adb root && setenforce 0
```

After that the app can read the files (still needs 644). To write new files into `/data/`, pre-create a directory writable by the app:

```bash
adb shell mkdir /data/audio && adb shell chown <app_uid> /data/audio
```

For production, allow it in the system policy; or place WAV/output files in the app's private directory (config hot-reload needs extra support).

### System-app Deployment (priv-app, userdebug/eng build)

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

> priv-app grants system signature and privileged permissions; `/data` file access is still gated by SELinux/DAC (see "/data File Access"). Whether the AAOS system usages and system sources (ECHO_REFERENCE/HOTWORD) work depends on the device's AAOS framework support, not the install method.

## Development

Replace the built-in source: edit `tools/gen_sample_wav.py` and re-run `python tools/gen_sample_wav.py`.

## Manual Verification Checklist

Device-dependent items, verified manually:

1. Both tabs switch correctly, each loads its own section configs
2. Default playback works out of the box (built-in source extracted to the app-private dir); configs pointing to `/data/xx.wav` can play real files
3. Switching to the Recording tab while playing stops playback; switching back stops recording; cannot play and record simultaneously
4. Recording produces a valid WAV (path/header/duration correct)
   - Tip: output defaults to the app-private dir; the `WAV file opened for writing` logcat line (emitted at recording start) shows the full path
5. Long-press Spinner reload works (including XML comments)
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
- **Email**: <kainanos@outlook.com>
- **GitHub**: <https://github.com/kainan-tek/AAudioTester>
- **Issues**: <https://github.com/kainan-tek/AAudioTester/issues>

---

<div align="center">

**If this project helps you, please give it a ⭐ Star!**

Made with ❤️ by kainan-tek

[⬆ Back to top](#aaudiotester)

</div>
