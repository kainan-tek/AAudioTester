#pragma once
//
// Shared infrastructure and common audio logic for player/recorder (single source of truth).
// All are stateless inline functions/RAII, so there is no ODR risk.
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

// Guards binding/releasing of the global JNI references (player_instance/recorder_instance).
// initializeNative runs on the main thread while releaseNative runs on the old executor thread;
// interleaving could double-release a global reference; locking the control path is near zero cost.
inline std::mutex& jniBindMutex() {
    static std::mutex m;
    return m;
}

// JNI binding boilerplate: replaces the instance global reference and returns its class
// (cleans up the reference and returns nullptr on failure).
// Shared by initializeNative of both player/recorder (delete old ref if non-null → NewGlobalRef → GetObjectClass).
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

// No-arg callback notification: null-check JNI refs + attach + CallVoidMethod
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

// Error callback notification with a string argument
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

// format → bytes per sample (unknown formats treated as 16-bit)
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

// Buffer capacity tiers: 40ms for low latency, 100ms otherwise
inline int32_t bufferCapacityFrames(int32_t sample_rate, aaudio_performance_mode_t mode) {
    return (mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY) ? (sample_rate * 40) / 1000
                                                         : (sample_rate * 100) / 1000;
}

// Optimize buffer size by burst: 2 bursts for low latency, 4 otherwise (capped at capacity)
inline void optimizeBufferSize(AAudioStream* stream, aaudio_performance_mode_t mode) {
    int32_t frames_per_burst = AAudioStream_getFramesPerBurst(stream);
    if (frames_per_burst > 0) {
        int32_t optimal_size = frames_per_burst * (mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? 2 : 4);
        optimal_size = std::min(optimal_size, AAudioStream_getBufferCapacityInFrames(stream));
        const int32_t result = AAudioStream_setBufferSizeInFrames(stream, optimal_size);
        if (result < 0) {
            // On failure the default capacity is silently kept, distorting low-latency test results — this must be visible
            AAC_LOGW("setBufferSizeInFrames(%d) failed: %s", optimal_size,
                     AAudio_convertResultToText(result));
        }
    }
}

// Stop the stream and wait for completion (100ms timeout); failure only logs a warning and does not block cleanup
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
