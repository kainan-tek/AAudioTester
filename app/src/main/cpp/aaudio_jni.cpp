#include <jni.h>

// 前置声明（不 include 两个 .h，避免 LOG_TAG 宏重定义）
void aaudio_player_set_jvm(JavaVM* vm);
void aaudio_recorder_set_jvm(JavaVM* vm);

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    aaudio_player_set_jvm(vm);
    aaudio_recorder_set_jvm(vm);
    return JNI_VERSION_1_6;
}
