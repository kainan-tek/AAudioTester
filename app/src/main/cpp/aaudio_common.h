#pragma once
//
// Shared infrastructure and common audio logic for player/recorder (single source of truth).
// All are inline definitions (the only stateful one, RtErrorSlot, is instantiated inside the
// engines' file-static state structs), so there is no ODR risk.
//

#include <jni.h>

#include <aaudio/AAudio.h>
#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <memory>
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

// Terminal-error slot for RT→worker delivery (one per engine): the RT callback publishes a static
// literal, or a message composed into buf (no heap on the RT thread); the worker thread takes it
// on a non-RT thread and delivers it to Java. errorCallback fires at most once per stream, so buf
// has a single writer (data-callback branches publish literals only, never touching buf);
// publish()'s release-store happens-after the buffer write, and take()'s acquire-load makes the
// buffer contents visible before the message is read.
struct RtErrorSlot {
    std::atomic<const char*> msg{nullptr};
    char buf[128] = {};

    // RT side: publish a static literal or a message written into buf
    void publish(const char* text) { msg.store(text, std::memory_order_release); }
    // Worker side: non-consuming check (loop conditions)
    bool pending() const { return msg.load(std::memory_order_acquire) != nullptr; }
    // Worker side: take the pending message (nullptr if none), leaving the slot empty
    const char* take() {
        const char* m = msg.load(std::memory_order_acquire);
        if (m) {
            msg.store(nullptr, std::memory_order_release);
        }
        return m;
    }
    // Start path: drop a leftover message from the previous session's final in-flight callback
    void clear() { msg.store(nullptr, std::memory_order_release); }
};

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

