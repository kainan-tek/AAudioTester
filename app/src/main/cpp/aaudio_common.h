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

#define AAC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, AAC_LOG_TAG, __VA_ARGS__)
#define AAC_LOGW(...) __android_log_print(ANDROID_LOG_WARN, AAC_LOG_TAG, __VA_ARGS__)

namespace aaudio_common {

struct AAudioStreamDeleter {
    void operator()(AAudioStream* s) const {
        if (s)
            AAudioStream_close(s);
    }
};
using AAudioStreamPtr = std::unique_ptr<AAudioStream, AAudioStreamDeleter>;

// Terminal-error slot for RT→worker delivery (one per engine): the RT callback publishes a
// pointer to STATIC storage; the worker thread takes it on a non-RT thread and delivers it to
// Java. No buffer, no composition on the RT thread — publish/take is a plain atomic pointer
// hand-off, so there is no lifetime or single-writer invariant to maintain.
struct RtErrorSlot {
    std::atomic<const char*> msg{nullptr};

    // RT side: publish a static literal (text must point to static storage — the worker may
    // read it long after the callback returned)
    void publish(const char* text) { msg.store(text, std::memory_order_release); }

    // Worker side: non-consuming check (loop conditions)
    bool pending() const { return msg.load(std::memory_order_acquire) != nullptr; }
    // Worker side: take the pending message (nullptr if none), leaving the slot empty. One atomic
    // exchange: with a load-then-clear pair, a publish landing between the two steps would have
    // its newer message silently erased by the clear.
    const char* take() { return msg.exchange(nullptr, std::memory_order_acq_rel); }
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
        // NewGlobalRef leaves OutOfMemoryError pending: clear it so the false return (not the
        // exception) is the failure signal at the JNI boundary, and no further JNI call in this
        // path runs with a pending exception
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
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

// Shared delivery skeleton: null-check JNI refs + attach + invoke + exception cleanup.
// Lock-free: all bind/release runs on the shared Kotlin executor thread, and the instance read
// below cannot race it: initializeNative joins any leftover reader/writer worker BEFORE
// rebinding (hard join-before-rebind invariant — a worker's late notifications always run
// against the binding they were created under), AAudioStream_close joins in-flight callbacks,
// and a released engine's late stop() is dropped by the IDLE gate set at the end of release().
// Returns false when the notification was NOT delivered (references unset, thread attach failed,
// invoke failed, or the listener threw) so latch-consuming callers can release the latch — a
// lost notification must not also consume the right to notify. A listener exception after the
// call counts as NOT delivered (returns false): the notice's effect (the listener's UI update)
// did not complete, and a claimed latch would leave no recovery path. Returning false hands
// recovery to the established mechanisms (latch release → a later stopped delivery, or Kotlin
// stop()'s self-run). `invoke` performs the CallVoidMethod (with any arguments) and returns
// false if it failed BEFORE invoking (reason already logged, pending exception already cleared —
// the skeleton must not misreport that as a listener throw).
template <typename Invoke>
inline bool deliverVoidNotification(JavaVM* jvm, jobject instance, jmethodID method,
                                    const char* name, Invoke&& invoke) {
    if (!jvm || !instance || !method) {
        AAC_LOGW("Cannot notify %s: JNI references not set", name);
        return false;
    }
    JniThreadAttachment attach(jvm);
    if (!attach.ok()) {
        AAC_LOGW("Failed to attach thread for JNI callback");
        return false;
    }
    if (!invoke(attach.env, instance, method)) {
        return false;  // pre-invoke failure: already logged, exception already cleared
    }
    if (attach.env->ExceptionCheck()) {
        // A throwing listener must not leave a pending exception across DetachCurrentThread
        AAC_LOGW("Java listener threw in %s notification (exception cleared)", name);
        attach.env->ExceptionClear();
        return false;  // not delivered: the listener's effect did not complete
    }
    return true;
}

// No-arg callback notification.
inline bool callVoidMethod(JavaVM* jvm, jobject instance, jmethodID method, const char* name) {
    return deliverVoidNotification(jvm, instance, method, name,
                                   [](JNIEnv* env, jobject obj, jmethodID m) {
                                       env->CallVoidMethod(obj, m);
                                       return true;
                                   });
}

// Error callback notification with a string argument.
inline bool notifyErrorToJava(JavaVM* jvm, jobject instance, jmethodID method, const std::string& error,
                              const char* name) {
    return deliverVoidNotification(jvm, instance, method, name,
                                   [&](JNIEnv* env, jobject obj, jmethodID m) {
                                       jstring error_str = env->NewStringUTF(error.c_str());
                                       if (!error_str) {
                                           // Delivering null would NPE the Kotlin listener; abandon this notification instead
                                           AAC_LOGW("Failed to create error string for %s notification", name);
                                           env->ExceptionClear();  // NewStringUTF leaves an OutOfMemoryError pending
                                           return false;
                                       }
                                       env->CallVoidMethod(obj, m, error_str);
                                       env->DeleteLocalRef(error_str);
                                       return true;
                                   });
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
    std::atomic<bool> notified{false};  // claimed ⇔ a notice was delivered (delivery failures release the claim, so a later path can retry; the next start resets it)

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
            // GetMethodID leaves NoSuchMethodError pending: same contract — the false return is
            // the failure signal; the reason is already in logcat
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
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
    // retry) and at start (dropping the previous session's final in-flight claim). A released
    // claim means the same listener method may be invoked again later; accepted, because every
    // listener effect is an idempotent UI-state write.
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
    bool isStaleRelease(JNIEnv* env, jobject thiz) const {
        if (instance && !env->IsSameObject(thiz, instance)) {
            AAC_LOGW("Stale release ignored: binding owned by a newer instance");
            return true;
        }
        return false;
    }

    // releaseNative's three-step orchestration, owned once: the stale-release guard decides
    // whether this instance still owns the state; the stop reclaims leftover session state (an
    // error path can leave stream/wav allocated with no queued cleanup stop); clear drops the
    // binding. Returns false when the release is stale — the caller must skip everything,
    // including the stop.
    template <typename Stop>
    bool releaseSession(JNIEnv* env, jobject thiz, bool hasLiveState, Stop&& stop) {
        if (isStaleRelease(env, thiz)) {
            return false;
        }
        // stopNative nulls stream/wav_file itself, so hasLiveState also avoids re-running a
        // stop that already completed
        if (hasLiveState) {
            stop();
        }
        // The guard returns on any takeover, and initialize cannot interleave (all native entry
        // is FIFO on the shared executor): the binding is ours — safe to clear
        clear(env);
        return true;
    }

    // The error-delivery policy shared by both engines: claim → deliver → release on failure so a
    // later path can retry. True iff delivered. On false the loss is attributed in logcat HERE
    // (swallowed by an earlier notice, or delivery failed) — callers need no wrapper logging
    // around the return value.
    bool deliverErrorOnce(const std::string& error, const char* name) {
        if (!claim()) {
            // The message is genuinely lost (an earlier notice owns the latch); keep the loss
            // attributable — the delivery-failure path below logs the same way
            AAC_LOGW("%s not delivered (latch owned by an earlier notice): %s", name, error.c_str());
            return false;
        }
        if (notifyErrorToJava(jvm, instance, error_method, error, name)) {
            return true;
        }
        // The take()-style callers consume the message before delivering: without this log the
        // text would be gone on a delivery failure (there is nothing left to retry from).
        AAC_LOGW("%s not delivered: %s", name, error.c_str());
        release();
        return false;
    }
};

// The worker threads' standard RT-error teardown: consume the terminal message, clear the
// session flag (the RT callback then returns STOP and a fresh start is gated), deliver the
// error through the one-shot latch. Returns true when a message was consumed.
inline bool consumeRtError(RtErrorSlot& slot, std::atomic<bool>& session_flag,
                           JavaNotifier& notifier, const char* name) {
    const char* msg = slot.take();
    if (!msg) return false;
    session_flag.store(false, std::memory_order_release);
    notifier.deliverErrorOnce(msg, name);
    return true;
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

// Default burst tiers for the stream buffer: 2 bursts for low latency, 4 otherwise
inline int32_t defaultBurstCount(aaudio_performance_mode_t mode) {
    return (mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY) ? 2 : 4;
}

// Builder-stage capacity estimate: the mode's time tier, widened when an explicit burst count
// needs more room. frames_per_burst is unknown before open, so sample_rate/100 assumes a
// ≤10ms burst (the common upper bound); a real burst beyond that is caught after open by
// optimizeBufferSize's clamp warning — capacity and buffer size can never silently diverge.
inline int32_t bufferCapacityFrames(int32_t sample_rate, aaudio_performance_mode_t mode,
                                    int32_t buffer_bursts) {
    const int32_t by_time = (mode == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY)
                                ? (sample_rate * 40) / 1000
                                : (sample_rate * 100) / 1000;
    const int32_t by_bursts = buffer_bursts > 0 ? buffer_bursts * (sample_rate / 100) : 0;
    return std::max(by_time, by_bursts);
}

// Size the stream buffer by burst: explicit N bursts when buffer_bursts > 0, otherwise the
// mode tier (defaultBurstCount). The request is clamped to capacity (AAudio clamps
// setBufferSizeInFrames silently, so an under-sized capacity must be made visible here).
// Logs requested vs applied on every session start.
inline void optimizeBufferSize(AAudioStream* stream, aaudio_performance_mode_t mode,
                               int32_t buffer_bursts) {
    const int32_t frames_per_burst = AAudioStream_getFramesPerBurst(stream);
    if (frames_per_burst <= 0) {
        return;
    }
    const int32_t bursts = buffer_bursts > 0 ? buffer_bursts : defaultBurstCount(mode);
    const int32_t requested = frames_per_burst * bursts;
    int32_t target = requested;
    const int32_t capacity = AAudioStream_getBufferCapacityInFrames(stream);
    if (capacity > 0 && target > capacity) {
        AAC_LOGW("Requested buffer %d frames (%d bursts x %d) exceeds capacity %d - clamped",
                 requested, bursts, frames_per_burst, capacity);
        target = capacity;
    }
    const int32_t result = AAudioStream_setBufferSizeInFrames(stream, target);
    if (result < 0) {
        // On failure the default capacity is silently kept, distorting low-latency test results — this must be visible
        AAC_LOGW("setBufferSizeInFrames(%d) failed: %s", target,
                 AAudio_convertResultToText(result));
        return;
    }
    // result is the size actually applied (the HAL may adjust the request downward)
    AAC_LOGI("Buffer size: requested %d frames (%d bursts x %d), applied %d, capacity %d",
             requested, bursts, frames_per_burst, result, capacity);
}

// Swap in the newly opened stream and clear the previous session's leftover terminal error. The
// ordering IS the invariant: reset() closes the old stream and joins its in-flight callbacks —
// the earliest point where no stale rt_error publisher remains — so the clear cannot be hoisted
// above it (the new reader/writer would consume the stale error and kill the fresh session).
inline void adoptStream(AAudioStreamPtr& stream, AAudioStream* raw, RtErrorSlot& rt_error) {
    stream.reset(raw);
    rt_error.clear();
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
