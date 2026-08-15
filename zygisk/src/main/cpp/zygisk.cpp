#include "zygisk.hpp"
#include "checksum.h"
#include "Dobby/include/dobby.h"
#include "pif_config.hpp"
#include "ipc.hpp"

#include <algorithm>
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <jni.h>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "PIF", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PIF", __VA_ARGS__)

#define DEX_PATH "/data/adb/modules/playintegrityfix/classes.dex"
#define MODULE_PROP "/data/adb/modules/playintegrityfix/module.prop"
#define DEFAULT_PIF "/data/adb/modules/playintegrityfix/pif.prop"
#define CUSTOM_PIF "/data/adb/pif.prop"
#define SCRIPT_ONLY_FLAG "/data/adb/pif_script_only"
#define UPDATE_FLAG "/data/adb/modules/playintegrityfix/update"

#define VENDING_PACKAGE "com.android.vending"
#define DROIDGUARD_PACKAGE "com.google.android.gms.unstable"

namespace {

constexpr uint8_t COMMAND_LOAD_PAYLOAD = 1;
constexpr int PAYLOAD_TIMEOUT_MS = 5000;

JNIEnv *gEnv = nullptr;
pif::Config gConfig;
std::vector<uint8_t> gDexBytes;

using T_Callback = void (*)(void *, const char *, const char *, uint32_t);

thread_local T_Callback o_callback = nullptr;
void (*o_system_property_read_callback)(prop_info *, T_Callback, void *) = nullptr;

struct FileStamp {
    bool exists = false;
    time_t mtime = 0;
    off_t size = 0;
};

struct CompanionCache {
    std::mutex mutex;
    bool loaded = false;
    bool verifyOk = false;
    bool updateFlag = false;
    FileStamp moduleStamp;
    FileStamp customPifStamp;
    FileStamp defaultPifStamp;
    FileStamp dexStamp;
    pif::Config config;
    std::vector<uint8_t> dexBytes;
};

CompanionCache gCache;

class JniLocal {
public:
    JniLocal(JNIEnv *env, jobject obj) : env(env), obj(obj) {}

    ~JniLocal() {
        if (env && obj) {
            env->DeleteLocalRef(obj);
        }
    }

    JniLocal(const JniLocal &) = delete;
    JniLocal &operator=(const JniLocal &) = delete;

    jobject get() const { return obj; }

private:
    JNIEnv *env;
    jobject obj;
};

FileStamp stampOf(const char *path) {
    FileStamp stamp;
    struct stat st{};
    if (stat(path, &st) == 0) {
        stamp.exists = true;
        stamp.mtime = st.st_mtime;
        stamp.size = st.st_size;
    }
    return stamp;
}

bool sameStamp(const FileStamp &a, const FileStamp &b) {
    return a.exists == b.exists && a.mtime == b.mtime && a.size == b.size;
}

void applySocketTimeout(int fd) {
    const timeval timeout{
            .tv_sec = PAYLOAD_TIMEOUT_MS / 1000,
            .tv_usec = static_cast<suseconds_t>((PAYLOAD_TIMEOUT_MS % 1000) * 1000),
    };

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

bool readFileBytes(const char *path, std::vector<uint8_t> &out) {
    out.clear();

    const int file = open(path, O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        return false;
    }

    struct stat st{};
    if (fstat(file, &st) == 0 && st.st_size > 0) {
        out.resize(static_cast<size_t>(st.st_size));
        const bool ok = pif::ipc::readExact(file, out.data(), out.size());
        close(file);
        return ok;
    }

    std::vector<uint8_t> buffer(4096);
    ssize_t bytes = 0;
    while ((bytes = TEMP_FAILURE_RETRY(read(file, buffer.data(), buffer.size()))) > 0) {
        out.insert(out.end(), buffer.begin(), buffer.begin() + bytes);
    }

    close(file);
    return bytes == 0 && !out.empty();
}

bool loadPropBytes(std::vector<uint8_t> &out) {
    if (readFileBytes(CUSTOM_PIF, out)) {
        return true;
    }
    return readFileBytes(DEFAULT_PIF, out);
}

uint32_t crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320U & (-static_cast<int>(crc & 1)));
        }
    }
    return ~crc;
}

