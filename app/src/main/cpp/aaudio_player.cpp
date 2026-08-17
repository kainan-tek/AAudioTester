#include "aaudio_player.h"
#include "ring_buffer.h"
#include "wav_file.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <aaudio/AAudio.h>

namespace {

struct AAudioStreamDeleter {
    void operator()(AAudioStream* s) const {
        if (s)
            AAudioStream_close(s);
    }
};
using AAudioStreamPtr = std::unique_ptr<AAudioStream, AAudioStreamDeleter>;

}  // namespace

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
    AAudioStreamPtr stream;
    std::unique_ptr<WavFile> wav_file;
    std::atomic<bool> is_playing{false};
    std::atomic<bool> callback_notified{false};  // true if Java already notified by callback

    // 播放数据缓冲：读线程(生产) → 回调(消费)，把磁盘 I/O 移出实时回调。
    std::unique_ptr<SpScRingBuffer> ring;
    std::thread read_thread;
    std::atomic<bool> eof_reached{false};
    std::atomic<bool> stop_read_thread{false};
    std::atomic<size_t> underrun_count{0};

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
    std::atomic<int> write_counter{0};
    std::atomic<bool> gpio_state{false};
    std::atomic<bool> mute_audio{false};
    std::atomic<bool> latency_test_enabled{false};
    int gpio_fd = -1;
#endif
};

