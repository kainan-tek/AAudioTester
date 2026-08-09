#include "aaudio_recorder.h"
#include "wav_file.h"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

#include <aaudio/AAudio.h>

namespace {

    struct AAudioStreamDeleter {
        void operator()(AAudioStream *s) const {
            if (s) AAudioStream_close(s);
        }
    };

    using AAudioStreamPtr = std::unique_ptr<AAudioStream, AAudioStreamDeleter>;

}  // namespace

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
    AAudioStreamPtr stream;
    std::unique_ptr<WavFile> wav_file;
    std::atomic<bool> is_recording{false};
    std::atomic<bool> callback_notified{false};  // true if Java already notified by callback

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

// RAII helper: attach current thread to JVM if needed, detach on destruction
struct JniThreadAttachment {
    JavaVM *jvm;
    JNIEnv *env = nullptr;
    bool attached = false;

    explicit JniThreadAttachment(JavaVM *vm) : jvm(vm) {
        if (jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
            if (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
            }
        }
    }

    ~JniThreadAttachment() {
        if (attached) {
            jvm->DetachCurrentThread();
        }
    }

    [[nodiscard]] bool ok() const { return env != nullptr; }
};

static void notifyJavaCallback(jmethodID method, const char* name) {
    if (!g_recorder.jvm || !g_recorder.recorder_instance || !method) {
        LOGW("Cannot notify %s: JNI references not set", name);
        return;
    }
    JniThreadAttachment attach(g_recorder.jvm);
    if (!attach.ok()) {
        LOGW("Failed to attach thread for JNI callback");
        return;
    }
    attach.env->CallVoidMethod(g_recorder.recorder_instance, method);
}

static void notifyRecordingStarted() {
    notifyJavaCallback(g_recorder.on_recording_started_method, "recording started");
}

static void notifyRecordingStopped() {
    notifyJavaCallback(g_recorder.on_recording_stopped_method, "recording stopped");
}

static void notifyRecordingError(const std::string &error) {
    if (!g_recorder.jvm || !g_recorder.recorder_instance || !g_recorder.on_recording_error_method) {
        LOGW("Cannot notify recording error: JNI references not set");
        return;
    }
    JniThreadAttachment attach(g_recorder.jvm);
    if (!attach.ok()) {
        LOGW("Failed to attach thread for JNI callback");
        return;
    }
    jstring error_str = attach.env->NewStringUTF(error.c_str());
    attach.env->CallVoidMethod(g_recorder.recorder_instance, g_recorder.on_recording_error_method,
                               error_str);
    attach.env->DeleteLocalRef(error_str);
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
        g_recorder.callback_notified.store(true, std::memory_order_release);
        notifyRecordingError("[FILE] WAV file not opened");
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    // Validate numFrames
    if (numFrames <= 0) {
        LOGE("Invalid numFrames: %d", numFrames);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    int32_t channel_count = AAudioStream_getChannelCount(stream);
    if (channel_count <= 0 || channel_count > 16) {
        LOGE("Invalid channel count: %d", channel_count);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    // Get bytes per sample based on format
    int32_t bytes_per_sample;
    switch (AAudioStream_getFormat(stream)) {
        case AAUDIO_FORMAT_PCM_I16:
            bytes_per_sample = 2;
            break;
        case AAUDIO_FORMAT_PCM_I24_PACKED:
            bytes_per_sample = 3;
            break;
        case AAUDIO_FORMAT_PCM_I32:
        case AAUDIO_FORMAT_PCM_FLOAT:
            bytes_per_sample = 4;
            break;
        default:
            bytes_per_sample = 2;
            break;
    }

    // Calculate bytes to write
    int32_t bytes_to_write = numFrames * channel_count * bytes_per_sample;

    if (!g_recorder.wav_file->writeData(audioData, static_cast<size_t>(bytes_to_write))) {
        LOGE("Failed to write audio data to WAV file");
        // Continue recording even if write fails (might be temporary permission issue)
    }

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void errorCallback(AAudioStream *stream, void *userData, aaudio_result_t error) {
    LOGE("AAudio error callback: %s", AAudio_convertResultToText(error));
    g_recorder.is_recording.store(false, std::memory_order_release);
    g_recorder.callback_notified.store(true, std::memory_order_release);
    // Notify Java so it calls stopNativeRecording to clean up resources
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
    int32_t buffer_capacity = (g_recorder.performance_mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY)
                              ? (g_recorder.sample_rate * 40) / 1000    // 40ms for low latency
                              : (g_recorder.sample_rate * 100) / 1000;  // 100ms for power saving
    AAudioStreamBuilder_setBufferCapacityInFrames(builder, buffer_capacity);

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

    int32_t frames_per_burst = AAudioStream_getFramesPerBurst(g_recorder.stream.get());
    if (frames_per_burst > 0) {
        int32_t optimal_size =
                frames_per_burst *
                (g_recorder.performance_mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? 2 : 4);
        optimal_size = std::min(optimal_size,
                                AAudioStream_getBufferCapacityInFrames(g_recorder.stream.get()));
        AAudioStream_setBufferSizeInFrames(g_recorder.stream.get(), optimal_size);
    }

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

    // Set recording flag before starting stream to avoid race condition with callback
    g_recorder.is_recording.store(true, std::memory_order_release);

    aaudio_result_t result = AAudioStream_requestStart(g_recorder.stream.get());
    if (result != AAUDIO_OK) {
        LOGE("Failed to start recording stream: %s", AAudio_convertResultToText(result));
        g_recorder.is_recording.store(false, std::memory_order_release);
        g_recorder.stream.reset();
        g_recorder.wav_file->close();
        g_recorder.wav_file.reset();
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
        aaudio_result_t result = AAudioStream_requestStop(g_recorder.stream.get());
        if (result != AAUDIO_OK) {
            LOGW("Failed to request stop: %s", AAudio_convertResultToText(result));
        } else {
            aaudio_stream_state_t state = AAUDIO_STREAM_STATE_STOPPING;
            result =
                    AAudioStream_waitForStateChange(g_recorder.stream.get(),
                                                    AAUDIO_STREAM_STATE_STOPPING, &state,
                                                    100000000);
            if (result != AAUDIO_OK) {
                LOGW("Failed to wait for stop: %s", AAudio_convertResultToText(result));
            }
        }
        g_recorder.stream.reset();
    }

    if (g_recorder.wav_file) {
        g_recorder.wav_file->close();
        g_recorder.wav_file.reset();
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

    if (g_recorder.is_recording.load(std::memory_order_acquire)) {
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