bool rewriteTamperedDescription(const char *path, const std::vector<uint8_t> &buf) {
    const int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    std::vector<std::string> lines;
    const std::string fileStr(buf.begin(), buf.end());
    size_t pos = 0;
    while (pos < fileStr.size()) {
        const size_t next = fileStr.find('\n', pos);
        std::string line = fileStr.substr(pos, next == std::string::npos ? std::string::npos : next - pos + 1);
        if (line.rfind("description=", 0) == 0) {
            line = "description=❌ This module has been tampered, please install from official source.\n";
        }
        lines.push_back(std::move(line));
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }

    if (ftruncate(fd, 0) != 0) {
        close(fd);
        return false;
    }

    lseek(fd, 0, SEEK_SET);
    for (const auto &line : lines) {
        if (pif::ipc::xwrite(fd, line.c_str(), line.size()) != static_cast<ssize_t>(line.size())) {
            close(fd);
            return false;
        }
    }

    close(fd);
    return false;
}

bool verifyModule(const char *path, const char *expectedHex) {
    if (access(UPDATE_FLAG, F_OK) == 0) {
        return true;
    }

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    std::vector<uint8_t> buf;
    uint8_t tmp[512];
    ssize_t n = 0;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0) {
        buf.insert(buf.end(), tmp, tmp + n);
    }
    close(fd);

    if (buf.empty()) {
        return false;
    }

    const uint32_t crc = crc32(buf.data(), buf.size());
    uint32_t expectedCrc = 0;
    sscanf(expectedHex, "%x", &expectedCrc);

    if (crc == expectedCrc) {
        return true;
    }

    LOGD("[COMPANION] module tampered!");
    return rewriteTamperedDescription(path, buf);
}

void appendJsonEscaped(std::string &out, std::string_view value) {
    out.push_back('"');
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "\\u%04x", ch);
                    out += hex;
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    out.push_back('"');
}

std::string propMapToJson() {
    std::string json = "{";
    bool first = true;
    for (const auto &[key, value] : gConfig.propMap) {
        if (!first) {
            json += ",";
        }
        first = false;
        appendJsonEscaped(json, key);
        json += ":";
        appendJsonEscaped(json, value);
    }
    json += "}";
    return json;
}

void modifyCallback(void *cookie, const char *name, const char *value, uint32_t serial) {
    if (!cookie || !name || !value || !o_callback) {
        return;
    }

    const char *oldValue = value;
    const std::string_view prop(name);

    if (prop == "init.svc.adbd") {
        value = "stopped";
    } else if (prop == "sys.usb.state") {
        value = "mtp";
    } else if (prop.ends_with("api_level")) {
        if (!gConfig.deviceInitialSdkInt.empty()) {
            value = gConfig.deviceInitialSdkInt.c_str();
        }
    } else if (prop.ends_with(".security_patch")) {
        if (!gConfig.securityPatch.empty()) {
            value = gConfig.securityPatch.c_str();
        }
    } else if (prop.ends_with(".build.id")) {
        if (!gConfig.buildId.empty()) {
            value = gConfig.buildId.c_str();
        }
    }

    if (gConfig.debug) {
        if (strcmp(oldValue, value) == 0) {
            LOGD("[%s]: %s (unchanged)", name, oldValue);
        } else {
            LOGD("[%s]: %s -> %s", name, oldValue, value);
        }
    }

    o_callback(cookie, name, value, serial);
}

void systemPropertyReadCallback(prop_info *pi, T_Callback callback, void *cookie) {
    if (pi && callback && cookie) {
        o_callback = callback;
    }
    o_system_property_read_callback(pi, modifyCallback, cookie);
}

bool doHook() {
    void *ptr = DobbySymbolResolver(nullptr, "__system_property_read_callback");
    if (ptr && DobbyHook(ptr, reinterpret_cast<void *>(systemPropertyReadCallback),
                         reinterpret_cast<void **>(&o_system_property_read_callback)) == 0) {
        if (gConfig.debug) {
            LOGD("hook __system_property_read_callback successful at %p", ptr);
        }
        return true;
    }

    LOGE("hook __system_property_read_callback failed!");
    return false;
}

