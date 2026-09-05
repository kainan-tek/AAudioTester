#ifndef AAUDIOTESTER_CPP_AAUDIO_PLAYER_H_
#define AAUDIOTESTER_CPP_AAUDIO_PLAYER_H_

#include <android/log.h>
#include <jni.h>

// Log macros
#define LOG_TAG "AAudioPlayer"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * AAudio Player JNI Interface
 *
 * This header defines the JNI interface for the AAudio player functionality.
 * It provides functions to initialize, control, and manage audio playback
 * using Android's AAudio API with WAV file support.
 */

/**
 * Initialize the audio player
 * @param env JNI environment
 * @param thiz Java object instance
 * @return JNI_TRUE if initialization successful, JNI_FALSE otherwise
 */
JNIEXPORT jboolean JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_initializeNative(JNIEnv* env,
                                                                                              jobject thiz);

/**
 * Start audio playback
 * @param env JNI environment
 * @param thiz Java object instance
 * @return JNI_TRUE if playback started successfully, JNI_FALSE otherwise
 */
JNIEXPORT jboolean JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_startNativePlayback(JNIEnv* env,
                                                                                                 jobject thiz);

/**
 * Stop audio playback
 * @param env JNI environment
 * @param thiz Java object instance
 * @return JNI_TRUE if a stopped/error notice already reached Java (delivered here, or the latch is
 *         owned by the error/EOF path); JNI_FALSE if delivery failed — Java must run the onStopped
 *         completion itself
 */
JNIEXPORT jboolean JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_stopNativePlayback(JNIEnv* env,
                                                                                                jobject thiz);

/**
 * Release audio player resources
 * @param env JNI environment
 * @param thiz Java object instance
 */
JNIEXPORT void JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_releaseNative(JNIEnv* env, jobject thiz);

/**
 * Set native audio configuration
 * @param env JNI environment
 * @param thiz Java object instance
 * @param usage Audio usage integer value
 * @param contentType Audio content type integer value
 * @param performanceMode Performance mode integer value
 * @param sharingMode Sharing mode integer value
 * @param filePath Audio file path
 * @return JNI_TRUE if configuration set successfully, JNI_FALSE otherwise
 */
JNIEXPORT jboolean JNICALL Java_com_example_aaudiotester_player_AAudioPlayer_setNativeConfig(
    JNIEnv* env, jobject thiz, jint usage, jint contentType, jint performanceMode, jint sharingMode, jstring filePath);

#ifdef __cplusplus
}
void aaudio_player_set_jvm(JavaVM* vm);

#endif

#endif  // AAUDIOTESTER_CPP_AAUDIO_PLAYER_H_
