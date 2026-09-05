#include "aaudio_player.h"
#include "ring_buffer.h"
#include "wav_file.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <aaudio/AAudio.h>

#include "aaudio_common.h"

#define LATENCY_TEST_ENABLE 0

#if LATENCY_TEST_ENABLE
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

#define LATENCY_TEST_GPIO_FILE "/sys/class/gpio/gpio376/value"
#define LATENCY_TEST_INTERVAL 100
#endif

/**
 * Global audio player state
 *
 * Design decision: Using global state because:
 * 1. Single instance per application (audio hardware is exclusive resource)
 * 2. JNI library is loaded once per process
 * 3. Simpler and more performant than instance management
 * 4. No requirement for multiple simultaneous playback instances
 *
 * Thread safety: Protected by atomic operations and JNI calls from Java layer
 * All state modifications are serialized through JNI method calls
 */
struct AudioPlayerState {
    aaudio_common::AAudioStreamPtr stream;
    std::unique_ptr<WavFile> wav_file;
    std::atomic<bool> is_playing{false};
    std::atomic<bool> callback_notified{false};  // true if Java already notified by callback

    // Playback data buffer: reader thread (producer) → callback (consumer), moving disk I/O out of the real-time callback.
    std::unique_ptr<SpScRingBuffer> ring;
    std::thread read_thread;
    std::atomic<bool> eof_reached{false};
    std::atomic<bool> stop_read_thread{false};
    std::atomic<size_t> underrun_count{0};
    // Terminal error set by the RT callback (literals in the data/error callbacks), delivered to
    // Java by the reader thread on a non-RT thread (protocol in RtErrorSlot)
    aaudio_common::RtErrorSlot rt_error;

    JavaVM* jvm = nullptr;
    jobject player_instance = nullptr;
    jmethodID on_playback_started_method = nullptr;
    jmethodID on_playback_stopped_method = nullptr;
    jmethodID on_playback_error_method = nullptr;

    aaudio_usage_t usage = AAUDIO_USAGE_MEDIA;
    aaudio_content_type_t content_type = AAUDIO_CONTENT_TYPE_MUSIC;
    aaudio_performance_mode_t performance_mode = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
    aaudio_sharing_mode_t sharing_mode = AAUDIO_SHARING_MODE_SHARED;
    std::string audio_file_path = "/data/48k_2ch_16bit.wav";

#if LATENCY_TEST_ENABLE
    std::atomic<int> write_counter{0};   // Single source of truth: block number derives mute and GPIO level
    std::atomic<bool> latency_test_enabled{false};
    int gpio_fd = -1;
#endif
};

namespace {

AudioPlayerState g_player;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Not playing: gate for initialize/setConfig — native state must not change mid-session
bool isPlayerIdle() {
    return !g_player.is_playing.load(std::memory_order_acquire);
}

// Chunk size for file→ring reads, rounded down to a frame multiple (24-bit frames don't divide
// 64KB evenly) so ring contents stay frame-aligned. Caller must ensure wav_file is open.
size_t readChunkBytes() {
    const size_t frame_size =
        aaudio_common::bytesPerSample(g_player.wav_file->getAAudioFormat()) *
        static_cast<size_t>(g_player.wav_file->getChannelCount());
    return (64 * 1024) / frame_size * frame_size;
}

}  // namespace

void aaudio_player_set_jvm(JavaVM* vm) {
    g_player.jvm = vm;
}

#if LATENCY_TEST_ENABLE
static bool initGpio() {
    g_player.gpio_fd = open(LATENCY_TEST_GPIO_FILE, O_WRONLY);
    if (g_player.gpio_fd < 0) {
        LOGE("Failed to open GPIO file: %s, errno: %d", LATENCY_TEST_GPIO_FILE, errno);
        return false;
    }
    LOGI("GPIO file opened successfully: fd=%d", g_player.gpio_fd);
    return true;
}

static void closeGpio() {
    if (g_player.gpio_fd >= 0) {
        close(g_player.gpio_fd);
        g_player.gpio_fd = -1;
    }
}

