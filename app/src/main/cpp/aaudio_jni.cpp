#include <jni.h>

// Forward declarations (do not include the two .h files to avoid LOG_TAG macro redefinition)
void aaudio_player_set_jvm(JavaVM* vm);
void aaudio_recorder_set_jvm(JavaVM* vm);

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    aaudio_player_set_jvm(vm);
    aaudio_recorder_set_jvm(vm);
    return JNI_VERSION_1_6;
}
