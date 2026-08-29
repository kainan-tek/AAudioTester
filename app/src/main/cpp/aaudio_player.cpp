#include "aaudio_player.h"
#include "ring_buffer.h"
#include "wav_file.h"

#include <atomic>
#include <chrono>
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

    // 播放数据缓冲：读线程(生产) → 回调(消费)，把磁盘 I/O 移出实时回调。
    std::unique_ptr<SpScRingBuffer> ring;
    std::thread read_thread;
    std::atomic<bool> eof_reached{false};
    std::atomic<bool> stop_read_thread{false};
    std::atomic<size_t> underrun_count{0};
    // RT 回调置位的终止错误（静态串，无分配）：由读线程在非 RT 线程投递，回调线程不碰 JNI
    std::atomic<const char*> rt_error_msg{nullptr};

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
    std::atomic<int> write_counter{0};   // 唯一事实源：块号推导静音与 GPIO 电平
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

#endif

static void notifyPlaybackStarted() {
    aaudio_common::callVoidMethod(g_player.jvm, g_player.player_instance, g_player.on_playback_started_method,
                                  "playback started");
}

static void notifyPlaybackStopped() {
    aaudio_common::callVoidMethod(g_player.jvm, g_player.player_instance, g_player.on_playback_stopped_method,
                                  "playback stopped");
}