void doSpoofVending() {
    constexpr int requestSdk = 32;

    jclass buildVersionClass = gEnv->FindClass("android/os/Build$VERSION");
    if (buildVersionClass == nullptr) {
        LOGE("Build.VERSION class not found");
        gEnv->ExceptionClear();
        return;
    }
    JniLocal versionRef(gEnv, buildVersionClass);

    jfieldID sdkIntFieldId = gEnv->GetStaticFieldID(buildVersionClass, "SDK_INT", "I");
    if (sdkIntFieldId == nullptr) {
        LOGE("SDK_INT field not found");
        gEnv->ExceptionClear();
        return;
    }

    const int oldValue = gEnv->GetStaticIntField(buildVersionClass, sdkIntFieldId);
    const int targetSdk = std::min(oldValue, requestSdk);
    if (oldValue == targetSdk) {
        return;
    }

    gEnv->SetStaticIntField(buildVersionClass, sdkIntFieldId, targetSdk);
    if (gEnv->ExceptionCheck()) {
        gEnv->ExceptionDescribe();
        gEnv->ExceptionClear();
        LOGE("SDK_INT field not accessible (JNI Exception)");
    } else if (gConfig.debug) {
        LOGD("[SDK_INT]: %d -> %d", oldValue, targetSdk);
    }
}

void updateBuildFields() {
    jclass buildClass = gEnv->FindClass("android/os/Build");
    jclass versionClass = gEnv->FindClass("android/os/Build$VERSION");
    if (buildClass == nullptr || versionClass == nullptr) {
        gEnv->ExceptionClear();
        if (buildClass) {
            gEnv->DeleteLocalRef(buildClass);
        }
        if (versionClass) {
            gEnv->DeleteLocalRef(versionClass);
        }
        return;
    }
    JniLocal buildRef(gEnv, buildClass);
    JniLocal versionRef(gEnv, versionClass);

    for (const auto &[key, value] : gConfig.propMap) {
        jclass targetClass = buildClass;
        jfieldID fieldId = gEnv->GetStaticFieldID(buildClass, key.c_str(), "Ljava/lang/String;");
        if (gEnv->ExceptionCheck()) {
            gEnv->ExceptionClear();
            fieldId = gEnv->GetStaticFieldID(versionClass, key.c_str(), "Ljava/lang/String;");
            targetClass = versionClass;
            if (gEnv->ExceptionCheck()) {
                gEnv->ExceptionClear();
                continue;
            }
        }

        jstring jValue = gEnv->NewStringUTF(value.c_str());
        if (jValue == nullptr) {
            gEnv->ExceptionClear();
            continue;
        }
        JniLocal valueRef(gEnv, jValue);

        gEnv->SetStaticObjectField(targetClass, fieldId, jValue);
        if (gEnv->ExceptionCheck()) {
            gEnv->ExceptionClear();
            continue;
        }

        if (gConfig.debug) {
            LOGD("Set '%s' to '%s'", key.c_str(), value.c_str());
        }
    }
}

