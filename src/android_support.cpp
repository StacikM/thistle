// Android backend for thistle::Http. A NativeActivity app (hasCode="false")
// has no Java of its own, but the Android framework's classes are always
// there, so this drives java.net.HttpURLConnection over JNI. That also means
// TLS, certificates and proxies come from the system rather than from a
// library we'd have to ship.
//
// Same shape as the Windows and Apple backends: thistle_http_start() runs the
// request on a detached thread and returns at once, the game polls
// thistle_http_poll() each frame. The state is a shared_ptr captured by value
// into the thread, so it stays alive until the request finishes even if the
// caller frees its handle first.
//
// The app's manifest needs <uses-permission android:name="android.permission.INTERNET" />.
// Plain http:// URLs are refused by Android 9+ unless the app opts in to
// cleartext traffic; use https://.
#include <android/native_activity.h>
#include <jni.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" const void* sapp_android_get_native_activity(void);

namespace {

struct ThistleHttp {
    std::atomic<bool> done{false};
    int status = 0;
    std::string body;
};
using HttpPtr = std::shared_ptr<ThistleHttp>;

// True (and clears it) if the last JNI call threw. A request that throws just
// ends with status 0, the same as a connection failure elsewhere.
bool threw(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

void fetch(JNIEnv* env, ThistleHttp& out, const std::string& method, const std::string& url,
           const std::string& body) {
    jclass url_cls = env->FindClass("java/net/URL");
    jclass http_cls = env->FindClass("java/net/HttpURLConnection");
    jclass out_cls = env->FindClass("java/io/OutputStream");
    jclass in_cls = env->FindClass("java/io/InputStream");
    if (threw(env) || !url_cls || !http_cls || !out_cls || !in_cls) return;

    jmethodID url_init = env->GetMethodID(url_cls, "<init>", "(Ljava/lang/String;)V");
    jmethodID open_conn = env->GetMethodID(url_cls, "openConnection", "()Ljava/net/URLConnection;");
    jmethodID set_method = env->GetMethodID(http_cls, "setRequestMethod", "(Ljava/lang/String;)V");
    jmethodID set_connect = env->GetMethodID(http_cls, "setConnectTimeout", "(I)V");
    jmethodID set_read = env->GetMethodID(http_cls, "setReadTimeout", "(I)V");
    jmethodID set_do_output = env->GetMethodID(http_cls, "setDoOutput", "(Z)V");
    jmethodID set_header = env->GetMethodID(http_cls, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
    jmethodID get_out = env->GetMethodID(http_cls, "getOutputStream", "()Ljava/io/OutputStream;");
    jmethodID get_code = env->GetMethodID(http_cls, "getResponseCode", "()I");
    jmethodID get_in = env->GetMethodID(http_cls, "getInputStream", "()Ljava/io/InputStream;");
    jmethodID get_err = env->GetMethodID(http_cls, "getErrorStream", "()Ljava/io/InputStream;");
    jmethodID disconnect = env->GetMethodID(http_cls, "disconnect", "()V");
    jmethodID out_write = env->GetMethodID(out_cls, "write", "([B)V");
    jmethodID out_close = env->GetMethodID(out_cls, "close", "()V");
    jmethodID in_read = env->GetMethodID(in_cls, "read", "([B)I");
    jmethodID in_close = env->GetMethodID(in_cls, "close", "()V");
    if (threw(env)) return;

    jstring jurl = env->NewStringUTF(url.c_str());
    jobject url_obj = env->NewObject(url_cls, url_init, jurl);
    if (threw(env) || !url_obj) return; // malformed URL
    jobject conn = env->CallObjectMethod(url_obj, open_conn);
    if (threw(env) || !conn) return;

    jstring jmethod = env->NewStringUTF(method.c_str());
    env->CallVoidMethod(conn, set_method, jmethod);
    env->CallVoidMethod(conn, set_connect, static_cast<jint>(15000));
    env->CallVoidMethod(conn, set_read, static_cast<jint>(15000));
    if (threw(env)) return;

    if (!body.empty()) {
        env->CallVoidMethod(conn, set_do_output, JNI_TRUE);
        jstring k = env->NewStringUTF("Content-Type");
        jstring v = env->NewStringUTF("application/json");
        env->CallVoidMethod(conn, set_header, k, v);
        jobject os = env->CallObjectMethod(conn, get_out);
        if (threw(env) || !os) { env->CallVoidMethod(conn, disconnect); threw(env); return; }
        jbyteArray bytes = env->NewByteArray(static_cast<jsize>(body.size()));
        env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(body.size()),
                                reinterpret_cast<const jbyte*>(body.data()));
        env->CallVoidMethod(os, out_write, bytes);
        env->CallVoidMethod(os, out_close);
        if (threw(env)) { env->CallVoidMethod(conn, disconnect); threw(env); return; }
    }

    const jint code = env->CallIntMethod(conn, get_code);
    if (threw(env)) { env->CallVoidMethod(conn, disconnect); threw(env); return; }
    out.status = static_cast<int>(code);

    // 4xx/5xx responses carry their body on the error stream instead.
    jobject is = env->CallObjectMethod(conn, code >= 400 ? get_err : get_in);
    if (!threw(env) && is) {
        jbyteArray buf = env->NewByteArray(8192);
        std::vector<jbyte> chunk(8192);
        for (;;) {
            const jint n = env->CallIntMethod(is, in_read, buf);
            if (threw(env) || n < 0) break;
            if (n == 0) continue;
            env->GetByteArrayRegion(buf, 0, n, chunk.data());
            out.body.append(reinterpret_cast<const char*>(chunk.data()), static_cast<size_t>(n));
        }
        env->CallVoidMethod(is, in_close);
        threw(env);
    }
    env->CallVoidMethod(conn, disconnect);
    threw(env);
}

void run_request(HttpPtr sp, std::string method, std::string url, std::string body) {
    const auto* activity = static_cast<const ANativeActivity*>(sapp_android_get_native_activity());
    JavaVM* vm = activity ? activity->vm : nullptr;
    JNIEnv* env = nullptr;
    if (vm && vm->AttachCurrentThread(&env, nullptr) == JNI_OK && env) {
        // Local references pile up in a long-lived native thread; a frame
        // frees every one this request made in a single call.
        if (env->PushLocalFrame(256) == 0) {
            fetch(env, *sp, method, url, body);
            env->PopLocalFrame(nullptr);
        }
        vm->DetachCurrentThread();
    }
    sp->done.store(true);
}

} // namespace

extern "C" void* thistle_http_start(const char* method, const char* url, const char* body) {
    auto sp = std::make_shared<ThistleHttp>();
    if (!url) {
        sp->done.store(true);
        return new HttpPtr(sp);
    }
    std::thread(run_request, sp, std::string(method && *method ? method : "GET"), std::string(url),
                std::string(body ? body : ""))
        .detach();
    return new HttpPtr(sp);
}

extern "C" int thistle_http_poll(void* handle, int* status, const char** body, int* len) {
    auto* spp = static_cast<HttpPtr*>(handle);
    if (!spp || !(*spp) || !(*spp)->done.load()) return 0;
    ThistleHttp& h = **spp;
    if (status) *status = h.status;
    if (body) *body = h.body.c_str();
    if (len) *len = static_cast<int>(h.body.size());
    return 1;
}

extern "C" void thistle_http_free(void* handle) { delete static_cast<HttpPtr*>(handle); }
