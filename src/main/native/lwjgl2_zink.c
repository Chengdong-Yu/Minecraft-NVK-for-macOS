#include "CocoaBridge.h"
#include <jni.h>
#include <EGL/egl.h>
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "mc_zink_image.h"

/* Prefixes of LWJGL 2's macOS peer structures (LWJGL BSD license in notices). */
typedef struct { void *window; struct { double x,y,width,height; } rect; void *view; void *context; } LegacyWindow;
typedef struct {
    bool isCALayer, isWindowed;
    LegacyWindow *window;
    void *pixelFormat, *pbuffer;
    void *parent;
    void *glLayer;
} LegacyPeer;
typedef struct {
    EGLContext context;
    EGLSurface surface;
    LegacyPeer *peer;
    void *layer;
    int width, height, interval;
} LegacyContext;

static EGLDisplay display;
static EGLConfig config;
static int (*publish)(MCZinkPresent, void *);
static int (*query)(MCZinkImage *);
static MCZinkPresent present;
static void (*finish)(void);

static void fail(JNIEnv *env, const char *message) {
    char error[320];snprintf(error,sizeof(error),"%s (EGL %#x)",message,eglGetError());
    jclass type=(*env)->FindClass(env,"org/lwjgl/LWJGLException");
    if(type)(*env)->ThrowNew(env,type,error);
}
static LegacyContext *context(JNIEnv *env,jobject handle) {
    return handle?(*env)->GetDirectBufferAddress(env,handle):NULL;
}
static void *view(LegacyContext *ctx) {
    return ctx->peer&&ctx->peer->isWindowed&&ctx->peer->window?ctx->peer->window->view:ctx->peer?ctx->peer->parent:NULL;
}
static void size(LegacyContext *ctx,int *w,int *h) {
    mc_cocoa_view_size(view(ctx),w,h);
}
static int release_output(const MCZinkImage *image,void *layer) { (void)image;(void)layer;return present(NULL,NULL); }
static void resize(LegacyContext *ctx) {
    int w,h;size(ctx,&w,&h);
    if(ctx->surface&&w==ctx->width&&h==ctx->height)return;
    EGLint attrs[]={EGL_WIDTH,w,EGL_HEIGHT,h,EGL_NONE};
    EGLSurface next=eglCreatePbufferSurface(display,config,attrs);
    if(!next)return;
    if(ctx->surface&&present&&publish)publish(release_output,NULL);
    EGLSurface old=ctx->surface;ctx->surface=next;ctx->width=w;ctx->height=h;
    if(eglGetCurrentContext()==ctx->context)eglMakeCurrent(display,next,next,ctx->context);
    if(old)eglDestroySurface(display,old);
}
static int initialize(JNIEnv *env) {
    if(display)return 1;
    display=eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,NULL);
    if(!display||!eglInitialize(display,NULL,NULL)){display=NULL;fail(env,"Cannot initialize Mesa EGL");return 0;}
    EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,24,EGL_STENCIL_SIZE,8,EGL_NONE};
    EGLint count=0;
    if(!eglChooseConfig(display,attrs,&config,1,&count)||!count){fail(env,"No Mesa OpenGL pbuffer configuration");return 0;}
    void *egl=dlopen(getenv("MC_EGL_LIBRARY"),RTLD_NOW|RTLD_LOCAL);
    void *bridge=dlopen(getenv("MC_NVK_LIBRARY"),RTLD_NOW|RTLD_LOCAL);
    publish=egl?dlsym(egl,"mc_zink_present_current"):NULL;
    query=egl?dlsym(egl,"mc_zink_current_image"):NULL;
    present=bridge?dlsym(bridge,"mc_nvk_present_zink"):NULL;
    finish=(void *)eglGetProcAddress("glFinish");
    if(!publish||!query||!present||!finish){fail(env,"Missing NVK GPU presenter");return 0;}
    return 1;
}
static jobject create(JNIEnv *env,jclass type,jobject peer,jobject shared) {
    (void)type;if(!initialize(env)||!eglBindAPI(EGL_OPENGL_API))return NULL;
    jclass buffer=(*env)->FindClass(env,"java/nio/ByteBuffer");
    jmethodID allocate=(*env)->GetStaticMethodID(env,buffer,"allocateDirect","(I)Ljava/nio/ByteBuffer;");
    jobject handle=(*env)->CallStaticObjectMethod(env,buffer,allocate,(jint)sizeof(LegacyContext));
    if(!handle)return NULL;
    LegacyContext *ctx=context(env,handle),*share=context(env,shared);
    ctx->peer=(*env)->GetDirectBufferAddress(env,peer);
    EGLint attrs[]={EGL_CONTEXT_MAJOR_VERSION,4,EGL_CONTEXT_MINOR_VERSION,6,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,EGL_NONE};
    ctx->context=eglCreateContext(display,config,share?share->context:EGL_NO_CONTEXT,attrs);
    if(!ctx->context){fail(env,"Cannot create Zink compatibility context");return NULL;}
    resize(ctx);if(!ctx->surface){fail(env,"Cannot create Zink framebuffer");return NULL;}
    return handle;
}
static void set_view(JNIEnv *env,jclass type,jobject peer,jobject handle) {
    (void)type;LegacyContext *ctx=context(env,handle);ctx->peer=(*env)->GetDirectBufferAddress(env,peer);
    void *layer=mc_cocoa_view_layer(view(ctx));
    if(layer){mc_cocoa_release(ctx->layer);ctx->layer=layer;}
}
static void make_current(JNIEnv *env,jclass type,jobject handle) {
    (void)type;LegacyContext *ctx=context(env,handle);eglBindAPI(EGL_OPENGL_API);resize(ctx);
    if(!eglMakeCurrent(display,ctx->surface,ctx->surface,ctx->context))fail(env,"Cannot make Zink context current");
    static int logged;
    if(!logged){const unsigned char *(*string)(unsigned)=(void *)eglGetProcAddress("glGetString");
        fprintf(stderr,"[ZINK-LWJGL2] renderer %s; version %s; EGL compatibility context, NVK GPU output\n",string(0x1f01),string(0x1f02));logged=1;}
}
static void swap(JNIEnv *env,jclass type,jobject handle) {
    (void)type;LegacyContext *ctx=context(env,handle);
    finish();MCZinkImage frame={0};if(!query(&frame))return;
    int result=publish(present,ctx->layer);
    if(result){char message[160];snprintf(message,sizeof(message),"LWJGL 2 NVK presentation failed: %d, layer=%p",result,ctx->layer);fail(env,message);return;}
    resize(ctx);
}
static void update(JNIEnv *env,jclass type,jobject handle) { (void)type;LegacyContext *ctx=context(env,handle);if(eglGetCurrentContext()==ctx->context)resize(ctx); }
static void release_current(JNIEnv *env,jclass type) { (void)type;if(!eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT))fail(env,"Cannot release Zink context"); }
static void clear(JNIEnv *env,jclass type,jobject handle) { (void)env;(void)type;(void)handle; }
static jboolean is_current(JNIEnv *env,jclass type,jobject handle) { (void)type;return eglGetCurrentContext()==context(env,handle)->context; }
static void interval(JNIEnv *env,jclass type,jobject handle,jint value) { (void)type;context(env,handle)->interval=value; }
static jlong share_group(JNIEnv *env,jclass type,jobject handle) { (void)type;(void)handle;fail(env,"CGL/OpenCL interoperability is unavailable for an EGL Zink context");return 0; }
static void destroy(JNIEnv *env,jclass type,jobject handle) {
    (void)type;LegacyContext *ctx=context(env,handle);
    if(eglGetCurrentContext()==ctx->context){finish();publish(release_output,NULL);release_current(env,type);}
    eglDestroySurface(display,ctx->surface);eglDestroyContext(display,ctx->context);
    mc_cocoa_release(ctx->layer);ctx->context=NULL;ctx->surface=NULL;ctx->layer=NULL;
}
static jlong address(JNIEnv *env,jclass type,jlong name) { (void)env;(void)type;return (jlong)(uintptr_t)eglGetProcAddress((const char *)(uintptr_t)name); }
static void library(JNIEnv *env,jclass type) { (void)env;(void)type; }