namespace {

AudioPlayerState g_player;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

bool validatePlayerState() {
    return !g_player.is_playing.load(std::memory_order_acquire);
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

static inline void toggleGpio() {
    bool current_state = g_player.gpio_state.load(std::memory_order_relaxed);
    bool new_state = !current_state;

    if (writeGpioValue(new_state ? 1 : 0)) {
        g_player.gpio_state.store(new_state, std::memory_order_relaxed);
    }
}
#endif

// RAII helper: attach current thread to JVM if needed, detach on destruction
struct JniThreadAttachment {
    JavaVM* jvm;
    JNIEnv* env = nullptr;
    bool attached = false;

    explicit JniThreadAttachment(JavaVM* vm) : jvm(vm) {
        if (jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
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
    if (!g_player.jvm || !g_player.player_instance || !method) {
        LOGW("Cannot notify %s: JNI references not set", name);
        return;
    }
    JniThreadAttachment attach(g_player.jvm);
    if (!attach.ok()) {
        LOGW("Failed to attach thread for JNI callback");
        return;
    }
    attach.env->CallVoidMethod(g_player.player_instance, method);
}

static void notifyPlaybackStarted() {
    notifyJavaCallback(g_player.on_playback_started_method, "playback started");
}

static void notifyPlaybackStopped() {
    notifyJavaCallback(g_player.on_playback_stopped_method, "playback stopped");
}

static void notifyPlaybackError(const std::string& error) {
    if (!g_player.jvm || !g_player.player_instance || !g_player.on_playback_error_method) {
        LOGW("Cannot notify playback error: JNI references not set");
        return;
    }
    JniThreadAttachment attach(g_player.jvm);
    if (!attach.ok()) {
        LOGW("Failed to attach thread for JNI callback");
        return;
    }
    jstring error_str = attach.env->NewStringUTF(error.c_str());
    attach.env->CallVoidMethod(g_player.player_instance, g_player.on_playback_error_method, error_str);
    attach.env->DeleteLocalRef(error_str);
}

// 停止读线程并 join（start 失败与正常 stop 共用）。
static void stopReadThread() {
    g_player.stop_read_thread.store(true, std::memory_order_release);
    if (g_player.read_thread.joinable()) {
        g_player.read_thread.join();
    }
}

// 播放读线程：持续把 WAV 数据读入环形缓冲，回调线程只 memcpy、永不碰磁盘。
static void playReadThread() {
    std::vector<char> buf(64 * 1024);
    while (!g_player.stop_read_thread.load(std::memory_order_acquire)) {
        const size_t n = g_player.wav_file->readAudioData(buf.data(), buf.size());
        if (n == 0) {  // 文件读尽
            g_player.eof_reached.store(true, std::memory_order_release);
            return;
        }
        size_t off = 0;
        while (off < n && !g_player.stop_read_thread.load(std::memory_order_acquire)) {
            off += g_player.ring->write(buf.data() + off, n - off);
            if (off < n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
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
        g_player.is_playing.store(false, std::memory_order_release);
        g_player.callback_notified.store(true, std::memory_order_release);
        notifyPlaybackError("[FILE] Audio file not opened");
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

    // Calculate bytes to read
    int32_t bytes_to_read = numFrames * channel_count * bytes_per_sample;

    // 先清零：underrun 或尾部不足时自动补静音，避免残留旧数据。
    memset(audioData, 0, static_cast<size_t>(bytes_to_read));

    const size_t bytes_read =
        g_player.ring->read(static_cast<char*>(audioData), static_cast<size_t>(bytes_to_read));

    if (bytes_read == 0) {
        // 缓冲空：数据已播尽(EOF) 或 读线程尚未填上(underrun，本次输出静音)。
        if (g_player.eof_reached.load(std::memory_order_acquire)) {
            g_player.is_playing.store(false, std::memory_order_release);
            g_player.callback_notified.store(true, std::memory_order_release);
            notifyPlaybackStopped();
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
        g_player.underrun_count.fetch_add(1, std::memory_order_relaxed);
    }

#if LATENCY_TEST_ENABLE
    if (g_player.latency_test_enabled.load(std::memory_order_relaxed)) {
        int current_count = g_player.write_counter.fetch_add(1);

        if (current_count % LATENCY_TEST_INTERVAL == 0) {
            toggleGpio();

            bool current_mute_state = g_player.mute_audio.load(std::memory_order_relaxed);
            g_player.mute_audio.store(!current_mute_state, std::memory_order_relaxed);

            if (current_count % (LATENCY_TEST_INTERVAL * 1000) == 0) {
                LOGD("Latency test: count=%d, gpio=%d, mute=%d", current_count,
                     g_player.gpio_state.load(std::memory_order_relaxed) ? 1 : 0, !current_mute_state ? 1 : 0);
            }
        }

        if (g_player.mute_audio.load(std::memory_order_relaxed)) {
            memset(audioData, 0, bytes_to_read);
        }
    }
#endif

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void errorCallback(AAudioStream* stream, void* userData, aaudio_result_t error) {
    LOGE("AAudio error: %s", AAudio_convertResultToText(error));
    g_player.is_playing.store(false, std::memory_order_release);
    g_player.callback_notified.store(true, std::memory_order_release);
    // Notify Java so it calls stopNativePlayback to clean up resources
    std::string error_msg = "[STREAM] Playback stream error: ";
    error_msg += AAudio_convertResultToText(error);
    notifyPlaybackError(error_msg);
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

    int32_t buffer_capacity = (g_player.performance_mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY)
                                  ? (sample_rate * 40) / 1000
                                  : (sample_rate * 100) / 1000;
    AAudioStreamBuilder_setBufferCapacityInFrames(builder, buffer_capacity);

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

    int32_t frames_per_burst = AAudioStream_getFramesPerBurst(g_player.stream.get());
    if (frames_per_burst > 0) {
        int32_t optimal_size =
            frames_per_burst * (g_player.performance_mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? 2 : 4);
        optimal_size = std::min(optimal_size, AAudioStream_getBufferCapacityInFrames(g_player.stream.get()));
        AAudioStream_setBufferSizeInFrames(g_player.stream.get(), optimal_size);
    }

    LOGI("Stream created: %dHz, %dch, format=%d, mode=%d", AAudioStream_getSampleRate(g_player.stream.get()),
         AAudioStream_getChannelCount(g_player.stream.get()), AAudioStream_getFormat(g_player.stream.get()),
         AAudioStream_getPerformanceMode(g_player.stream.get()));

    return true;
}

extern "C" {

JNIEXPORT jboolean JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_initializeNative(JNIEnv* env,
                                                                                              jobject thiz,
                                                                                              jstring filePath) {
    LOGI("initializeNative");

    if (!validatePlayerState()) {
        LOGE("Cannot initialize while playing");
        return JNI_FALSE;
    }

    if (g_player.player_instance) {
        env->DeleteGlobalRef(g_player.player_instance);
        g_player.player_instance = nullptr;
    }
    g_player.player_instance = env->NewGlobalRef(thiz);
    if (!g_player.player_instance) {
        LOGE("Failed to create global reference");
        return JNI_FALSE;
    }

    jclass clazz = env->GetObjectClass(thiz);
    if (!clazz) {
        LOGE("Failed to get object class");
        if (g_player.player_instance) {
            env->DeleteGlobalRef(g_player.player_instance);
            g_player.player_instance = nullptr;
        }
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

    if (filePath) {
        const char* path = env->GetStringUTFChars(filePath, nullptr);
        if (path) {
            g_player.audio_file_path = std::string(path);
            env->ReleaseStringUTFChars(filePath, path);
        } else {
            LOGE("Failed to get file path string");
            return JNI_FALSE;
        }
    }

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_setNativeConfig(
    JNIEnv* env, jobject thiz, jint usage, jint contentType, jint performanceMode, jint sharingMode, jstring filePath) {
    LOGI("setNativeConfig");

    if (!validatePlayerState()) {
        LOGE("Cannot change config while playing");
        return JNI_FALSE;
    }

    g_player.usage = static_cast<aaudio_usage_t>(usage);
    g_player.content_type = static_cast<aaudio_content_type_t>(contentType);
    g_player.performance_mode = static_cast<aaudio_performance_mode_t>(performanceMode);
    g_player.sharing_mode = static_cast<aaudio_sharing_mode_t>(sharingMode);

    if (filePath) {
        const char* path = env->GetStringUTFChars(filePath, nullptr);
        if (path) {
            g_player.audio_file_path = std::string(path);
            env->ReleaseStringUTFChars(filePath, path);
        } else {
            LOGE("Failed to get file path string");
            return JNI_FALSE;
        }
    }

    LOGI("Config updated: usage=%d, contentType=%d, performanceMode=%d, sharingMode=%d, file=%s", g_player.usage,
         g_player.content_type, g_player.performance_mode, g_player.sharing_mode, g_player.audio_file_path.c_str());

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_startNativePlayback(JNIEnv* env,
                                                                                                 jobject thiz) {
    LOGI("startNativePlayback");

    if (g_player.is_playing.load(std::memory_order_acquire)) {
        return JNI_FALSE;
    }

    stopReadThread();  // 清理上次 EOF/error 残留、尚未 join 的读线程，避免对 joinable 线程重新赋值触发 terminate

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

    // 环形缓冲 + 读线程：文件流式进入缓冲，回调只 memcpy（磁盘 I/O 移出实时线程）。
    g_player.ring = std::make_unique<SpScRingBuffer>(kDefaultRingCapacity);
    g_player.eof_reached.store(false, std::memory_order_release);
    g_player.stop_read_thread.store(false, std::memory_order_release);
    g_player.underrun_count.store(0, std::memory_order_relaxed);
    g_player.read_thread = std::thread(playReadThread);

#if LATENCY_TEST_ENABLE
    if (!initGpio()) {
        LOGE("Failed to initialize GPIO for latency test - latency test DISABLED");
        g_player.latency_test_enabled.store(false);
    } else {
        g_player.write_counter.store(0);
        g_player.gpio_state.store(false);
        g_player.mute_audio.store(false);
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

JNIEXPORT jboolean JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_stopNativePlayback(JNIEnv* env,
                                                                                                jobject thiz) {
    LOGI("stopNativePlayback");

    g_player.is_playing.store(false, std::memory_order_release);
    stopReadThread();

    if (g_player.stream) {
        aaudio_result_t result = AAudioStream_requestStop(g_player.stream.get());
        if (result != AAUDIO_OK) {
            LOGW("Failed to request stop: %s", AAudio_convertResultToText(result));
        } else {
            aaudio_stream_state_t state = AAUDIO_STREAM_STATE_STOPPING;
            result =
                AAudioStream_waitForStateChange(g_player.stream.get(), AAUDIO_STREAM_STATE_STOPPING, &state, 100000000);
            if (result != AAUDIO_OK) {
                LOGW("Failed to wait for stop: %s", AAudio_convertResultToText(result));
            }
        }
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

    // Only notify stopped if callback hasn't already notified Java (error or EOF)
    bool already_notified = g_player.callback_notified.exchange(false, std::memory_order_acq_rel);
    if (!already_notified) {
        notifyPlaybackStopped();
    }
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_releaseNative(JNIEnv* env, jobject thiz) {
    LOGI("Releasing AAudio player");

    // Clean up stream/WAV resources even if playback already ended (e.g. EOF set is_playing
    // false but left the stream open). stopNativePlayback resets stream/wav_file to null, so
    // this condition also avoids re-running it after a normal stop.
    if (g_player.stream || g_player.wav_file) {
        Java_com_example_aaudiotester_player_AAudioPlayer_stopNativePlayback(env, thiz);
    }

    if (g_player.player_instance) {
        env->DeleteGlobalRef(g_player.player_instance);
        g_player.player_instance = nullptr;
    }

    g_player.on_playback_started_method = nullptr;
    g_player.on_playback_stopped_method = nullptr;
    g_player.on_playback_error_method = nullptr;

    LOGI("AAudioPlayer released");
}

}  // extern "C"
