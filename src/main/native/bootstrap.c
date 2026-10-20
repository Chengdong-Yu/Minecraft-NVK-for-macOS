#include <jni.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <mach-o/dyld.h>

static void fail(JNIEnv *env, const char *message) {
    jclass exception = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (exception) (*env)->ThrowNew(env, exception, message);
}

JNIEXPORT void JNICALL Java_local_nvk_NativeEnvironment_checkEarlyWindowLibraries(JNIEnv *env, jclass type) {
    (void)type;
    for (uint32_t i=0;i<_dyld_image_count();i++) {
        const char *path=_dyld_get_image_name(i);
        const char *name=path?strrchr(path,'/'):NULL;
        name=name?name+1:path;
        if(name&&(!strcmp(name,"libglfw.dylib")||!strcmp(name,"libglfw.3.dylib")||!strcmp(name,"libSDL3.dylib"))) {
            fail(env,"A window library was loaded before NVK Bootstrap. For Forge/older NeoForge, install with install.py so config/fml.toml has earlyWindowControl = false, then restart the game.");
            return;
        }
    }
}

JNIEXPORT void JNICALL Java_local_nvk_NativeEnvironment_set(JNIEnv *env, jclass type,
        jstring key, jstring value) {
    (void)type;
    const char *name = (*env)->GetStringUTFChars(env, key, NULL);
    if (!name) return;
    const char *text = value ? (*env)->GetStringUTFChars(env, value, NULL) : NULL;
    if (value && !text) { (*env)->ReleaseStringUTFChars(env, key, name); return; }
    int result = value ? setenv(name, text, 1) : unsetenv(name);
    if (text) (*env)->ReleaseStringUTFChars(env, value, text);
    (*env)->ReleaseStringUTFChars(env, key, name);
    if (result != 0) fail(env, "Cannot configure the native rendering environment");
}

JNIEXPORT jstring JNICALL Java_local_nvk_NativeEnvironment_checkDriver(JNIEnv *env, jclass type,
        jstring path) {
    (void)type;
    const char *name = (*env)->GetStringUTFChars(env, path, NULL);
    if (!name) return NULL;
    void *driver = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    (*env)->ReleaseStringUTFChars(env, path, name);
    if (!driver) { fail(env, "Cannot load the installed NVK driver"); return NULL; }
    void *proc = dlsym(driver, "vk_icdGetInstanceProcAddr");
    if (!proc || !dlsym(driver, "nvmtl_nvk_stage_import") || !dlsym(driver, "nvmtl_nvk_surface_dirty")) {
        dlclose(driver);
        fail(env, "The installed NVK driver lacks the private IOSurface import API required by this build");
        return NULL;
    }
    Dl_info info = {0};
    const char *route = dladdr(proc, &info) && info.dli_fname ? info.dli_fname : "NVK driver";
    jstring result = (*env)->NewStringUTF(env, route);
    dlclose(driver);
    return result;
}