JNIEXPORT void JNICALL Java_local_nvk_NativeEnvironment_installLegacy(JNIEnv *env,jclass type,jclass gl,jclass mac) {
    (void)type;
    JNINativeMethod gl_methods[]={
        {"ngetFunctionAddress","(J)J",address},
        {"nLoadOpenGLLibrary","()V",library}, {"nUnloadOpenGLLibrary","()V",library}};
    JNINativeMethod mac_methods[]={
        {"nCreate","(Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;)Ljava/nio/ByteBuffer;",create},
        {"nSwapBuffers","(Ljava/nio/ByteBuffer;)V",swap},
        {"nUpdate","(Ljava/nio/ByteBuffer;)V",update},
        {"nReleaseCurrentContext","()V",release_current},
        {"clearDrawable","(Ljava/nio/ByteBuffer;)V",clear},
        {"setView","(Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;)V",set_view},
        {"nMakeCurrent","(Ljava/nio/ByteBuffer;)V",make_current},
        {"nIsCurrent","(Ljava/nio/ByteBuffer;)Z",is_current},
        {"nSetSwapInterval","(Ljava/nio/ByteBuffer;I)V",interval},
        {"nDestroy","(Ljava/nio/ByteBuffer;)V",destroy},
        {"getCGLShareGroup","(Ljava/nio/ByteBuffer;)J",share_group}};
    if((*env)->RegisterNatives(env,gl,gl_methods,sizeof(gl_methods)/sizeof(*gl_methods)))return;
    if((*env)->RegisterNatives(env,mac,mac_methods,sizeof(mac_methods)/sizeof(*mac_methods)))return;
    fprintf(stderr,"[ZINK-LWJGL2] Registered LWJGL 2 context and OpenGL dispatch hooks\n");
}