static inline bool writeGpioValue(int value) {
    if (g_player.gpio_fd < 0) {
        return false;
    }

    char buf = value ? '1' : '0';
    ssize_t result = write(g_player.gpio_fd, &buf, 1);

    if (result != 1) {
        LOGE("Failed to write GPIO value: %d, result: %zd, errno: %d", value, result, errno);
        return false;
    }

    return true;
}

#endif

static void notifyPlaybackError(const std::string& error);  // Forward: started's failure path delivers as an error

static void notifyPlaybackStarted() {
    if (!aaudio_common::callVoidMethod(g_player.jvm, g_player.player_instance, g_player.on_playback_started_method,
                                       "playback started")) {
        // The session is live but Kotlin never learned of it (state stays IDLE, and stop() is
        // IDLE-gated — no UI way to end it): deliver as a terminal error instead, so the existing
        // ERROR path (state=ERROR → Fragment.onError queues stop()) cleans up and recovers the UI.
        // Safe to call directly: we are on the executor thread, not the RT callback thread.
        notifyPlaybackError("[STREAM] Started notification failed to deliver");
    }
}

static bool notifyPlaybackStopped() {
    return aaudio_common::callVoidMethod(g_player.jvm, g_player.player_instance, g_player.on_playback_stopped_method,
                                         "playback stopped");
}

static void notifyPlaybackError(const std::string& error) {
    // Atomic dedup: the rt_error path and errorCallback may deliver concurrently; notify only once.
    // If already set (stopped/error), ignore; subsequent stopNativePlayback also won't resend onStopped over an error notice
    if (g_player.callback_notified.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!aaudio_common::notifyErrorToJava(g_player.jvm, g_player.player_instance, g_player.on_playback_error_method,
                                          error, "playback error")) {
        // Delivery failed: release the latch so the stop path can still notify (duplicate beats lost —
        // a consumed latch with no delivery would leave the UI stuck in PLAYING forever)
        g_player.callback_notified.store(false, std::memory_order_release);
    }
}

// Stop the reader thread and join (shared by failed start and normal stop).
static void stopReadThread() {
    g_player.stop_read_thread.store(true, std::memory_order_release);
    if (g_player.read_thread.joinable()) {
        g_player.read_thread.join();
    }
}