void injectDex() {
    if (gDexBytes.empty()) {
        if (gConfig.debug) {
            LOGD("[INJECT] No dex payload available");
        }
        return;
    }

    jclass classLoaderClass = gEnv->FindClass("java/lang/ClassLoader");
    if (classLoaderClass == nullptr) {
        gEnv->ExceptionClear();
        return;
    }
    JniLocal classLoaderRef(gEnv, classLoaderClass);

    jmethodID getSystemClassLoader = gEnv->GetStaticMethodID(
            classLoaderClass, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    jobject systemClassLoader = gEnv->CallStaticObjectMethod(classLoaderClass, getSystemClassLoader);
    if (gEnv->ExceptionCheck() || systemClassLoader == nullptr) {
        gEnv->ExceptionDescribe();
        gEnv->ExceptionClear();
        return;
    }
    JniLocal systemLoaderRef(gEnv, systemClassLoader);

    jobject dexBuffer = gEnv->NewDirectByteBuffer(gDexBytes.data(), static_cast<jlong>(gDexBytes.size()));
    if (dexBuffer == nullptr) {
        gEnv->ExceptionClear();
        return;
    }
    JniLocal dexBufferRef(gEnv, dexBuffer);

    jclass inMemoryClassLoaderClass = gEnv->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (inMemoryClassLoaderClass == nullptr) {
        gEnv->ExceptionClear();
        return;
    }
    JniLocal inMemoryClassRef(gEnv, inMemoryClassLoaderClass);

    jmethodID inMemoryClassLoaderInit = gEnv->GetMethodID(
            inMemoryClassLoaderClass, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    jobject dexClassLoader = gEnv->NewObject(
            inMemoryClassLoaderClass, inMemoryClassLoaderInit, dexBuffer, systemClassLoader);
    if (gEnv->ExceptionCheck() || dexClassLoader == nullptr) {
        gEnv->ExceptionDescribe();
        gEnv->ExceptionClear();
        return;
    }
    JniLocal dexLoaderRef(gEnv, dexClassLoader);

    jmethodID loadClass = gEnv->GetMethodID(
            classLoaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring entryClassName = gEnv->NewStringUTF("es.chiteroman.playintegrityfix.EntryPoint");
    if (entryClassName == nullptr) {
        gEnv->ExceptionClear();
        return;
    }
    JniLocal entryNameRef(gEnv, entryClassName);

    jobject entryClassObject = gEnv->CallObjectMethod(dexClassLoader, loadClass, entryClassName);
    if (gEnv->ExceptionCheck() || entryClassObject == nullptr) {
        gEnv->ExceptionDescribe();
        gEnv->ExceptionClear();
        return;
    }
    JniLocal entryClassRef(gEnv, entryClassObject);

    auto entryPointClass = static_cast<jclass>(entryClassObject);
    jmethodID entryInit = gEnv->GetStaticMethodID(entryPointClass, "init", "(Ljava/lang/String;ZZZ)V");
    const std::string json = propMapToJson();
    jstring jsonString = gEnv->NewStringUTF(json.c_str());
    if (jsonString == nullptr) {
        gEnv->ExceptionClear();
        return;
    }
    JniLocal jsonRef(gEnv, jsonString);

    gEnv->CallStaticVoidMethod(entryPointClass, entryInit, jsonString, gConfig.spoofProvider,
                               gConfig.spoofSignature, gConfig.spoofBuild);
    if (gEnv->ExceptionCheck()) {
        gEnv->ExceptionDescribe();
        gEnv->ExceptionClear();
    }
}

bool requestPayload(int fd) {
    if (fd < 0) {
        return false;
    }

    applySocketTimeout(fd);

    bool ok = pif::ipc::writeExact(fd, &COMMAND_LOAD_PAYLOAD, sizeof(COMMAND_LOAD_PAYLOAD));
    uint8_t companionOk = 0;
    ok = ok && pif::ipc::readExact(fd, &companionOk, sizeof(companionOk));
    if (!ok || companionOk == 0) {
        close(fd);
        return false;
    }

    ok = readConfig(fd, gConfig);
    if (ok && gConfig.needsDex()) {
        ok = pif::ipc::readVector(fd, gDexBytes);
    } else {
        gDexBytes.clear();
    }

    close(fd);
    if (!ok) {
        gDexBytes.clear();
        gConfig = {};
        return false;
    }
    return true;
}

bool ensureCachedPayload() {
    const FileStamp moduleStamp = stampOf(MODULE_PROP);
    const FileStamp customPifStamp = stampOf(CUSTOM_PIF);
    const FileStamp defaultPifStamp = stampOf(DEFAULT_PIF);
    const FileStamp dexStamp = stampOf(DEX_PATH);
    const bool updateFlag = access(UPDATE_FLAG, F_OK) == 0;

    const bool verifyFresh = gCache.verifyOk
            && sameStamp(gCache.moduleStamp, moduleStamp)
            && gCache.updateFlag == updateFlag;
    if (!verifyFresh) {
        gCache.verifyOk = verifyModule(MODULE_PROP, MODULE_PROP_CHECKSUM_HEX);
        gCache.moduleStamp = moduleStamp;
        gCache.updateFlag = updateFlag;
        if (!gCache.verifyOk) {
            gCache.loaded = false;
            return false;
        }
    } else if (!gCache.verifyOk) {
        return false;
    }

    if (gCache.loaded
            && sameStamp(gCache.customPifStamp, customPifStamp)
            && sameStamp(gCache.defaultPifStamp, defaultPifStamp)
            && sameStamp(gCache.dexStamp, dexStamp)) {
        return true;
    }

    std::vector<uint8_t> propBytes;
    if (!loadPropBytes(propBytes)) {
        gCache.loaded = false;
        return false;
    }

    pif::Config parsed = pif::parseConfig(std::string_view(
            reinterpret_cast<const char *>(propBytes.data()), propBytes.size()));

    std::vector<uint8_t> dexBytes;
    if (parsed.needsDex() && !readFileBytes(DEX_PATH, dexBytes)) {
        gCache.loaded = false;
        return false;
    }

    gCache.config = std::move(parsed);
    gCache.dexBytes = std::move(dexBytes);
    gCache.customPifStamp = customPifStamp;
    gCache.defaultPifStamp = defaultPifStamp;
    gCache.dexStamp = dexStamp;
    gCache.loaded = true;
    return true;
}

void companion(int fd) {
    applySocketTimeout(fd);

    uint8_t command = 0;
    bool ok = pif::ipc::readExact(fd, &command, sizeof(command));
    ok = ok && command == COMMAND_LOAD_PAYLOAD;

    {
        std::lock_guard<std::mutex> lock(gCache.mutex);
        if (ok) {
            ok = ensureCachedPayload();
        }

        ok = pif::ipc::writeBool(fd, ok) && ok;
        if (!ok) {
            return;
        }

        ok = writeConfig(fd, gCache.config);
        if (ok && gCache.config.needsDex()) {
            ok = pif::ipc::writeVector(fd, gCache.dexBytes);
        }
    }

    if (!ok) {
        LOGE("[COMPANION] failed to send payload");
    }
}

}

using namespace zygisk;

class PlayIntegrityFix : public ModuleBase {
public:
    void onLoad(Api *api_, JNIEnv *env_) override {
        api = api_;
        env = env_;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        payloadLoaded = false;
        isGmsUnstable = false;
        isVending = false;
        gConfig = {};
        gDexBytes.clear();

        if (!args || !env) {
            api->setOption(DLCLOSE_MODULE_LIBRARY);
            return;
        }

        std::string dir;
        if (args->app_data_dir) {
            const char *rawDir = env->GetStringUTFChars(args->app_data_dir, nullptr);
            if (rawDir) {
                dir = rawDir;
                env->ReleaseStringUTFChars(args->app_data_dir, rawDir);
            }
        }

        const std::string_view appDir(dir);
        const bool isGms = appDir.ends_with("/com.google.android.gms") || appDir.ends_with("/com.android.vending");
        if (!isGms) {
            api->setOption(DLCLOSE_MODULE_LIBRARY);
            return;
        }

        api->setOption(FORCE_DENYLIST_UNMOUNT);

        std::string name;
        if (args->nice_name) {
            const char *rawName = env->GetStringUTFChars(args->nice_name, nullptr);
            if (rawName) {
                name = rawName;
                env->ReleaseStringUTFChars(args->nice_name, rawName);
            }
        }

        const std::string_view niceName(name);
        isGmsUnstable = niceName == DROIDGUARD_PACKAGE;
        isVending = niceName == VENDING_PACKAGE;
        if (!isGmsUnstable && !isVending) {
            api->setOption(DLCLOSE_MODULE_LIBRARY);
            return;
        }

        if (access(SCRIPT_ONLY_FLAG, F_OK) == 0) {
            api->setOption(DLCLOSE_MODULE_LIBRARY);
            return;
        }

        payloadLoaded = requestPayload(api->connectCompanion());
        if (!payloadLoaded) {
            api->setOption(DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const AppSpecializeArgs *args) override {
        if (!payloadLoaded) {
            return;
        }

        gEnv = env;

        if (isGmsUnstable) {
            if (gConfig.spoofBuild) {
                updateBuildFields();
            }

            if (gConfig.needsDex()) {
                injectDex();
            } else if (gConfig.debug) {
                LOGD("[INJECT] Dex payload skipped because spoofProvider and spoofSignature are false");
            }

            if (gConfig.spoofProps) {
                doHook();
            }
        } else if (isVending) {
            if (gConfig.spoofVendingBuild) {
                updateBuildFields();
            } else if (gConfig.spoofVendingSdk) {
                doSpoofVending();
            }
        }
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        api->setOption(DLCLOSE_MODULE_LIBRARY);
    }

private:
    Api *api = nullptr;
    JNIEnv *env = nullptr;
    bool payloadLoaded = false;
    bool isGmsUnstable = false;
    bool isVending = false;
};

REGISTER_ZYGISK_MODULE(PlayIntegrityFix)
REGISTER_ZYGISK_COMPANION(companion)
