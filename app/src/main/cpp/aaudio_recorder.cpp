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
    std::atomic<bool> callback_notified{false};  // true if Java already notified by callback

    // Recording data buffer: callback (producer) → writer thread (consumer), moving disk I/O out of the real-time callback.
    std::unique_ptr<SpScRingBuffer> ring;
    std::thread write_thread;
    std::atomic<bool> stop_write_thread{false};
    std::atomic<size_t> dropped_bytes{0};
    // Terminal error set by the RT callback (literals in the data/error callbacks), delivered to
    // Java by the writer thread on a non-RT thread (protocol in RtErrorSlot)
    aaudio_common::RtErrorSlot rt_error;

    JavaVM *jvm = nullptr;
    jobject recorder_instance = nullptr;
    jmethodID on_recording_stopped_method = nullptr;
    jmethodID on_recording_error_method = nullptr;

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
    g_recorder.jvm = vm;
}

static bool notifyRecordingStopped() {
    return aaudio_common::callVoidMethod(g_recorder.jvm, g_recorder.recorder_instance,
                                         g_recorder.on_recording_stopped_method, "recording stopped");
}

// Returns false when the error was NOT delivered to Java (dedup-swallowed by an earlier notice,
// or delivery failed) so callers with a genuine loss to report can keep it attributable in logcat.
static bool notifyRecordingError(const std::string &error) {
    // Atomic dedup: the rt_error path and errorCallback may deliver concurrently; notify only once.
    // If already set (stopped/error), ignore; subsequent stopNativeRecording also won't resend onStopped over an error notice
    if (g_recorder.callback_notified.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    if (!aaudio_common::notifyErrorToJava(g_recorder.jvm, g_recorder.recorder_instance,
                                          g_recorder.on_recording_error_method, error, "recording error")) {
        // Delivery failed: release the latch so the stop path can still notify (duplicate beats lost —
        // a consumed latch with no delivery would leave the UI stuck in RECORDING forever)
        g_recorder.callback_notified.store(false, std::memory_order_release);
        return false;
    }
    return true;
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
        if (const char* msg = g_recorder.rt_error.take()) {
            g_recorder.is_recording.store(false, std::memory_order_release);
            notifyRecordingError(msg);
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
    if (const char* msg = g_recorder.rt_error.take()) {
        g_recorder.is_recording.store(false, std::memory_order_release);
        notifyRecordingError(msg);
    }
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
    // Same RT discipline as the data callback: flag only, no allocation, no JNI. The message is composed into
    // rt_error.buf (no heap) and published via rt_error.publish; the writer thread delivers to Java. After notifying,
    // Fragment.onError cleans up via stop() (close stream, join writer thread, backfill WAV header).
    snprintf(g_recorder.rt_error.buf, sizeof(g_recorder.rt_error.buf), "[STREAM] Recording stream error: %s",
             AAudio_convertResultToText(error));
    g_recorder.rt_error.publish(g_recorder.rt_error.buf);
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
    g_recorder.stream.reset(raw_stream);

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

    // Join-before-rebind: same invariant as the player's initializeNative — join a leftover
    // writer (in its final stretch after an error path set is_recording false) before rebinding,
    // so its late notifications always land on the instance they were created under. Idempotent.
    stopWriteThread();

    jclass clazz =
        aaudio_common::bindJavaInstance(env, thiz, g_recorder.recorder_instance, "recorder");
    if (!clazz) {
        return JNI_FALSE;
    }

    g_recorder.on_recording_stopped_method = env->GetMethodID(clazz, "onNativeRecordingStopped",
                                                              "()V");
    g_recorder.on_recording_error_method = env->GetMethodID(clazz, "onNativeRecordingError",
                                                            "(Ljava/lang/String;)V");

    if (!g_recorder.on_recording_stopped_method || !g_recorder.on_recording_error_method) {
        LOGE("Failed to get callback method IDs");
        env->DeleteLocalRef(clazz);
        if (g_recorder.recorder_instance) {
            env->DeleteGlobalRef(g_recorder.recorder_instance);
            g_recorder.recorder_instance = nullptr;
        }
        return JNI_FALSE;
    }

    env->DeleteLocalRef(clazz);
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
Java_com_example_aaudiotester_recorder_AAudioRecorder_startNativeRecording(JNIEnv *env,
                                                                             jobject thiz) {
    // Unreachable via Java: state and this flag are updated synchronously by notifications on the
    // same executor thread, so the Kotlin-side gate is an exact mirror of this one. Returning
    // false without notifying is therefore safe (no stuck-UI path exists).
    if (g_recorder.is_recording.load(std::memory_order_acquire)) {
        LOGW("Already recording");
        return JNI_FALSE;
    }

    LOGI("startNativeRecording");

    stopWriteThread();  // Clean up the leftover/unjoined writer thread from the last error (drain old data first, then rebuild resources)

    g_recorder.callback_notified.store(false, std::memory_order_release);

    if (!createAAudioStream()) {
        notifyRecordingError("[STREAM] Failed to create recording stream");
        return JNI_FALSE;
    }

    std::string file_path = getFinalRecordingFilePath();
    g_recorder.wav_file = std::make_unique<WavFile>();

    if (!g_recorder.wav_file->openWrite(file_path, g_recorder.sample_rate, g_recorder.channel_count,
                                        g_recorder.format)) {
        LOGE("Failed to open WAV file: %s", file_path.c_str());
        g_recorder.stream.reset();
        g_recorder.wav_file.reset();
        notifyRecordingError("[FILE] Failed to create recording file");
        return JNI_FALSE;
    }

    // Ring buffer + writer thread: the callback only memcpys into the buffer; the writer thread handles disk writes.
    g_recorder.ring = std::make_unique<SpScRingBuffer>(kDefaultRingCapacity);
    g_recorder.stop_write_thread.store(false, std::memory_order_release);
    g_recorder.dropped_bytes.store(0, std::memory_order_relaxed);
    // Clear an error left by the last session's final in-flight callback, or the new writer would
    // consume it and kill this session. Must follow createAAudioStream(): its stream.reset() is
    // what closes the old stream and joins its callbacks (same ordering as the player's clear,
    // which also happens after createAAudioStream has swapped in the new stream)
    g_recorder.rt_error.clear();
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
        notifyRecordingError("[STREAM] Failed to start recording stream");
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
Java_com_example_aaudiotester_recorder_AAudioRecorder_stopNativeRecording(JNIEnv *env,
                                                                            jobject thiz) {
    LOGI("stopNativeRecording");

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
            if (!notifyRecordingError("[FILE] Failed to finalize recording file (header write failed)")) {
                LOGW("Finalization failure could not be reported to the user (an earlier notice owns the latch)");
            }
        } else if (truncated) {
            // File is valid but incomplete (causes are mutually exclusive, the logcat LOGE tells
            // which): report instead of faking a full take. [TRUNC] is a distinct token (not [FILE])
            // so the Kotlin side matches on the protocol token, never on this message's wording
            if (!notifyRecordingError("[TRUNC] Recording incomplete")) {
                LOGW("Incomplete-recording report could not be delivered to the user (an earlier notice owns the latch)");
            }
        }
    }
    g_recorder.ring.reset();

    if (g_recorder.dropped_bytes.load(std::memory_order_relaxed) > 0) {
        LOGW("Recording dropped bytes: %zu", g_recorder.dropped_bytes.load(std::memory_order_relaxed));
    }

    // One-shot latch: only the first claimer notifies stopped (skipped if error/EOF already set),
    // same protocol as the player's stop path — claimed until the next start resets it.
    // Return-value contract (same protocol as start): JNI_TRUE = a notice already reached Kotlin
    // (delivered here, or the latch is owned by the error path); JNI_FALSE = our delivery
    // failed — Kotlin self-runs the onStopped completion to recover the UI.
    if (g_recorder.callback_notified.exchange(true, std::memory_order_acq_rel)) {
        return JNI_TRUE;  // error path owns the latch: the UI was already updated by its notice
    }
    if (notifyRecordingStopped()) {
        return JNI_TRUE;
    }
    // Session is over either way; callVoidMethod already logged the reason and the latch is reset
    // by the next start — JNI_FALSE just tells Kotlin to run the onStopped completion itself
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_example_aaudiotester_recorder_AAudioRecorder_releaseNative(JNIEnv *env,
                                                                      jobject thiz) {
    LOGI("releaseNative");

    // Stale-release guard: a newer instance has taken over the binding (rotation rebuild) —
    // the stream/wav state and the reference below belong to it, not to this dead instance.
    // (A null binding cannot coexist with live state: release clears both, and initialize's
    // failure path clears the binding only while no state exists.)
    if (g_recorder.recorder_instance && !env->IsSameObject(thiz, g_recorder.recorder_instance)) {
        LOGW("Stale release ignored: binding owned by a newer instance");
        return;
    }

    // Clean up stream/WAV resources even if recording already ended (e.g. a stream error set
    // is_recording false but left the WAV header unfinalized). stopNativeRecording resets
    // stream/wav_file, so this condition also avoids re-running it after a normal stop.
    if (g_recorder.stream || g_recorder.wav_file) {
        Java_com_example_aaudiotester_recorder_AAudioRecorder_stopNativeRecording(env, thiz);
    }

    // Binding is either unset or still ours here (the stale-release guard above returns on any
    // takeover, and initialize cannot interleave — all native entry is FIFO on the shared
    // executor): safe to clear the reference and method IDs.
    if (g_recorder.recorder_instance) {
        env->DeleteGlobalRef(g_recorder.recorder_instance);
        g_recorder.recorder_instance = nullptr;
        g_recorder.on_recording_stopped_method = nullptr;
        g_recorder.on_recording_error_method = nullptr;
    }

    LOGI("AAudioRecorder released");
}

}  // extern "C"
