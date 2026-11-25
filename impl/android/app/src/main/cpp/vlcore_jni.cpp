// JNI 胶水：Kotlin 只看到「路径进、JSON 出」，所有 VL 线逻辑在 impl/shard。
//
// 线程约定：
//   · nativeRunJSON 是阻塞调用，Kotlin 侧在 Dispatchers.Default 上调用；
//   · 进度回调与远程识别回调都发生在同一次 nativeRunJSON 调用内（同一线程），
//     因此把 JNIEnv* 暂存在句柄里是安全的，不需要 AttachCurrentThread。
#include <jni.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "vlcore/vlcore_c.h"

namespace {

struct EngineHandle {
    vlcore_engine_t* engine{nullptr};
    JNIEnv* env{nullptr};          // 仅在一次 nativeRunJSON 期间有效
    jobject remote{nullptr};       // RemoteRecognizer 的全局引用
    jmethodID remoteMethod{nullptr};
};

std::string ToString(JNIEnv* env, jstring value) {
    if (value == nullptr) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result(chars != nullptr ? chars : "");
    if (chars != nullptr) env->ReleaseStringUTFChars(value, chars);
    return result;
}

void ThrowIllegalState(JNIEnv* env, const std::string& message) {
    jclass cls = env->FindClass("java/lang/IllegalStateException");
    if (cls != nullptr) {
        env->ThrowNew(cls, message.c_str());
        env->DeleteLocalRef(cls);
    }
}

EngineHandle* Handle(jlong pointer) {
    return reinterpret_cast<EngineHandle*>(pointer);
}

void ProgressTrampoline(int done, int total, void* user) {
    auto* ctx = static_cast<std::pair<JNIEnv*, jobject>*>(user);
    if (ctx == nullptr || ctx->second == nullptr) return;
    JNIEnv* env = ctx->first;
    jclass cls = env->GetObjectClass(ctx->second);
    jmethodID method = env->GetMethodID(cls, "onProgress", "(II)V");
    env->DeleteLocalRef(cls);
    if (method == nullptr) return;
    env->CallVoidMethod(ctx->second, method, done, total);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

char* RemoteTrampoline(const uint8_t* rgb, int width, int height,
                       const char* prompt, void* user, char** error) {
    auto* handle = static_cast<EngineHandle*>(user);
    JNIEnv* env = handle != nullptr ? handle->env : nullptr;
    if (handle == nullptr || env == nullptr || handle->remote == nullptr) {
        if (error != nullptr) *error = strdup("远程识别服务未配置。");
        return nullptr;
    }

    const jsize size = width * height * 3;
    jbyteArray pixels = env->NewByteArray(size);
    if (pixels == nullptr) {
        if (error != nullptr) *error = strdup("图像缓冲分配失败。");
        return nullptr;
    }
    env->SetByteArrayRegion(pixels, 0, size, reinterpret_cast<const jbyte*>(rgb));

    jstring promptText = env->NewStringUTF(prompt != nullptr ? prompt : "OCR:");
    auto result = static_cast<jstring>(
        env->CallObjectMethod(handle->remote, handle->remoteMethod, pixels, width, height, promptText));

    env->DeleteLocalRef(promptText);
    env->DeleteLocalRef(pixels);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        if (error != nullptr) *error = strdup("远程识别调用失败。");
        return nullptr;
    }
    if (result == nullptr) {
        if (error != nullptr) *error = strdup("远程识别返回空结果。");
        return nullptr;
    }
    const std::string text = ToString(env, result);
    env->DeleteLocalRef(result);
    return strdup(text.c_str());
}

}  // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_ai_ocrframe_vlocr_VlEngine_nativeLayoutEngineBuilt(JNIEnv*, jclass) {
    return vlcore_layout_engine_built() != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_ai_ocrframe_vlocr_VlEngine_nativeVlmEngineBuilt(JNIEnv*, jclass) {
    return vlcore_vlm_engine_built() != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_ai_ocrframe_vlocr_VlEngine_nativePresetName(JNIEnv* env, jclass) {
    return env->NewStringUTF(vlcore_preset_name());
}

JNIEXPORT jstring JNICALL
Java_ai_ocrframe_vlocr_VlEngine_nativePresetVersion(JNIEnv* env, jclass) {
    return env->NewStringUTF(vlcore_preset_version());
}

JNIEXPORT jstring JNICALL
Java_ai_ocrframe_vlocr_VlEngine_nativePipelineVersion(JNIEnv* env, jclass) {
    return env->NewStringUTF(vlcore_pipeline_version());
}

JNIEXPORT jint JNICALL
Java_ai_ocrframe_vlocr_VlEngine_nativeMaxNewTokens(JNIEnv*, jclass) {
    return vlcore_max_new_tokens();
}

JNIEXPORT jlong JNICALL
Java_ai_ocrframe_vlocr_VlEngine_nativeCreate(JNIEnv* env, jclass, jstring layoutPath,
                                             jstring vlmPath, jstring mmprojPath,
                                             jint contextSize, jint threads) {
    const std::string layout = ToString(env, layoutPath);
    const std::string vlm = ToString(env, vlmPath);
    const std::string mmproj = ToString(env, mmprojPath);

    char* message = nullptr;
    vlcore_engine_t* engine = vlcore_create(layout.empty() ? nullptr : layout.c_str(),
                                            vlm.empty() ? nullptr : vlm.c_str(),
                                            mmproj.empty() ? nullptr : mmproj.c_str(),
                                            contextSize, threads, &message);
    if (engine == nullptr) {
        ThrowIllegalState(env, message != nullptr ? std::string(message) : std::string("识别引擎初始化失败。"));
        if (message != nullptr) vlcore_free_string(message);
        return 0;
    }
    auto* handle = new EngineHandle();
    handle->engine = engine;
    return reinterpret_cast<jlong>(handle);
}

JNIEXPORT void JNICALL
Java_ai_ocrframe_vlocr_VlEngine_nativeDestroy(JNIEnv* env, jclass, jlong pointer) {
    auto* handle = Handle(pointer);
    if (handle == nullptr) return;
    if (handle->remote != nullptr) env->DeleteGlobalRef(handle->remote);
    if (handle->engine != nullptr) vlcore_destroy(handle->engine);
    delete handle;
}

JNIEXPORT void JNICALL
Java_ai_ocrframe_vlocr_VlEngine_nativeSetRemote(JNIEnv* env, jclass, jlong pointer, jobject recognizer) {
    auto* handle = Handle(pointer);
    if (handle == nullptr || handle->engine == nullptr) return;
    if (handle->remote != nullptr) {
        env->DeleteGlobalRef(handle->remote);
        handle->remote = nullptr;
        handle->remoteMethod = nullptr;
    }
    if (recognizer == nullptr) {
        vlcore_set_remote(handle->engine, nullptr, nullptr);
        return;
    }
    handle->remote = env->NewGlobalRef(recognizer);
    jclass cls = env->GetObjectClass(recognizer);
    handle->remoteMethod = env->GetMethodID(cls, "recognize", "([BIILjava/lang/String;)Ljava/lang/String;");
    env->DeleteLocalRef(cls);
    vlcore_set_remote(handle->engine, RemoteTrampoline, handle);
}

JNIEXPORT jstring JNICALL
Java_ai_ocrframe_vlocr_VlEngine_nativeRunJSON(JNIEnv* env, jclass, jlong pointer,
                                              jbyteArray rgb, jint width, jint height,
                                              jobject progress) {
    auto* handle = Handle(pointer);
    if (handle == nullptr || handle->engine == nullptr) {
        ThrowIllegalState(env, "识别引擎未初始化。");
        return nullptr;
    }
    if (rgb == nullptr || width <= 0 || height <= 0) {
        ThrowIllegalState(env, "待识别图片无效。");
        return nullptr;
    }

    const jsize length = env->GetArrayLength(rgb);
    const jsize expected = width * height * 3;
    if (length != expected) {
        ThrowIllegalState(env, "图像缓冲尺寸与宽高不匹配。");
        return nullptr;
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(length));
    env->GetByteArrayRegion(rgb, 0, length, reinterpret_cast<jbyte*>(buffer.data()));

    handle->env = env;   // 供远程回调使用（同线程，见文件头注释）

    std::pair<JNIEnv*, jobject> progressContext(env, progress);
    char* message = nullptr;
    char* json = vlcore_run(handle->engine, buffer.data(), width, height,
                            progress != nullptr ? ProgressTrampoline : nullptr,
                            progress != nullptr ? &progressContext : nullptr, &message);

    handle->env = nullptr;

    if (json == nullptr) {
        ThrowIllegalState(env, message != nullptr ? std::string(message) : std::string("识别失败。"));
        if (message != nullptr) vlcore_free_string(message);
        return nullptr;
    }
    jstring result = env->NewStringUTF(json);
    vlcore_free_string(json);
    return result;
}

}  // extern "C"