// Playback reader thread: keeps reading WAV data into the ring buffer; the callback thread only memcpys and never touches disk.
// Also serves as the termination notification thread: EOF/RT error JNI notifications are delivered here (non-RT thread); the callback thread only sets flags.
// On the EOF path it also performs the session teardown (stream/wav_file/ring) — EOF sets Kotlin state to IDLE, so no queued stop() would clean up otherwise.
static void playReadThread() {
    std::vector<char> buf(readChunkBytes());
    while (!g_player.stop_read_thread.load(std::memory_order_acquire)) {
        // Terminal error set by the RT callback: delivered here to avoid JNI on the callback thread
        if (const char* msg = g_player.rt_error.take()) {
            g_player.is_playing.store(false, std::memory_order_release);
            notifyPlaybackError(msg);
            return;
        }
        const size_t n = g_player.wav_file->readAudioData(buf.data(), buf.size());
        if (n == 0) {
            if (g_player.wav_file->hasReadError()) {
                // I/O error or truncated file: report as an error rather than faking a normal EOF
                g_player.is_playing.store(false, std::memory_order_release);
                notifyPlaybackError("[FILE] Audio read failed or file truncated");
                return;
            }
            g_player.eof_reached.store(true, std::memory_order_release);  // File fully read
            // Wait for the callback to drain the ring before notifying stopped (RT callback only returns STOP, never touches JNI)
            while (!g_player.stop_read_thread.load(std::memory_order_acquire) &&
                   g_player.ring->readable() > 0 &&
                   !g_player.rt_error.pending()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            g_player.is_playing.store(false, std::memory_order_release);
            if (const char* msg = g_player.rt_error.take()) {
                notifyPlaybackError(msg);
            } else if (g_player.ring->readable() == 0 &&
                       !g_player.callback_notified.exchange(true, std::memory_order_acq_rel)) {
                // Notify stopped only on a genuine drain. An exit via stop_read_thread with data
                // still queued is a user stop or a failed start — notification belongs to
                // stopNativePlayback / the start-failure path, whose error this latch would swallow.
                if (!notifyPlaybackStopped()) {
                    // Delivery failed: release the latch so a later user stop can still notify
                    // (same rationale as the error path's latch release in notifyPlaybackError)
                    g_player.callback_notified.store(false, std::memory_order_release);
                }
            }
            if (g_player.underrun_count.load(std::memory_order_relaxed) > 0) {
                LOGW("Playback underruns: %zu", g_player.underrun_count.load(std::memory_order_relaxed));
            }
            // EOF: playback ended, proactively close the stream to release the hardware session (start/release resets are idempotent fallbacks;
            // executor-side stream operations all happen after joining the reader thread, so there is no concurrent reset)
            if (g_player.stream) g_player.stream.reset();
            // Finish the teardown here: EOF sets Kotlin state to IDLE, so no queued stop() will clean up
            // after us (unlike error paths, which delegate to the stop queued by onError). All executor-side
            // entries join this thread first, and AAudioStream close waits for in-flight callbacks, so
            // nothing can still touch these.
            g_player.wav_file.reset();
            g_player.ring.reset();
            return;
        }
        size_t off = 0;
        while (off < n && !g_player.stop_read_thread.load(std::memory_order_acquire)) {
            if (g_player.rt_error.pending()) break;  // Yield early to error delivery
            off += g_player.ring->write(buf.data() + off, n - off);
            if (off < n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // Before exit, deliver an error the RT callback may have set during the stop race window
    if (const char* msg = g_player.rt_error.take()) {
        g_player.is_playing.store(false, std::memory_order_release);
        notifyPlaybackError(msg);
    }
}

static aaudio_data_callback_result_t audioCallback(AAudioStream* stream,
                                                   void* userData,
                                                   void* audioData,
                                                   int32_t numFrames) {
    if (!g_player.is_playing.load(std::memory_order_acquire)) {
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    if (!g_player.wav_file || !g_player.wav_file->isOpen()) {
        // Only set the flag; JNI notification is delivered by the reader thread on a non-RT thread (no JNI on the callback thread)
        g_player.rt_error.publish("[FILE] Audio file not opened");
        g_player.is_playing.store(false, std::memory_order_release);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    // Callback argument guard: clear state and set the flag, matching the file-error branch above (a bare return would leave the UI stuck on "in progress")
    int32_t channel_count = AAudioStream_getChannelCount(stream);
    if (numFrames <= 0 || channel_count <= 0 || channel_count > 16) {
        LOGE("Invalid callback args: numFrames=%d, channelCount=%d", numFrames, channel_count);
        g_player.rt_error.publish("[STREAM] Invalid callback args from stream");
        g_player.is_playing.store(false, std::memory_order_release);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    // Get bytes per sample based on format
    const int32_t bytes_per_sample = aaudio_common::bytesPerSample(AAudioStream_getFormat(stream));

    // Calculate bytes to read
    int32_t bytes_to_read = numFrames * channel_count * bytes_per_sample;

    const size_t bytes_read =
        g_player.ring->read(static_cast<char*>(audioData), static_cast<size_t>(bytes_to_read));

    // Only pad trailing silence: zero-fill on underrun or a short tail to avoid stale data (no cost when full)
    if (bytes_read < static_cast<size_t>(bytes_to_read)) {
        memset(static_cast<char*>(audioData) + bytes_read, 0, bytes_to_read - bytes_read);
    }

    if (bytes_read == 0) {
        // Buffer empty: data fully played (EOF) or the reader thread hasn't filled it yet (underrun, output silence this time).
        if (g_player.eof_reached.load(std::memory_order_acquire)) {
            // Termination notification (stopped/error) is delivered by the reader thread on a non-RT thread; here just return STOP
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
        g_player.underrun_count.fetch_add(1, std::memory_order_relaxed);
    }

#if LATENCY_TEST_ENABLE
    if (g_player.latency_test_enabled.load(std::memory_order_relaxed)) {
        // Single source of truth: block number derives GPIO and mute; no independent state to drift
        const int count = g_player.write_counter.fetch_add(1);   // 0-based
        const int block = count / LATENCY_TEST_INTERVAL;
        const bool muted = (block & 1) == 0;                     // even blocks muted
        if (count % LATENCY_TEST_INTERVAL == 0) {                // block start (count=0 is the first)
            const int gpio = (block + 1) & 1;                    // +1 aligns with init's low level: block0 writes 1 to produce the first edge
            if (!writeGpioValue(gpio)) {
                // GPIO failure: reference lost, disable the test to avoid muting audio into silence
                g_player.latency_test_enabled.store(false, std::memory_order_relaxed);
                closeGpio();
            } else if (block % 1000 == 0) {                      // log once every 1000 blocks
                LOGD("Latency test: count=%d, gpio=%d, mute=%d", count, gpio, muted ? 1 : 0);
            }
        }
        if (muted) {
            memset(audioData, 0, bytes_to_read);
        }
    }
#endif

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void errorCallback(AAudioStream* stream, void* userData, aaudio_result_t error) {
    LOGE("AAudio error: %s", AAudio_convertResultToText(error));
    g_player.is_playing.store(false, std::memory_order_release);
    // Same RT discipline as the data callback: flag only, no allocation, no JNI. The message is composed into
    // rt_error.buf (no heap) and published via rt_error.publish; the reader thread delivers to Java. After notifying,
    // Fragment.onError cleans up via stop() (close stream, join reader thread).
    // If the reader has already exited (stop path joined it), the flag stays unconsumed and the session ends as
    // onStopped instead of onError — acceptable, the session is ending anyway.
    snprintf(g_player.rt_error.buf, sizeof(g_player.rt_error.buf), "[STREAM] Playback stream error: %s",
             AAudio_convertResultToText(error));
    g_player.rt_error.publish(g_player.rt_error.buf);
}

static bool createAAudioStream() {
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
        LOGE("Failed to create builder: %s", AAudio_convertResultToText(result));
        return false;
    }

    int32_t sample_rate = 48000;
    int32_t channel_count = 2;
    aaudio_format_t format = AAUDIO_FORMAT_PCM_I16;

    if (g_player.wav_file && g_player.wav_file->isOpen()) {
        sample_rate = g_player.wav_file->getSampleRate();
        channel_count = g_player.wav_file->getChannelCount();
        format = static_cast<aaudio_format_t>(g_player.wav_file->getAAudioFormat());
    }

    AAudioStreamBuilder_setSampleRate(builder, sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, channel_count);
    AAudioStreamBuilder_setFormat(builder, format);
    AAudioStreamBuilder_setUsage(builder, g_player.usage);
    AAudioStreamBuilder_setContentType(builder, g_player.content_type);
    AAudioStreamBuilder_setSharingMode(builder, g_player.sharing_mode);
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(builder, g_player.performance_mode);

    AAudioStreamBuilder_setBufferCapacityInFrames(
        builder, aaudio_common::bufferCapacityFrames(sample_rate, g_player.performance_mode));

    AAudioStreamBuilder_setDataCallback(builder, audioCallback, nullptr);
    AAudioStreamBuilder_setErrorCallback(builder, errorCallback, nullptr);

    AAudioStream* raw_stream = nullptr;
    result = AAudioStreamBuilder_openStream(builder, &raw_stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) {
        LOGE("Failed to open stream: %s", AAudio_convertResultToText(result));
        return false;
    }
    g_player.stream.reset(raw_stream);

    aaudio_common::optimizeBufferSize(g_player.stream.get(), g_player.performance_mode);

    int32_t actual_rate = AAudioStream_getSampleRate(g_player.stream.get());
    int32_t actual_channels = AAudioStream_getChannelCount(g_player.stream.get());
    aaudio_format_t actual_format = AAudioStream_getFormat(g_player.stream.get());

    LOGI("Stream created: %dHz, %dch, format=%d, mode=%d", actual_rate, actual_channels,
         actual_format, AAudioStream_getPerformanceMode(g_player.stream.get()));

    // AAudio has no getMinBufferSize: minBurst=AAudioStream_getFramesPerBurst is the minimum buffer unit,
    // bufferSize=actual buffer (optimizeBufferSize set it to 2x/4x burst), capacity=upper limit.
    int32_t frames_per_burst = AAudioStream_getFramesPerBurst(g_player.stream.get());
    int32_t buffer_size_frames = AAudioStream_getBufferSizeInFrames(g_player.stream.get());
    int32_t capacity_frames = AAudioStream_getBufferCapacityInFrames(g_player.stream.get());
    const int32_t frame_bytes = aaudio_common::bytesPerSample(actual_format) * actual_channels;
    LOGI("AAudio buffer info: minBurst=%d frames (%d B, %.1fms) | bufferSize=%d frames (%d B, %.1fms) | capacity=%d frames (%.1fms)",
         frames_per_burst, frames_per_burst * frame_bytes, frames_per_burst * 1000.0 / actual_rate,
         buffer_size_frames, buffer_size_frames * frame_bytes, buffer_size_frames * 1000.0 / actual_rate,
         capacity_frames, capacity_frames * 1000.0 / actual_rate);

    // If the stream's actual channels/format differ from the WAV, treat as an error: the callback
    // consumes bytes in stream format while the ring holds file-format layout; a mismatch would cause
    // pitch shift/noise without notice (the recorder side avoids this by writing back actual values).
    if (actual_channels != channel_count || actual_format != format) {
        LOGE("Stream params mismatch: requested %dch/fmt%d, actual %dch/fmt%d", channel_count,
             format, actual_channels, actual_format);
        g_player.stream.reset();
        return false;
    }
    // Sample rate is resampled automatically by the framework; warn only (some devices return the resampled rate from openStream)
    if (actual_rate != sample_rate) {
        LOGW("Sample rate adapted: %d requested, %d actual", sample_rate, actual_rate);
    }

    return true;
}

extern "C" {

JNIEXPORT jboolean JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_initializeNative(JNIEnv* env,
                                                                                              jobject thiz) {
    LOGI("initializeNative");

    if (!isPlayerIdle()) {
        LOGE("Cannot initialize while playing");
        return JNI_FALSE;
    }

    // Join-before-rebind: the idle gate already passes while a leftover worker (an EOF/error path
    // set is_playing false) is still delivering its final notification. Join it here so its JNI
    // callbacks ran against the OLD binding — a rebind can never redirect a late notification onto
    // the new instance. Idempotent when no worker is left.
    stopReadThread();

    jclass clazz = aaudio_common::bindJavaInstance(env, thiz, g_player.player_instance, "player");
    if (!clazz) {
        return JNI_FALSE;
    }

    g_player.on_playback_started_method = env->GetMethodID(clazz, "onNativePlaybackStarted", "()V");
    g_player.on_playback_stopped_method = env->GetMethodID(clazz, "onNativePlaybackStopped", "()V");
    g_player.on_playback_error_method = env->GetMethodID(clazz, "onNativePlaybackError", "(Ljava/lang/String;)V");

    env->DeleteLocalRef(clazz);

    if (!g_player.on_playback_started_method || !g_player.on_playback_stopped_method ||
        !g_player.on_playback_error_method) {
        LOGE("Failed to get callback method IDs");
        if (g_player.player_instance) {
            env->DeleteGlobalRef(g_player.player_instance);
            g_player.player_instance = nullptr;
        }
        return JNI_FALSE;
    }

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_setNativeConfig(
    JNIEnv* env, jobject thiz, jint usage, jint contentType, jint performanceMode, jint sharingMode, jstring filePath) {
    LOGI("setNativeConfig");

    if (!isPlayerIdle()) {
        LOGE("Cannot change config while playing");
        return JNI_FALSE;
    }

    // All-or-nothing: fetching the path is the only fallible step, so do it before mutating any
    // field — on failure the config is untouched and the Kotlin side correctly keeps its copy
    if (filePath) {
        const char* path = env->GetStringUTFChars(filePath, nullptr);
        if (!path) {
            LOGE("Failed to get file path string");
            return JNI_FALSE;
        }
        g_player.audio_file_path = std::string(path);
        env->ReleaseStringUTFChars(filePath, path);
    }

    g_player.usage = static_cast<aaudio_usage_t>(usage);
    g_player.content_type = static_cast<aaudio_content_type_t>(contentType);
    g_player.performance_mode = static_cast<aaudio_performance_mode_t>(performanceMode);
    g_player.sharing_mode = static_cast<aaudio_sharing_mode_t>(sharingMode);

    LOGI("Config updated: usage=%d, contentType=%d, performanceMode=%d, sharingMode=%d, file=%s", g_player.usage,
         g_player.content_type, g_player.performance_mode, g_player.sharing_mode, g_player.audio_file_path.c_str());

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_startNativePlayback(JNIEnv* env,
                                                                                                 jobject thiz) {
    LOGI("startNativePlayback");

    // Unreachable via Java: state and this flag are updated synchronously by notifications on the
    // same executor thread, so the Kotlin-side gate is an exact mirror of this one. Returning
    // false without notifying is therefore safe (no stuck-UI path exists).
    if (g_player.is_playing.load(std::memory_order_acquire)) {
        return JNI_FALSE;
    }

    stopReadThread();  // Clean up the leftover/unjoined reader thread from the last EOF/error, avoiding reassigning a joinable thread (which would terminate)

    g_player.callback_notified.store(false, std::memory_order_release);

    g_player.wav_file = std::make_unique<WavFile>();
    if (!g_player.wav_file->openRead(g_player.audio_file_path)) {
        LOGE("Failed to open: %s", g_player.audio_file_path.c_str());
        g_player.wav_file.reset();
        notifyPlaybackError("[FILE] Cannot open audio file");
        return JNI_FALSE;
    }

    if (!createAAudioStream()) {
        g_player.wav_file.reset();
        notifyPlaybackError("[STREAM] Failed to create playback stream");
        return JNI_FALSE;
    }

    // Ring buffer + reader thread: the file streams into the buffer, the callback only memcpys (disk I/O moved off the real-time thread).
    g_player.ring = std::make_unique<SpScRingBuffer>(kDefaultRingCapacity);
    g_player.eof_reached.store(false, std::memory_order_release);
    g_player.stop_read_thread.store(false, std::memory_order_release);
    g_player.underrun_count.store(0, std::memory_order_relaxed);
    // Clear an error left by the last session's final in-flight callback (set after the old reader exited), or the new reader would consume it and kill this session
    g_player.rt_error.clear();
    // Pre-fill one chunk so the first callback (it races ahead of the reader's first disk read)
    // never hits an empty ring (underrun). Must precede thread spawn — the file position is shared.
    std::vector<char> prefill(readChunkBytes());
    const size_t n = g_player.wav_file->readAudioData(prefill.data(), prefill.size());
    if (n == 0) {
        // Zero bytes at open = first-chunk I/O failure, or a data section that is actually empty
        // (only reachable via the streaming-WAV size fallback). Spawning the reader would run its
        // EOF/error teardown (notify + stream reset) concurrently with requestStart below —
        // close the stream / consume the notification latch mid-start. Fail here instead:
        // no reader thread, no race, the error notification is delivered synchronously.
        const bool read_failed = g_player.wav_file->hasReadError();
        g_player.stream.reset();
        g_player.ring.reset();
        g_player.wav_file.reset();
        notifyPlaybackError(read_failed ? "[FILE] Audio read failed or file truncated"
                                        : "[FILE] Audio file contains no audio data");
        return JNI_FALSE;
    }
    g_player.ring->write(prefill.data(), n);
    g_player.read_thread = std::thread(playReadThread);

#if LATENCY_TEST_ENABLE
    if (!initGpio()) {
        LOGE("Failed to initialize GPIO for latency test - latency test DISABLED");
        g_player.latency_test_enabled.store(false);
    } else {
        g_player.write_counter.store(0);
        g_player.latency_test_enabled.store(true);

        writeGpioValue(0);
        LOGI("Latency test initialized: GPIO=%s, interval=%d", LATENCY_TEST_GPIO_FILE, LATENCY_TEST_INTERVAL);
    }
#endif

    g_player.is_playing.store(true, std::memory_order_release);
    aaudio_result_t result = AAudioStream_requestStart(g_player.stream.get());

    if (result != AAUDIO_OK) {
        LOGE("Failed to start: %s", AAudio_convertResultToText(result));
        g_player.is_playing.store(false, std::memory_order_release);
        stopReadThread();
        g_player.stream.reset();
        g_player.wav_file.reset();
        g_player.ring.reset();
        notifyPlaybackError("[STREAM] Failed to start playback stream");
        return JNI_FALSE;
    }

    notifyPlaybackStarted();
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_stopNativePlayback(JNIEnv* env,
                                                                                             jobject thiz) {
    LOGI("stopNativePlayback");

    g_player.is_playing.store(false, std::memory_order_release);
    stopReadThread();

    if (g_player.stream) {
        aaudio_common::stopStreamAndWait(g_player.stream.get());
        g_player.stream.reset();
    }

    g_player.wav_file.reset();
    g_player.ring.reset();

    if (g_player.underrun_count.load(std::memory_order_relaxed) > 0) {
        LOGW("Playback underruns: %zu", g_player.underrun_count.load(std::memory_order_relaxed));
    }

#if LATENCY_TEST_ENABLE
    closeGpio();
#endif

    // One-shot latch: only the first claimer notifies stopped (skipped if error/EOF already set), avoiding double notification
    if (!g_player.callback_notified.exchange(true, std::memory_order_acq_rel)) {
        if (!notifyPlaybackStopped()) {
            // Session is over either way; callVoidMethod already logged the reason and the latch
            // is reset by the next start — this log just makes a lost UI update attributable
            LOGW("Stopped notification could not be delivered");
        }
    }
}

JNIEXPORT void JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_releaseNative(JNIEnv* env, jobject thiz) {
    LOGI("Releasing AAudio player");

    // Stale-release guard: a newer instance has taken over the binding (rotation rebuild) —
    // the stream/wav state and the reference below belong to it, not to this dead instance.
    // (A null binding cannot coexist with live state: release clears both, and initialize's
    // failure path clears the binding only while no state exists.)
    if (g_player.player_instance && !env->IsSameObject(thiz, g_player.player_instance)) {
        LOGW("Stale release ignored: binding owned by a newer instance");
        return;
    }

    // Clean up stream/WAV resources even if playback already ended (e.g. an error path whose queued
    // stop() has not run yet). stopNativePlayback resets stream/wav_file to null, so this condition
    // also avoids re-running it after a normal stop.
    if (g_player.stream || g_player.wav_file) {
        Java_com_example_aaudiotester_player_AAudioPlayer_stopNativePlayback(env, thiz);
    }

    // Binding is either unset or still ours here (the stale-release guard above returns on any
    // takeover, and initialize cannot interleave — all native entry is FIFO on the shared
    // executor): safe to clear the reference and method IDs.
    if (g_player.player_instance) {
        env->DeleteGlobalRef(g_player.player_instance);
        g_player.player_instance = nullptr;
        g_player.on_playback_started_method = nullptr;
        g_player.on_playback_stopped_method = nullptr;
        g_player.on_playback_error_method = nullptr;
    }

    LOGI("AAudioPlayer released");
}

}  // extern "C"
