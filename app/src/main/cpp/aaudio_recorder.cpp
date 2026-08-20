#include "aaudio_recorder.h"
#include "ring_buffer.h"
#include "wav_file.h"

#include <atomic>
#include <chrono>
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

    // 录音数据缓冲：回调(生产) → 写线程(消费)，把磁盘 I/O 移出实时回调。
    std::unique_ptr<SpScRingBuffer> ring;
    std::thread write_thread;
    std::atomic<bool> stop_write_thread{false};
    std::atomic<size_t> dropped_bytes{0};

    JavaVM *jvm = nullptr;
    jobject recorder_instance = nullptr;
    jmethodID on_recording_started_method = nullptr;
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

    bool validateRecorderState() {
        return !g_recorder.is_recording.load(std::memory_order_acquire);
    }

}  // namespace

void aaudio_recorder_set_jvm(JavaVM* vm) {
    g_recorder.jvm = vm;
}

static void notifyRecordingStarted() {
    aaudio_common::callVoidMethod(g_recorder.jvm, g_recorder.recorder_instance,
                                  g_recorder.on_recording_started_method, "recording started");
}

static void notifyRecordingStopped() {
    aaudio_common::callVoidMethod(g_recorder.jvm, g_recorder.recorder_instance,
                                  g_recorder.on_recording_stopped_method, "recording stopped");
}

static void notifyRecordingError(const std::string &error) {
    // 统一置位：错误已通知 Java，后续 stopNativeRecording 不再补发 onStopped 覆盖错误提示
    g_recorder.callback_notified.store(true, std::memory_order_release);
    aaudio_common::notifyErrorToJava(g_recorder.jvm, g_recorder.recorder_instance,
                                     g_recorder.on_recording_error_method, error, "recording error");
}

// 停止写线程并 join（join 前写线程会排空缓冲中剩余数据落盘）。
static void stopWriteThread() {
    g_recorder.stop_write_thread.store(true, std::memory_order_release);
    if (g_recorder.write_thread.joinable()) {
        g_recorder.write_thread.join();
    }
}

