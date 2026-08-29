#pragma once
//
// player/recorder 共享的基础设施与音频公共逻辑（单一事实源）。
// 全部为无状态 inline 函数/RAII，无 ODR 风险。
//

#include <jni.h>

#include <aaudio/AAudio.h>
#include <android/log.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>

#ifndef AAC_LOG_TAG
#define AAC_LOG_TAG "AAudio"
#endif

#define AAC_LOGW(...) __android_log_print(ANDROID_LOG_WARN, AAC_LOG_TAG, __VA_ARGS__)

namespace aaudio_common {

struct AAudioStreamDeleter {
    void operator()(AAudioStream* s) const {
        if (s)
            AAudioStream_close(s);
    }
};
using AAudioStreamPtr = std::unique_ptr<AAudioStream, AAudioStreamDeleter>;

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

// 守护全局 JNI 引用（player_instance/recorder_instance）的绑定与释放。initializeNative 在
// 主线程、releaseNative 在旧 executor 线程，二者交错会把全局引用二次释放；控制路径加锁零成本。
inline std::mutex& jniBindMutex() {
    static std::mutex m;
    return m;
}

// JNI 绑定样板：替换 instance 全局引用并返回其类（失败时清理引用并返回 nullptr）。
// player/recorder 的 initializeNative 共用此段（判空删旧引用 → NewGlobalRef → GetObjectClass）。
inline jclass bindJavaInstance(JNIEnv* env, jobject thiz, jobject& instance, const char* what) {
    std::lock_guard<std::mutex> lock(jniBindMutex());
    if (instance) {
        env->DeleteGlobalRef(instance);
        instance = nullptr;
    }
    instance = env->NewGlobalRef(thiz);
    if (!instance) {
        AAC_LOGW("Failed to create global reference for %s", what);
        return nullptr;
    }
    jclass clazz = env->GetObjectClass(thiz);
    if (!clazz) {
        AAC_LOGW("Failed to get object class for %s", what);
        env->DeleteGlobalRef(instance);
        instance = nullptr;
    }
    return clazz;
}

// 无参回调通知：JNI 引用判空 + attach + CallVoidMethod
inline void callVoidMethod(JavaVM* jvm, jobject instance, jmethodID method, const char* name) {
    if (!jvm || !instance || !method) {
        AAC_LOGW("Cannot notify %s: JNI references not set", name);
        return;
    }
    JniThreadAttachment attach(jvm);
    if (!attach.ok()) {
        AAC_LOGW("Failed to attach thread for JNI callback");
        return;
    }
    attach.env->CallVoidMethod(instance, method);
}

// 带字符串参数的错误回调通知
inline void notifyErrorToJava(JavaVM* jvm, jobject instance, jmethodID method, const std::string& error,
                              const char* name) {
    if (!jvm || !instance || !method) {
        AAC_LOGW("Cannot notify %s: JNI references not set", name);
        return;
    }
    JniThreadAttachment attach(jvm);
    if (!attach.ok()) {
        AAC_LOGW("Failed to attach thread for JNI callback");
        return;
    }
    jstring error_str = attach.env->NewStringUTF(error.c_str());
    attach.env->CallVoidMethod(instance, method, error_str);
    attach.env->DeleteLocalRef(error_str);
}

// format → 每样本字节数（未知格式按 16bit 处理）
inline int32_t bytesPerSample(aaudio_format_t format) {
    switch (format) {
        case AAUDIO_FORMAT_PCM_I24_PACKED:
            return 3;
        case AAUDIO_FORMAT_PCM_I32:
        case AAUDIO_FORMAT_PCM_FLOAT:
            return 4;
        case AAUDIO_FORMAT_PCM_I16:
        default:
            return 2;
    }
}

// 缓冲容量档位：低延迟 40ms，其余 100ms
inline int32_t bufferCapacityFrames(int32_t sample_rate, aaudio_performance_mode_t mode) {
    return (mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY) ? (sample_rate * 40) / 1000
                                                         : (sample_rate * 100) / 1000;
}

// 按 burst 优化缓冲大小：低延迟 2 burst，其余 4 burst（不超过容量）
inline void optimizeBufferSize(AAudioStream* stream, aaudio_performance_mode_t mode) {
    int32_t frames_per_burst = AAudioStream_getFramesPerBurst(stream);
    if (frames_per_burst > 0) {
        int32_t optimal_size = frames_per_burst * (mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? 2 : 4);
        optimal_size = std::min(optimal_size, AAudioStream_getBufferCapacityInFrames(stream));
        const int32_t result = AAudioStream_setBufferSizeInFrames(stream, optimal_size);
        if (result < 0) {
            // 失败会静默沿用默认容量，低延迟测试结果失真，必须可见
            AAC_LOGW("setBufferSizeInFrames(%d) failed: %s", optimal_size,
                     AAudio_convertResultToText(result));
        }
    }
}

// 停流并等待完成（100ms 超时）；失败仅告警，不阻断清理流程
inline void stopStreamAndWait(AAudioStream* stream) {
    aaudio_result_t result = AAudioStream_requestStop(stream);
    if (result != AAUDIO_OK) {
        AAC_LOGW("Failed to request stop: %s", AAudio_convertResultToText(result));
        return;
    }
    aaudio_stream_state_t state = AAUDIO_STREAM_STATE_STOPPING;
    result = AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_STOPPING, &state, 100000000);
    if (result != AAUDIO_OK) {
        AAC_LOGW("Failed to wait for stop: %s", AAudio_convertResultToText(result));
    }
}

}  // namespace aaudio_common
