#include "android_files.h"

#if defined(__ANDROID__)

#include <SDL.h>
#include <jni.h>

namespace materializr {
namespace {

// Resolve (env, activity, MaterializrActivity class). Returns false if anything
// is missing. Caller must DeleteLocalRef(activity) and DeleteLocalRef(clazz).
bool jniActivity(JNIEnv*& env, jobject& activity, jclass& clazz) {
    env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return false;
    clazz = env->GetObjectClass(activity);
    if (!clazz) { env->DeleteLocalRef(activity); return false; }
    return true;
}

void callStaticVoidString(const char* method, const std::string& a) {
    JNIEnv* env; jobject act; jclass cls;
    if (!jniActivity(env, act, cls)) return;
    jmethodID mid = env->GetStaticMethodID(cls, method, "(Ljava/lang/String;)V");
    if (mid) {
        jstring s = env->NewStringUTF(a.c_str());
        env->CallStaticVoidMethod(cls, mid, s);
        env->DeleteLocalRef(s);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(act);
}

void callStaticVoidStringString(const char* method, const std::string& a, const std::string& b) {
    JNIEnv* env; jobject act; jclass cls;
    if (!jniActivity(env, act, cls)) return;
    jmethodID mid = env->GetStaticMethodID(cls, method, "(Ljava/lang/String;Ljava/lang/String;)V");
    if (mid) {
        jstring sa = env->NewStringUTF(a.c_str());
        jstring sb = env->NewStringUTF(b.c_str());
        env->CallStaticVoidMethod(cls, mid, sa, sb);
        env->DeleteLocalRef(sa);
        env->DeleteLocalRef(sb);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(act);
}

// Call a no-arg static method returning String. Empty string on any failure.
std::string callStaticStringNoArg(const char* method) {
    JNIEnv* env; jobject act; jclass cls;
    if (!jniActivity(env, act, cls)) return {};
    std::string out;
    jmethodID mid = env->GetStaticMethodID(cls, method, "()Ljava/lang/String;");
    if (mid) {
        jobject res = env->CallStaticObjectMethod(cls, mid);
        if (res) {
            const char* s = env->GetStringUTFChars(static_cast<jstring>(res), nullptr);
            out = s ? s : "";
            if (s) env->ReleaseStringUTFChars(static_cast<jstring>(res), s);
            env->DeleteLocalRef(res);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(act);
    return out;
}

// Call a String->String static method. Empty string on any failure.
std::string callStaticStringArg(const char* method, const std::string& a) {
    JNIEnv* env; jobject act; jclass cls;
    if (!jniActivity(env, act, cls)) return {};
    std::string out;
    jmethodID mid = env->GetStaticMethodID(cls, method, "(Ljava/lang/String;)Ljava/lang/String;");
    if (mid) {
        jstring sa = env->NewStringUTF(a.c_str());
        jobject res = env->CallStaticObjectMethod(cls, mid, sa);
        env->DeleteLocalRef(sa);
        if (res) {
            const char* s = env->GetStringUTFChars(static_cast<jstring>(res), nullptr);
            out = s ? s : "";
            if (s) env->ReleaseStringUTFChars(static_cast<jstring>(res), s);
            env->DeleteLocalRef(res);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(act);
    return out;
}

} // namespace

bool androidStartOpenDocument(const std::string& mimeCsv) {
    callStaticVoidString("nativeOpenDocument", mimeCsv);
    return true;
}

bool androidStartCreateDocument(const std::string& suggestedName, const std::string& mime) {
    callStaticVoidStringString("nativeCreateDocument", suggestedName, mime);
    return true;
}

void androidShareFile(const std::string& path, const std::string& mime) {
    callStaticVoidStringString("nativeShareFile", path, mime);
}

bool androidPollFileResult(std::string& outValue) {
    JNIEnv* env; jobject act; jclass cls;
    if (!jniActivity(env, act, cls)) return false;
    bool ready = false;
    jmethodID mid = env->GetStaticMethodID(cls, "pollFileResult", "()Ljava/lang/String;");
    if (mid) {
        jobject res = env->CallStaticObjectMethod(cls, mid);
        if (res) {
            const char* s = env->GetStringUTFChars(static_cast<jstring>(res), nullptr);
            outValue = s ? s : "";
            if (s) env->ReleaseStringUTFChars(static_cast<jstring>(res), s);
            env->DeleteLocalRef(res);
            ready = true;
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(act);
    return ready;
}

bool androidCommitSave(const std::string& tempPath) {
    JNIEnv* env; jobject act; jclass cls;
    if (!jniActivity(env, act, cls)) return false;
    jboolean ok = JNI_FALSE;
    jmethodID mid = env->GetStaticMethodID(cls, "nativeCommitSave", "(Ljava/lang/String;)Z");
    if (mid) {
        jstring s = env->NewStringUTF(tempPath.c_str());
        ok = env->CallStaticBooleanMethod(cls, mid, s);
        env->DeleteLocalRef(s);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(act);
    return ok == JNI_TRUE;
}

std::string androidLastDocUri()  { return callStaticStringNoArg("nativeLastDocUri"); }
std::string androidLastDocName() { return callStaticStringNoArg("nativeLastDocName"); }
std::string androidOpenUri(const std::string& uri) {
    return callStaticStringArg("nativeOpenUri", uri);
}

} // namespace materializr

#endif // __ANDROID__