// 录音写线程：从环形缓冲取数据落盘；回调线程只 memcpy、永不碰磁盘。
static void recordWriteThread() {
    std::vector<char> buf(64 * 1024);
    size_t dropped_bytes = 0;
    bool save_failed = false;
    // 主循环与退出排空共用：失败一次后流带 failbit，跳过重试零开销；录音继续，数据不再落盘。
    // 实际保存量由 close() 回填的 WAV 头记录，无需在此重复统计。
    auto writeOut = [&](const char *data, size_t n) {
        if (save_failed || !g_recorder.wav_file->writeData(data, n)) {
            if (!save_failed) {
                save_failed = true;
                LOGE("File write failed - recording continues without saving");
            }
            dropped_bytes += n;
        }
    };
    while (!g_recorder.stop_write_thread.load(std::memory_order_acquire)) {
        const size_t n = g_recorder.ring->read(buf.data(), buf.size());
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        writeOut(buf.data(), n);
    }
    // 退出前排空剩余数据，避免丢尾部。
    size_t n;
    while ((n = g_recorder.ring->read(buf.data(), buf.size())) > 0) {
        writeOut(buf.data(), n);
    }
    if (save_failed) {
        LOGW("Recording finished: %zu bytes not saved (file write failed)", dropped_bytes);
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

    int bits_per_sample = 16;
    switch (g_recorder.format) {
        case AAUDIO_FORMAT_PCM_I16:
            bits_per_sample = 16;
            break;
        case AAUDIO_FORMAT_PCM_FLOAT:
            bits_per_sample = 32;
            break;
        case AAUDIO_FORMAT_PCM_I24_PACKED:
            bits_per_sample = 24;
            break;
        case AAUDIO_FORMAT_PCM_I32:
            bits_per_sample = 32;
            break;
        default:
            bits_per_sample = 16;
            break;
    }
    oss << "_" << bits_per_sample << "bit";
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
        LOGE("WAV file not available");
        g_recorder.is_recording.store(false, std::memory_order_release);
        notifyRecordingError("[FILE] WAV file not opened");
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    // 回调参数守卫：清状态并通知，与上方文件异常分支模式一致（原裸 return 会致 UI 卡“进行中”）
    int32_t channel_count = AAudioStream_getChannelCount(stream);
    if (numFrames <= 0 || channel_count <= 0 || channel_count > 16) {
        LOGE("Invalid callback args: numFrames=%d, channelCount=%d", numFrames, channel_count);
        g_recorder.is_recording.store(false, std::memory_order_release);
        notifyRecordingError("[STREAM] Invalid callback args from stream");
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    // Get bytes per sample based on format
    const int32_t bytes_per_sample = aaudio_common::bytesPerSample(AAudioStream_getFormat(stream));

    // Calculate bytes to write
    int32_t bytes_to_write = numFrames * channel_count * bytes_per_sample;

    // 整帧要么全进缓冲、要么全丢（宁丢不阻塞），落盘由写线程负责。
    if (!g_recorder.ring->tryWrite(static_cast<const char*>(audioData), static_cast<size_t>(bytes_to_write))) {
        g_recorder.dropped_bytes.fetch_add(static_cast<size_t>(bytes_to_write), std::memory_order_relaxed);
    }

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void errorCallback(AAudioStream *stream, void *userData, aaudio_result_t error) {
    LOGE("AAudio error callback: %s", AAudio_convertResultToText(error));
    g_recorder.is_recording.store(false, std::memory_order_release);
    // 通知 Java 后由 Fragment.onError 经 stop() 清理资源（关流、join 写线程、回填 WAV 头）
    std::string error_msg = "[STREAM] Recording stream error: ";
    error_msg += AAudio_convertResultToText(error);
    notifyRecordingError(error_msg);
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

    if (!validateRecorderState()) {
        LOGE("Cannot initialize while recording");
        return JNI_FALSE;
    }

    if (g_recorder.recorder_instance != nullptr) {
        env->DeleteGlobalRef(g_recorder.recorder_instance);
        g_recorder.recorder_instance = nullptr;
    }
    g_recorder.recorder_instance = env->NewGlobalRef(thiz);
    if (!g_recorder.recorder_instance) {
        LOGE("Failed to create global reference");
        return JNI_FALSE;
    }

    jclass clazz = env->GetObjectClass(thiz);
    if (clazz == nullptr) {
        LOGE("Failed to get object class");
        if (g_recorder.recorder_instance) {
            env->DeleteGlobalRef(g_recorder.recorder_instance);
            g_recorder.recorder_instance = nullptr;
        }
        return JNI_FALSE;
    }

    g_recorder.on_recording_started_method = env->GetMethodID(clazz, "onNativeRecordingStarted",
                                                              "()V");
    g_recorder.on_recording_stopped_method = env->GetMethodID(clazz, "onNativeRecordingStopped",
                                                              "()V");
    g_recorder.on_recording_error_method = env->GetMethodID(clazz, "onNativeRecordingError",
                                                            "(Ljava/lang/String;)V");

    if (!g_recorder.on_recording_started_method || !g_recorder.on_recording_stopped_method ||
        !g_recorder.on_recording_error_method) {
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

    if (!validateRecorderState()) {
        LOGE("Cannot change config while recording");
        return JNI_FALSE;
    }

    if (audioFilePath == nullptr) {
        LOGE("Audio file path is null");
        return JNI_FALSE;
    }

    g_recorder.input_preset = static_cast<aaudio_input_preset_t>(inputPreset);
    g_recorder.sample_rate = sampleRate;
    g_recorder.channel_count = channelCount;
    g_recorder.format = static_cast<aaudio_format_t>(format);
    g_recorder.performance_mode = static_cast<aaudio_performance_mode_t>(performanceMode);
    g_recorder.sharing_mode = static_cast<aaudio_sharing_mode_t>(sharingMode);

    const char *path_str = env->GetStringUTFChars(audioFilePath, nullptr);
    if (path_str != nullptr) {
        g_recorder.output_path = path_str;
        // Check if path ends with .wav - if not, it's a directory path and we need to generate filename
        g_recorder.auto_generate_filename =
                (g_recorder.output_path.length() < 4 ||
                 g_recorder.output_path.substr(g_recorder.output_path.length() - 4) != ".wav");
        env->ReleaseStringUTFChars(audioFilePath, path_str);
    } else {
        LOGE("Failed to get audio file path string");
        return JNI_FALSE;
    }

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_example_aaudiotester_recorder_AAudioRecorder_startNativeRecording(JNIEnv *env,
                                                                             jobject thiz) {
    if (g_recorder.is_recording.load(std::memory_order_acquire)) {
        LOGW("Already recording");
        return JNI_FALSE;
    }

    LOGI("startNativeRecording");

    stopWriteThread();  // 清理上次 error 残留、尚未 join 的写线程（先排空旧数据再重建资源）

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

    // 环形缓冲 + 写线程：回调只 memcpy 进缓冲，写线程负责落盘。
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
        g_recorder.wav_file->close();
        g_recorder.wav_file.reset();
        g_recorder.ring.reset();
        notifyRecordingError("[STREAM] Failed to start recording stream");
        return JNI_FALSE;
    }

    notifyRecordingStarted();

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

    stopWriteThread();  // join 写线程：排空缓冲中剩余数据落盘

    if (g_recorder.wav_file) {
        const bool finalized = g_recorder.wav_file->close();
        g_recorder.wav_file.reset();
        if (!finalized) {
            // 回填头部失败：录音数据未完整落盘，按错误上报而非伪装成正常结束
            notifyRecordingError("[FILE] Failed to finalize recording file (header write failed)");
        }
    }
    g_recorder.ring.reset();

    if (g_recorder.dropped_bytes.load(std::memory_order_relaxed) > 0) {
        LOGW("Recording dropped bytes: %zu", g_recorder.dropped_bytes.load(std::memory_order_relaxed));
    }

    // Only notify stopped if callback hasn't already notified Java (error or EOF)
    bool already_notified = g_recorder.callback_notified.exchange(false, std::memory_order_acq_rel);
    if (!already_notified) {
        notifyRecordingStopped();
    }

    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_example_aaudiotester_recorder_AAudioRecorder_releaseNative(JNIEnv *env,
                                                                      jobject thiz) {
    LOGI("releaseNative");

    // Clean up stream/WAV resources even if recording already ended (e.g. a stream error set
    // is_recording false but left the WAV header unfinalized). stopNativeRecording resets
    // stream/wav_file, so this condition also avoids re-running it after a normal stop.
    if (g_recorder.stream || g_recorder.wav_file) {
        Java_com_example_aaudiotester_recorder_AAudioRecorder_stopNativeRecording(env, thiz);
    }

    if (g_recorder.recorder_instance) {
        env->DeleteGlobalRef(g_recorder.recorder_instance);
        g_recorder.recorder_instance = nullptr;
    }

    g_recorder.on_recording_started_method = nullptr;
    g_recorder.on_recording_stopped_method = nullptr;
    g_recorder.on_recording_error_method = nullptr;

    LOGI("AAudioRecorder released");
}

}  // extern "C"