static void notifyPlaybackError(const std::string& error) {
    // 原子判重：rt_error 路径与 errorCallback 可能并发投递，只通知一次；
    // 已置位（stopped/error）则忽略，后续 stopNativePlayback 也不补发 onStopped 覆盖错误提示
    if (g_player.callback_notified.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    aaudio_common::notifyErrorToJava(g_player.jvm, g_player.player_instance, g_player.on_playback_error_method,
                                     error, "playback error");
}

// 停止读线程并 join（start 失败与正常 stop 共用）。
static void stopReadThread() {
    g_player.stop_read_thread.store(true, std::memory_order_release);
    if (g_player.read_thread.joinable()) {
        g_player.read_thread.join();
    }
}

// 播放读线程：持续把 WAV 数据读入环形缓冲，回调线程只 memcpy、永不碰磁盘。
// 兼作终止通知线程：EOF/RT 错误的 JNI 通知在此（非 RT 线程）投递，回调线程只置位。
static void playReadThread() {
    // 24-bit 帧大小(6/12...)不整除 64KB：缓冲取整到帧倍数，保证环形缓冲内容始终帧对齐
    const size_t frame_size =
        aaudio_common::bytesPerSample(g_player.wav_file->getAAudioFormat()) *
        static_cast<size_t>(g_player.wav_file->getChannelCount());
    const size_t buf_size = (64 * 1024) / frame_size * frame_size;
    std::vector<char> buf(buf_size);
    while (!g_player.stop_read_thread.load(std::memory_order_acquire)) {
        // RT 回调置的终止错误：在此投递，避免回调线程做 JNI
        if (const char* msg = g_player.rt_error_msg.load(std::memory_order_acquire)) {
            g_player.rt_error_msg.store(nullptr, std::memory_order_release);
            g_player.is_playing.store(false, std::memory_order_release);
            notifyPlaybackError(msg);
            return;
        }
        const size_t n = g_player.wav_file->readAudioData(buf.data(), buf.size());
        if (n == 0) {
            if (g_player.wav_file->hasReadError()) {
                // I/O 错误或文件截断：按错误上报，而非伪装成正常 EOF
                g_player.is_playing.store(false, std::memory_order_release);
                notifyPlaybackError("[FILE] Audio read failed or file truncated");
                return;
            }
            g_player.eof_reached.store(true, std::memory_order_release);  // 文件读尽
            // 等回调排空 ring 后再通知 stopped（RT 回调只 return STOP，不碰 JNI）
            while (!g_player.stop_read_thread.load(std::memory_order_acquire) &&
                   g_player.ring->readable() > 0 &&
                   g_player.rt_error_msg.load(std::memory_order_acquire) == nullptr) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            g_player.is_playing.store(false, std::memory_order_release);
            if (const char* msg = g_player.rt_error_msg.load(std::memory_order_acquire)) {
                g_player.rt_error_msg.store(nullptr, std::memory_order_release);
                notifyPlaybackError(msg);
            } else if (!g_player.callback_notified.exchange(true, std::memory_order_acq_rel)) {
                notifyPlaybackStopped();
            }
            // EOF 播放结束：主动关流释放硬件 session（start/release 的 reset 兜底幂等；
            // executor 侧对 stream 的操作均在 join 读线程之后，故无并发 reset）
            if (g_player.stream) g_player.stream.reset();
            return;
        }
        size_t off = 0;
        while (off < n && !g_player.stop_read_thread.load(std::memory_order_acquire)) {
            if (g_player.rt_error_msg.load(std::memory_order_acquire)) break;  // 提前让位错误投递
            off += g_player.ring->write(buf.data() + off, n - off);
            if (off < n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // 退出前投递 RT 回调在 stop 竞态窗口内可能已置的错误
    if (const char* msg = g_player.rt_error_msg.load(std::memory_order_acquire)) {
        g_player.rt_error_msg.store(nullptr, std::memory_order_release);
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
        // 只置位，JNI 通知由读线程在非 RT 线程投递（回调线程禁止 JNI）
        g_player.rt_error_msg.store("[FILE] Audio file not opened", std::memory_order_release);
        g_player.is_playing.store(false, std::memory_order_release);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    // 回调参数守卫：清状态并置位，与上方文件异常分支模式一致（原裸 return 会致 UI 卡“进行中”）
    int32_t channel_count = AAudioStream_getChannelCount(stream);
    if (numFrames <= 0 || channel_count <= 0 || channel_count > 16) {
        LOGE("Invalid callback args: numFrames=%d, channelCount=%d", numFrames, channel_count);
        g_player.rt_error_msg.store("[STREAM] Invalid callback args from stream", std::memory_order_release);
        g_player.is_playing.store(false, std::memory_order_release);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    // Get bytes per sample based on format
    const int32_t bytes_per_sample = aaudio_common::bytesPerSample(AAudioStream_getFormat(stream));

    // Calculate bytes to read
    int32_t bytes_to_read = numFrames * channel_count * bytes_per_sample;

    const size_t bytes_read =
        g_player.ring->read(static_cast<char*>(audioData), static_cast<size_t>(bytes_to_read));

    // 只补尾部静音：underrun 或尾部不足时补零避免残留旧数据（满数据时零开销）
    if (bytes_read < static_cast<size_t>(bytes_to_read)) {
        memset(static_cast<char*>(audioData) + bytes_read, 0, bytes_to_read - bytes_read);
    }

    if (bytes_read == 0) {
        // 缓冲空：数据已播尽(EOF) 或 读线程尚未填上(underrun，本次输出静音)。
        if (g_player.eof_reached.load(std::memory_order_acquire)) {
            // 终止通知（stopped/错误）由读线程在非 RT 线程投递，此处只 return STOP
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
        g_player.underrun_count.fetch_add(1, std::memory_order_relaxed);
    }

#if LATENCY_TEST_ENABLE
    if (g_player.latency_test_enabled.load(std::memory_order_relaxed)) {
        // 单一事实源：块号推导 GPIO 与静音，无独立状态可失步
        const int count = g_player.write_counter.fetch_add(1);   // 0-based
        const int block = count / LATENCY_TEST_INTERVAL;
        const bool muted = (block & 1) == 0;                     // 偶数块静音
        if (count % LATENCY_TEST_INTERVAL == 0) {                // 块起点（count=0 即首个）
            const int gpio = (block + 1) & 1;                    // +1 对齐 init 的低电平：block0 写 1 出首条边沿
            if (!writeGpioValue(gpio)) {
                // GPIO 失败：参考丢失，禁用测试，避免音频继续空切静音
                g_player.latency_test_enabled.store(false, std::memory_order_relaxed);
                closeGpio();
            } else if (block % 1000 == 0) {                      // 每 1000 块打一条日志
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
    // 通知 Java 后由 Fragment.onError 经 stop() 清理资源（关流、join 读线程）
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

    // AAudio 无 getMinBufferSize：minBurst=AAudioStream_getFramesPerBurst 为最小缓冲单位，
    // bufferSize=实际缓冲（optimizeBufferSize 已设 2×/4× burst），capacity=上限。
    int32_t frames_per_burst = AAudioStream_getFramesPerBurst(g_player.stream.get());
    int32_t buffer_size_frames = AAudioStream_getBufferSizeInFrames(g_player.stream.get());
    int32_t capacity_frames = AAudioStream_getBufferCapacityInFrames(g_player.stream.get());
    const int32_t frame_bytes = aaudio_common::bytesPerSample(actual_format) * actual_channels;
    LOGI("AAudio buffer info: minBurst=%d frames (%d B, %.1fms) | bufferSize=%d frames (%d B, %.1fms) | capacity=%d frames (%.1fms)",
         frames_per_burst, frames_per_burst * frame_bytes, frames_per_burst * 1000.0 / actual_rate,
         buffer_size_frames, buffer_size_frames * frame_bytes, buffer_size_frames * 1000.0 / actual_rate,
         capacity_frames, capacity_frames * 1000.0 / actual_rate);

    // 流实际声道/格式与 WAV 不一致时按错误处理：回调按流格式消费字节，而 ring 中是
    // 文件格式布局，不一致会导致变调/噪声且无提示（recorder 侧通过 actual 回写规避）。
    if (actual_channels != channel_count || actual_format != format) {
        LOGE("Stream params mismatch: requested %dch/fmt%d, actual %dch/fmt%d", channel_count,
             format, actual_channels, actual_format);
        g_player.stream.reset();
        return false;
    }
    // 采样率由框架自动重采样，仅告警（个别设备 openStream 返回重采样后的 rate）
    if (actual_rate != sample_rate) {
        LOGW("Sample rate adapted: %d requested, %d actual", sample_rate, actual_rate);
    }

    return true;
}

extern "C" {

JNIEXPORT jboolean JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_initializeNative(JNIEnv* env,
                                                                                              jobject thiz) {
    LOGI("initializeNative");

    if (!validatePlayerState()) {
        LOGE("Cannot initialize while playing");
        return JNI_FALSE;
    }

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

    // 一次性 latch：首个 claimer 才通知 stopped（error/EOF 已置位则跳过），避免双通知
    if (!g_player.callback_notified.exchange(true, std::memory_order_acq_rel)) {
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

    // 所有权守卫 + 互斥：仅删除当前仍指向本实例的全局引用——旋转/重建时旧实例延迟排队的
    // release（旧 executor 线程）与 initializeNative（主线程）经 jniBindMutex 互斥，杜绝二次释放。
    // method ID 只在真正释放本实例时清空：若引用已被新实例替换，method ID 归新实例所有。
    std::lock_guard<std::mutex> lock(aaudio_common::jniBindMutex());
    if (g_player.player_instance && env->IsSameObject(thiz, g_player.player_instance)) {
        env->DeleteGlobalRef(g_player.player_instance);
        g_player.player_instance = nullptr;
        g_player.on_playback_started_method = nullptr;
        g_player.on_playback_stopped_method = nullptr;
        g_player.on_playback_error_method = nullptr;
    }

    LOGI("AAudioPlayer released");
}

}  // extern "C"
