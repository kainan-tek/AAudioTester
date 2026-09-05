// AAudio recorder header file
#ifndef AAUDIOTESTER_CPP_AAUDIO_RECORDER_H_
#define AAUDIOTESTER_CPP_AAUDIO_RECORDER_H_

#include <android/log.h>
#include <jni.h>

// Log tags
#define LOG_TAG "AAudioRecorder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * AAudio Recorder JNI Interface
 *
 * This header defines the JNI interface for the AAudio recorder functionality.
 * It provides functions to initialize, control, and manage audio recording
 * using Android's AAudio API with WAV file support.
 */

/**
 * Initialize the audio recorder
 * @param env JNI environment
 * @param thiz Java object instance
 * @return JNI_TRUE if initialization successful, JNI_FALSE otherwise
 */
JNIEXPORT jboolean JNICALL
Java_com_example_aaudiotester_recorder_AAudioRecorder_initializeNative(JNIEnv *env,
                                                                       jobject thiz);

/**
 * Set native audio recording configuration
 * @param env JNI environment
 * @param thiz Java object instance
 * @param inputPreset Audio input preset
 * @param sampleRate Sample rate
 * @param channelCount Channel count
 * @param format Audio format
 * @param performanceMode Performance mode
 * @param sharingMode Sharing mode
 * @param audioFilePath Output file path
 * @return JNI_TRUE if configuration set successfully, JNI_FALSE otherwise
 */
JNIEXPORT jboolean JNICALL
Java_com_example_aaudiotester_recorder_AAudioRecorder_setNativeConfig(JNIEnv *env,
                                                                      jobject thiz,
                                                                      jint inputPreset,
                                                                      jint sampleRate,
                                                                      jint channelCount,
                                                                      jint format,
                                                                      jint performanceMode,
                                                                      jint sharingMode,
                                                                      jstring audioFilePath);

/**
 * Start audio recording
 * @param env JNI environment
 * @param thiz Java object instance
 * @return JNI_TRUE if recording started successfully, JNI_FALSE otherwise
 */
JNIEXPORT jboolean JNICALL
Java_com_example_aaudiotester_recorder_AAudioRecorder_startNativeRecording(JNIEnv *env,
                                                                           jobject thiz);

/**
 * Stop audio recording
 * @param env JNI environment
 * @param thiz Java object instance
 * @return JNI_TRUE if a stopped/error notice already reached Java (delivered here, or the latch is
 *         owned by the error path); JNI_FALSE if delivery failed — Java must run the onStopped
 *         completion itself
 */
JNIEXPORT jboolean JNICALL
Java_com_example_aaudiotester_recorder_AAudioRecorder_stopNativeRecording(JNIEnv *env,
                                                                          jobject thiz);

/**
 * Release audio recorder resources
 * @param env JNI environment
 * @param thiz Java object instance
 */
JNIEXPORT void JNICALL
Java_com_example_aaudiotester_recorder_AAudioRecorder_releaseNative(JNIEnv *env, jobject thiz);

#ifdef __cplusplus
}
void aaudio_recorder_set_jvm(JavaVM* vm);

#endif

#endif  // AAUDIOTESTER_CPP_AAUDIO_RECORDER_H_
