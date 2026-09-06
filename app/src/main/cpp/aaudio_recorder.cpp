#include "aaudio_recorder.h"
#include "ring_buffer.h"
#include "wav_file.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <aaudio/AAudio.h>

#include "aaudio_common.h"

/**
 * Global audio recorder state
 *
 * Design decision: Using global state because:
 * 1. Single instance per application (audio recording hardware is exclusive resource)
 * 2. JNI library is loaded once per process
 * 3. Simpler and more performant than instance management
 * 4. No requirement for multiple simultaneous recording instances
 *
 * Thread safety: Protected by atomic operations and JNI calls from Java layer
 * All state modifications are serialized through JNI method calls
 */
struct AudioRecorderState {
    aaudio_common::AAudioStreamPtr stream;
    std::unique_ptr<WavFile> wav_file;
    std::atomic<bool> is_recording{false};

    // Recording data buffer: callback (producer) → writer thread (consumer), moving disk I/O out of the real-time callback.
    std::unique_ptr<SpScRingBuffer> ring;
    std::thread write_thread;
    std::atomic<bool> stop_write_thread{false};
    std::atomic<size_t> dropped_bytes{0};
    // Terminal error set by the RT callback (literals in the data/error callbacks), delivered to
    // Java by the writer thread on a non-RT thread (protocol in RtErrorSlot)
    aaudio_common::RtErrorSlot rt_error;

    // Java notification endpoint: listener binding + one-shot latch (mechanics in aaudio_common)
    aaudio_common::JavaNotifier notifier;

    aaudio_input_preset_t input_preset = AAUDIO_INPUT_PRESET_GENERIC;
    int32_t sample_rate = 48000;
    int32_t channel_count = 1;
    aaudio_format_t format = AAUDIO_FORMAT_PCM_I16;
    aaudio_performance_mode_t performance_mode = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
    aaudio_sharing_mode_t sharing_mode = AAUDIO_SHARING_MODE_SHARED;
    std::string output_path = "/data/recorded_48k_1ch_16bit.wav";
    bool auto_generate_filename = false;
};

namespace {

    AudioRecorderState g_recorder;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

    // Not recording: gate for initialize/setConfig — native state must not change mid-session
    bool isRecorderIdle() {
        return !g_recorder.is_recording.load(std::memory_order_acquire);
    }

}  // namespace

void aaudio_recorder_set_jvm(JavaVM* vm) {
    g_recorder.notifier.jvm = vm;
}

// Stop the writer thread and join (the writer drains remaining buffered data to disk before joining).
static void stopWriteThread() {
    g_recorder.stop_write_thread.store(true, std::memory_order_release);
    if (g_recorder.write_thread.joinable()) {
        g_recorder.write_thread.join();
    }
}

// Recording writer thread: pulls data from the ring buffer to disk; the callback thread only memcpys and never touches disk.
static void recordWriteThread() {
    std::vector<char> buf(64 * 1024);
    size_t unsaved_bytes = 0;
    bool save_failed = false;
    // Shared by the main loop and the exit drain: after one failure the stream carries failbit, so
    // skipping retries is free; recording continues but data stops being saved.
    // The actual saved amount is recorded by the WAV header backfilled in close(); no need to count here.
    auto writeOut = [&](const char *data, size_t n) {
        if (save_failed || !g_recorder.wav_file->writeData(data, n)) {
            if (!save_failed) {
                save_failed = true;
                LOGE("File write failed - recording continues without saving");
            }
            unsaved_bytes += n;
        }
    };
    while (!g_recorder.stop_write_thread.load(std::memory_order_acquire)) {
        // Terminal error set by the RT callback: delivered here to avoid JNI on the callback thread
        if (consumeRtError(g_recorder.rt_error, g_recorder.is_recording, g_recorder.notifier, "recording error")) {
            // break, not return: the ring still holds audio captured before the stream died —
            // valid data, so the exit-drain below saves it. The stop path joins this thread
            // before closing the WAV file, so the drained tail lands in a consistent file.
            break;
        }
        const size_t n = g_recorder.ring->read(buf.data(), buf.size());
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        writeOut(buf.data(), n);
    }
    // Drain remaining data before exit to avoid losing the tail.
    size_t n;
    while ((n = g_recorder.ring->read(buf.data(), buf.size())) > 0) {
        writeOut(buf.data(), n);
    }
    // After draining, deliver an error the RT callback may have set during the stop race window
    consumeRtError(g_recorder.rt_error, g_recorder.is_recording, g_recorder.notifier, "recording error");
    if (save_failed) {
        LOGW("Recording finished: %zu bytes not saved (file write failed)", unsaved_bytes);
    }
}