// JNI binding boilerplate: replaces the instance global reference and returns its class
// (cleans up the reference and returns nullptr on failure).
// Shared by initializeNative of both player/recorder (delete old ref if non-null → NewGlobalRef → GetObjectClass).
// No lock: every bind/release caller runs on the shared Kotlin executor thread (see callVoidMethod
// below for why the instance reads from other threads are still safe).
inline jclass bindJavaInstance(JNIEnv* env, jobject thiz, jobject& instance, const char* what) {
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

// No-arg callback notification: null-check JNI refs + attach + CallVoidMethod.
// Returns false when the notification was NOT delivered (references unset, thread attach failed)
// so latch-consuming callers can release the latch — symmetric with notifyErrorToJava below.
// Lock-free: all bind/release runs on the shared Kotlin executor thread, and the instance read
// below cannot race it: initializeNative joins any leftover reader/writer worker BEFORE
// rebinding (hard join-before-rebind invariant — a worker's late notifications always run
// against the binding they were created under), AAudioStream_close joins in-flight callbacks,
// and a released engine's late stop() is dropped by the IDLE gate set at the end of release().
// A listener exception after CallVoidMethod still counts as delivered (returns true).
inline bool callVoidMethod(JavaVM* jvm, jobject instance, jmethodID method, const char* name) {
    if (!jvm || !instance || !method) {
        AAC_LOGW("Cannot notify %s: JNI references not set", name);
        return false;
    }
    JniThreadAttachment attach(jvm);
    if (!attach.ok()) {
        AAC_LOGW("Failed to attach thread for JNI callback");
        return false;
    }
    attach.env->CallVoidMethod(instance, method);
    if (attach.env->ExceptionCheck()) {
        // A throwing listener must not leave a pending exception across DetachCurrentThread
        AAC_LOGW("Java listener threw in %s notification (exception cleared)", name);
        attach.env->ExceptionClear();
    }
    return true;
}

// Error callback notification with a string argument.
// Returns false when the notification was NOT delivered (references unset, thread attach failed, or string
// creation failed) so the caller can release its dedup latch — a lost notification must not also consume the
// right to notify. A listener exception after CallVoidMethod still counts as delivered (returns true).
inline bool notifyErrorToJava(JavaVM* jvm, jobject instance, jmethodID method, const std::string& error,
                              const char* name) {
    if (!jvm || !instance || !method) {
        AAC_LOGW("Cannot notify %s: JNI references not set", name);
        return false;
    }
    JniThreadAttachment attach(jvm);
    if (!attach.ok()) {
        AAC_LOGW("Failed to attach thread for JNI callback");
        return false;
    }
    jstring error_str = attach.env->NewStringUTF(error.c_str());
    if (!error_str) {
        // Delivering null would NPE the Kotlin listener; abandon this notification instead
        AAC_LOGW("Failed to create error string for %s notification", name);
        attach.env->ExceptionClear();  // NewStringUTF leaves an OutOfMemoryError pending
        return false;
    }
    attach.env->CallVoidMethod(instance, method, error_str);
    attach.env->DeleteLocalRef(error_str);
    if (attach.env->ExceptionCheck()) {
        // Same as callVoidMethod: never detach with a pending exception
        AAC_LOGW("Java listener threw in %s notification (exception cleared)", name);
        attach.env->ExceptionClear();
    }
    return true;
}

// Java notification endpoint for one engine: JVM + listener binding + the one-shot "already
// notified" latch. One per engine, instantiated inside the engines' file-static state structs
// (header-only, so no ODR risk — same rationale as RtErrorSlot).
//
// Thread discipline (invariants previously restated at each use, now stated once):
//  - jvm is set once from JNI_OnLoad before any thread can use it.
//  - instance/methods are written only on the engine's executor under the join-before-rebind
//    invariant (initializeNative joins any leftover worker BEFORE rebinding; AAudioStream close
//    joins in-flight callbacks), so a worker's late notifications always run against the binding
//    they were created under.
//  - The latch is atomic: claim() is an acq_rel exchange, release() a release-store.
struct JavaNotifier {
    JavaVM* jvm = nullptr;
    jobject instance = nullptr;
    jmethodID stopped_method = nullptr;
    jmethodID error_method = nullptr;
    std::atomic<bool> notified{false};  // claimed ⇔ a notice was delivered, or a stop-path delivery failed and left it claimed (both reset by the next start)

    // initializeNative: rebind to a fresh fragment instance (delete old global ref → new ref →
    // method IDs). quiesce joins any leftover worker BEFORE the rebinding — the join-before-rebind
    // invariant made structural: a worker's late notifications must run against the OLD binding,
    // never be redirected onto the new instance, and the compiler now forces every caller to do it.
    // Returns false on failure with the binding fully cleared.
    bool bind(JNIEnv* env, jobject thiz, const char* what,
              const char* stopped_name, const char* error_name,
              void (*quiesce)()) {
        quiesce();
        jclass clazz = bindJavaInstance(env, thiz, instance, what);
        if (!clazz) {
            return false;
        }
        stopped_method = env->GetMethodID(clazz, stopped_name, "()V");
        error_method = env->GetMethodID(clazz, error_name, "(Ljava/lang/String;)V");
        env->DeleteLocalRef(clazz);  // single deletion point: the local ref exists only in this scope
        if (!stopped_method || !error_method) {
            AAC_LOGW("Failed to get %s callback method IDs", what);
            if (instance) {
                env->DeleteGlobalRef(instance);
                instance = nullptr;
            }
            return false;
        }
        return true;
    }

    // releaseNative: drop the binding (global ref + method IDs). Executor-side only.
    void clear(JNIEnv* env) {
        if (instance) {
            env->DeleteGlobalRef(instance);
            instance = nullptr;
            stopped_method = nullptr;
            error_method = nullptr;
        }
    }

    // Latch primitives. claim() takes the one-shot notification right (false = a notice was
    // already delivered); release() gives it back — on delivery failure (so a later path can
    // retry) and at start (dropping the previous session's final in-flight claim).
    bool claim() { return !notified.exchange(true, std::memory_order_acq_rel); }
    void release() { notified.store(false, std::memory_order_release); }

    // One-shot stopped notification: claim → deliver → release on failure (a later path can retry).
    // True iff a notice reached Kotlin (delivered here, or the latch is owned by the error/EOF
    // path); false = delivery failed — Kotlin self-runs the onStopped completion to recover the UI
    // (the cross-language return-value contract of stopNativeXxx: JNI_TRUE ⇔ this true).
    // Releasing on failure is uniform for all callers: the session is over either way (stop joins
    // the workers before claiming; the EOF path is its worker's last act), and the next start's
    // release() drops any leftover claim regardless.
    bool deliverStoppedOnce(const char* name) {
        if (!claim()) {
            return true;  // error/EOF path owns the latch: the UI was already updated by its notice
        }
        if (callVoidMethod(jvm, instance, stopped_method, name)) {
            return true;
        }
        // callVoidMethod already logged the reason; release so a later path can still notify
        release();
        return false;
    }

    // releaseNative: true when this call is STALE — a newer instance has taken over the binding
    // (rotation rebuild), so the stream/wav state and the reference below belong to it, not to
    // this dead instance; the caller must skip all cleanup. False = proceed with cleanup and call
    // clear(env) afterwards. (A null binding cannot coexist with live state: release clears both,
    // and initialize's failure path clears the binding only while no state exists.)
    bool isStaleRelease(JNIEnv* env, jobject thiz) {
        if (instance && !env->IsSameObject(thiz, instance)) {
            AAC_LOGW("Stale release ignored: binding owned by a newer instance");
            return true;
        }
        return false;
    }

    // The error-delivery policy shared by both engines: claim → deliver → release on failure so a
    // later path can retry. True iff delivered (false = swallowed by an earlier notice, or
    // delivery failed — callers with a genuine loss to report keep it attributable in logcat).
    bool deliverErrorOnce(const std::string& error, const char* name) {
        if (!claim()) {
            return false;
        }
        if (notifyErrorToJava(jvm, instance, error_method, error, name)) {
            return true;
        }
        release();
        return false;
    }
};

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