static std::string generateTimestampedFilePath() {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    auto now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_buf;
    localtime_r(&now_time_t, &tm_buf);

    std::ostringstream oss;
    oss << g_recorder.output_path << "/";
    oss << "rec_";
    oss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S");
    oss << "_" << std::setfill('0') << std::setw(3) << now_ms.count();
    oss << "_" << (g_recorder.sample_rate / 1000) << "k";
    oss << "_" << g_recorder.channel_count << "ch";

    // Bit depth = bytes per sample × 8 (same source as the wav_file header derivation; the format→bit-depth mapping is unique)
    oss << "_" << (aaudio_common::bytesPerSample(g_recorder.format) * 8) << "bit";
    oss << ".wav";

    return oss.str();
}

static std::string getFinalRecordingFilePath() {
    if (g_recorder.auto_generate_filename) {
        return generateTimestampedFilePath();
    }
    return g_recorder.output_path;
}

static aaudio_data_callback_result_t audioCallback(AAudioStream *stream,
                                                   void *userData,
                                                   void *audioData,
                                                   int32_t numFrames) {
    if (!g_recorder.is_recording.load(std::memory_order_acquire)) {
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    if (!g_recorder.wav_file || !g_recorder.wav_file->isOpen()) {
        // Only set the flag; JNI notification is delivered by the writer thread on a non-RT thread (no JNI on the callback thread)
        LOGE("WAV file not available");
        g_recorder.rt_error.publish("[FILE] WAV file not opened");
        g_recorder.is_recording.store(false, std::memory_order_release);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    // Callback argument guard: clear state and set the flag, matching the file-error branch above (a bare return would leave the UI stuck on "in progress")
    int32_t channel_count = AAudioStream_getChannelCount(stream);
    if (numFrames <= 0 || channel_count <= 0 || channel_count > 16) {
        LOGE("Invalid callback args: numFrames=%d, channelCount=%d", numFrames, channel_count);
        g_recorder.rt_error.publish("[STREAM] Invalid callback args from stream");
        g_recorder.is_recording.store(false, std::memory_order_release);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    // Get bytes per sample based on format
    const int32_t bytes_per_sample = aaudio_common::bytesPerSample(AAudioStream_getFormat(stream));

    // Calculate bytes to write
    int32_t bytes_to_write = numFrames * channel_count * bytes_per_sample;

    // Whole frames either fully enter the buffer or are dropped (drop rather than block); disk writes are handled by the writer thread.
    if (!g_recorder.ring->tryWrite(static_cast<const char*>(audioData), static_cast<size_t>(bytes_to_write))) {
        g_recorder.dropped_bytes.fetch_add(static_cast<size_t>(bytes_to_write), std::memory_order_relaxed);
    }

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void errorCallback(AAudioStream *stream, void *userData, aaudio_result_t error) {
    LOGE("AAudio error callback: %s", AAudio_convertResultToText(error));
    g_recorder.is_recording.store(false, std::memory_order_release);
    // Same RT discipline as the data callback: flag only, no allocation, no JNI. A static
    // literal is published (the specific result text is in the LOGE line above); the writer
    // thread delivers to Java. After notifying,
    // Fragment.onError cleans up via stop() (close stream, join writer thread, backfill WAV header).
    g_recorder.rt_error.publish("[STREAM] Recording stream error");
}

static bool createAAudioStream() {
    AAudioStreamBuilder *builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);

    if (result != AAUDIO_OK) {
        LOGE("Failed to create stream builder: %s", AAudio_convertResultToText(result));
        return false;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(builder, g_recorder.sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, g_recorder.channel_count);
    AAudioStreamBuilder_setFormat(builder, g_recorder.format);
    AAudioStreamBuilder_setPerformanceMode(builder, g_recorder.performance_mode);
    AAudioStreamBuilder_setSharingMode(builder, g_recorder.sharing_mode);
    AAudioStreamBuilder_setInputPreset(builder, g_recorder.input_preset);

    // Set buffer capacity based on performance mode
    AAudioStreamBuilder_setBufferCapacityInFrames(
            builder, aaudio_common::bufferCapacityFrames(g_recorder.sample_rate, g_recorder.performance_mode));

    AAudioStreamBuilder_setDataCallback(builder, audioCallback, nullptr);
    AAudioStreamBuilder_setErrorCallback(builder, errorCallback, nullptr);

    AAudioStream *raw_stream = nullptr;
    result = AAudioStreamBuilder_openStream(builder, &raw_stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) {
        LOGE("Failed to open recording stream: %s", AAudio_convertResultToText(result));
        return false;
    }
    aaudio_common::adoptStream(g_recorder.stream, raw_stream, g_recorder.rt_error);

    aaudio_common::optimizeBufferSize(g_recorder.stream.get(), g_recorder.performance_mode);

    int32_t actual_sample_rate = AAudioStream_getSampleRate(g_recorder.stream.get());
    int32_t actual_channel_count = AAudioStream_getChannelCount(g_recorder.stream.get());
    aaudio_format_t actual_format = AAudioStream_getFormat(g_recorder.stream.get());

    LOGI("Recording stream created - Sample Rate: %d, Channels: %d, Format: %d, Buffer: %d frames",
         actual_sample_rate,
         actual_channel_count, actual_format,
         AAudioStream_getBufferCapacityInFrames(g_recorder.stream.get()));

    g_recorder.sample_rate = actual_sample_rate;
    g_recorder.channel_count = actual_channel_count;
    g_recorder.format = actual_format;

    return true;
}

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_example_aaudiotester_recorder_AAudioRecorder_initializeNative(JNIEnv *env,
                                                                         jobject thiz) {
    LOGI("initializeNative");

    if (!isRecorderIdle()) {
        LOGE("Cannot initialize while recording");
        return JNI_FALSE;
    }

    // A leftover writer (in its final stretch after an error path set is_recording false) is
    // joined by the bind's quiesce, idempotently.
    if (!g_recorder.notifier.bind(env, thiz, "recorder",
                                  "onNativeRecordingStopped", "onNativeRecordingError",
                                  stopWriteThread)) {
        return JNI_FALSE;
    }

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_example_aaudiotester_recorder_AAudioRecorder_setNativeConfig(JNIEnv *env,
                                                                        jobject thiz,
                                                                        jint inputPreset,
                                                                        jint sampleRate,
                                                                        jint channelCount,
                                                                        jint format,
                                                                        jint performanceMode,
                                                                        jint sharingMode,
                                                                        jstring audioFilePath) {
    LOGI("setNativeConfig");

    if (!isRecorderIdle()) {
        LOGE("Cannot change config while recording");
        return JNI_FALSE;
    }

    if (audioFilePath == nullptr) {
        LOGE("Audio file path is null");
        return JNI_FALSE;
    }

    // All-or-nothing: fetching the path is the only fallible step, so do it before mutating any
    // field — on failure the config is untouched and the Kotlin side correctly keeps its copy
    const char *path_str = env->GetStringUTFChars(audioFilePath, nullptr);
    if (path_str == nullptr) {
        LOGE("Failed to get audio file path string");
        return JNI_FALSE;
    }

    g_recorder.input_preset = static_cast<aaudio_input_preset_t>(inputPreset);
    g_recorder.sample_rate = sampleRate;
    g_recorder.channel_count = channelCount;
    g_recorder.format = static_cast<aaudio_format_t>(format);
    g_recorder.performance_mode = static_cast<aaudio_performance_mode_t>(performanceMode);
    g_recorder.sharing_mode = static_cast<aaudio_sharing_mode_t>(sharingMode);

    g_recorder.output_path = path_str;
    // Check if path ends with .wav - if not, it's a directory path and we need to generate filename
    g_recorder.auto_generate_filename =
            (g_recorder.output_path.length() < 4 ||
             g_recorder.output_path.substr(g_recorder.output_path.length() - 4) != ".wav");
    env->ReleaseStringUTFChars(audioFilePath, path_str);

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_example_aaudiotester_recorder_AAudioRecorder_startNative(JNIEnv *env,
                                                                             jobject thiz) {
    // Unreachable via Java: state and this flag are updated synchronously by notifications on the
    // same executor thread, so the Kotlin-side gate is an exact mirror of this one. Returning
    // false without notifying is therefore safe (no stuck-UI path exists).
    if (g_recorder.is_recording.load(std::memory_order_acquire)) {
        LOGW("Already recording");
        return JNI_FALSE;
    }

    LOGI("startNative");

    stopWriteThread();  // Clean up the leftover/unjoined writer thread from the last error (drain old data first, then rebuild resources)

    g_recorder.notifier.release();  // drop the previous session's final in-flight claim

    if (!createAAudioStream()) {
        g_recorder.notifier.deliverErrorOnce("[STREAM] Failed to create recording stream",
                                             "recording error");
        return JNI_FALSE;
    }

    std::string file_path = getFinalRecordingFilePath();
    g_recorder.wav_file = std::make_unique<WavFile>();

    if (!g_recorder.wav_file->openWrite(file_path, g_recorder.sample_rate, g_recorder.channel_count,
                                        g_recorder.format)) {
        LOGE("Failed to open WAV file: %s", file_path.c_str());
        g_recorder.stream.reset();
        g_recorder.wav_file.reset();
        g_recorder.notifier.deliverErrorOnce("[FILE] Failed to create recording file",
                                             "recording error");
        return JNI_FALSE;
    }

    // Ring buffer + writer thread: the callback only memcpys into the buffer; the writer thread handles disk writes.
    g_recorder.ring = std::make_unique<SpScRingBuffer>(kDefaultRingCapacity);
    g_recorder.stop_write_thread.store(false, std::memory_order_release);
    g_recorder.dropped_bytes.store(0, std::memory_order_relaxed);
    g_recorder.write_thread = std::thread(recordWriteThread);

    // Set recording flag before starting stream to avoid race condition with callback
    g_recorder.is_recording.store(true, std::memory_order_release);

    aaudio_result_t result = AAudioStream_requestStart(g_recorder.stream.get());
    if (result != AAUDIO_OK) {
        LOGE("Failed to start recording stream: %s", AAudio_convertResultToText(result));
        g_recorder.is_recording.store(false, std::memory_order_release);
        stopWriteThread();
        g_recorder.stream.reset();
        if (!g_recorder.wav_file->close()) {
            // Header backfill failed: the leftover file is malformed. Recording never started, so
            // there is nothing to report to the user — the log is all we have
            LOGE("WAV finalize failed on aborted start");
        }
        g_recorder.wav_file.reset();
        g_recorder.ring.reset();
        g_recorder.notifier.deliverErrorOnce("[STREAM] Failed to start recording stream",
                                             "recording error");
        return JNI_FALSE;
    }

    // No started notification: Kotlin derives "recording" from this JNI_TRUE return — the
    // synchronous result of this very call, on the same executor thread. JNI callbacks stay
    // reserved for asynchronous events delivered from other threads (stopped/error), which cannot
    // ride the return value; a delivery-failure fallback for started would share every failure
    // condition with the primary call and thus be dead code.
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_example_aaudiotester_recorder_AAudioRecorder_stopNative(JNIEnv *env,
                                                                            jobject thiz) {
    LOGI("stopNative");

    g_recorder.is_recording.store(false, std::memory_order_release);

    if (g_recorder.stream) {
        aaudio_common::stopStreamAndWait(g_recorder.stream.get());
        g_recorder.stream.reset();
    }

    stopWriteThread();  // Join the writer thread: drain remaining buffered data to disk

    if (g_recorder.wav_file) {
        const bool truncated = g_recorder.wav_file->hasTruncatedWrite();
        const bool finalized = g_recorder.wav_file->close();
        g_recorder.wav_file.reset();
        if (!finalized) {
            // Header backfill failed: recording data wasn't fully saved; report as an error rather than faking a normal finish
            // (a lost delivery is attributed by deliverErrorOnce itself)
            g_recorder.notifier.deliverErrorOnce(
                "[FILE] Failed to finalize recording file (header write failed)", "recording error");
        } else if (truncated) {
            // File is valid but incomplete (causes are mutually exclusive, the logcat LOGE tells
            // which): report instead of faking a full take. [TRUNC] is a distinct token (not [FILE])
            // so the Kotlin side matches on the protocol token, never on this message's wording
            g_recorder.notifier.deliverErrorOnce("[TRUNC] Recording incomplete", "recording error");
        }
    }
    g_recorder.ring.reset();

    if (g_recorder.dropped_bytes.load(std::memory_order_relaxed) > 0) {
        LOGW("Recording dropped bytes: %zu", g_recorder.dropped_bytes.load(std::memory_order_relaxed));
    }

    // One-shot latch: only the first claimer notifies stopped (skipped if error/EOF already set).
    // Return-value contract: see JavaNotifier::deliverStoppedOnce.
    return g_recorder.notifier.deliverStoppedOnce("recording stopped") ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_example_aaudiotester_recorder_AAudioRecorder_releaseNative(JNIEnv *env,
                                                                      jobject thiz) {
    LOGI("releaseNative");

    if (!g_recorder.notifier.releaseSession(
            env, thiz, g_recorder.stream || g_recorder.wav_file,
            [&] { Java_com_example_aaudiotester_recorder_AAudioRecorder_stopNative(env, thiz); })) {
        return;  // Stale release: a newer instance owns the binding and the state below it
    }

    LOGI("AAudioRecorder released");
}

}  // extern "C"
