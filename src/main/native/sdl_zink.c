#include "CocoaBridge.h"
#include <EGL/egl.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "mc_zink_image.h"

typedef void SDL_Window;
typedef uint32_t SDL_PropertiesID;
typedef struct Context {
    SDL_Window *window;
    EGLContext context;
    EGLSurface surface;
    void *layer;
    int width, height, interval;
} Context;
static _Thread_local Context *current;
static EGLDisplay display;
static EGLConfig config;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void *original;
static int attributes[40] = {[0]=8,[1]=8,[2]=8,[3]=8,[5]=1,[6]=24,[7]=8,[17]=3,[18]=3,[21]=1};
static MCZinkPresent present;
static int (*publish)(MCZinkPresent,void*);

static void *symbol(const char *name) {
    if (!original) original=dlopen(getenv("MC_SDL_ORIGINAL"), RTLD_NOW|RTLD_LOCAL);
    void *function=original?dlsym(original,name):NULL;
    if (!function) { fprintf(stderr,"[NVK-SDL] Missing original SDL symbol %s: %s\n",name,dlerror()); abort(); }
    return function;
}
#define ORIGINAL(name, ret, args) ((ret (*) args)symbol(#name))
static bool error(const char *message) {
    ORIGINAL(SDL_SetError, bool, (const char*,...))("NVK/Zink: %s (EGL 0x%x)",message,eglGetError());
    fprintf(stderr,"[NVK-SDL] %s\n",message);
    return false;
}
static void initialize(void) {
    typedef EGLDisplay (*GetPlatformDisplay)(EGLenum,void*,const EGLint*);
    GetPlatformDisplay get=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    display=get?get(0x31dd,NULL,NULL):EGL_NO_DISPLAY;
    if (display==EGL_NO_DISPLAY || !eglInitialize(display,NULL,NULL) || !eglBindAPI(EGL_OPENGL_API)) { error("Cannot initialize Mesa surfaceless EGL"); return; }
    EGLint count=0;
    const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,24,EGL_STENCIL_SIZE,8,EGL_NONE};
    if (!eglChooseConfig(display,attrs,&config,1,&count) || !count) { config=NULL;error("Cannot select EGL configuration");return; }
    void *bridge=dlopen(getenv("MC_NVK_LIBRARY"),RTLD_NOW|RTLD_LOCAL);
    present=bridge?dlsym(bridge,"mc_nvk_present_zink"):NULL;
    void *egl=dlopen(getenv("MC_EGL_LIBRARY"),RTLD_NOW|RTLD_LOCAL);
    publish=egl?dlsym(egl,"mc_zink_present_current"):NULL;
    fprintf(stderr,"[NVK-SDL] Mesa EGL initialized; OpenGL window contexts route through Zink\n");
}

SDL_Window *SDL_CreateWindow(const char *title,int w,int h,uint64_t flags) {
    return ORIGINAL(SDL_CreateWindow,SDL_Window*,(const char*,int,int,uint64_t))(title,w,h,flags&~UINT64_C(2));
}
SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID props) {
    bool gl=ORIGINAL(SDL_GetBooleanProperty,bool,(SDL_PropertiesID,const char*,bool))(props,"SDL.window.create.opengl",false);
    if (gl) ORIGINAL(SDL_SetBooleanProperty,bool,(SDL_PropertiesID,const char*,bool))(props,"SDL.window.create.opengl",false);
    SDL_Window *window=ORIGINAL(SDL_CreateWindowWithProperties,SDL_Window*,(SDL_PropertiesID))(props);
    if (gl) ORIGINAL(SDL_SetBooleanProperty,bool,(SDL_PropertiesID,const char*,bool))(props,"SDL.window.create.opengl",true);
    return window;
}
bool SDL_GL_LoadLibrary(const char *path) { (void)path;pthread_once(&once,initialize);return display&&config; }
void SDL_GL_UnloadLibrary(void) {}
void *SDL_GL_GetProcAddress(const char *name) { return (void*)eglGetProcAddress(name); }
bool SDL_GL_SetAttribute(int attribute,int value) {
    if (attribute<0||attribute>=40) return error("Unsupported SDL GL attribute");
    attributes[attribute]=value;return true;
}
bool SDL_GL_GetAttribute(int attribute,int *value) {
    if (attribute<0||attribute>=40||!value) return error("Unsupported SDL GL attribute query");
    *value=attributes[attribute];return true;
}
void SDL_GL_ResetAttributes(void) {
    memset(attributes,0,sizeof(attributes));attributes[0]=attributes[1]=attributes[2]=attributes[3]=8;
    attributes[5]=1;attributes[6]=24;attributes[7]=8;attributes[17]=3;attributes[18]=3;attributes[21]=1;
}
void *SDL_GL_CreateContext(SDL_Window *window) {
    pthread_once(&once,initialize);
    if (!display||!config) return NULL;
    Context *c=calloc(1,sizeof(*c));c->window=window;
    ORIGINAL(SDL_GetWindowSizeInPixels,bool,(SDL_Window*,int*,int*))(window,&c->width,&c->height);
    EGLint attrs[]={EGL_WIDTH,c->width,EGL_HEIGHT,c->height,EGL_NONE};
    c->surface=eglCreatePbufferSurface(display,config,attrs);
    EGLint ctx[]={0x3098,attributes[17],0x30fb,attributes[18],0x30fd,attributes[21]==2?2:1,EGL_NONE};
    c->context=eglCreateContext(display,config,attributes[22]&&current?current->context:EGL_NO_CONTEXT,ctx);
    if (!c->surface||!c->context||!eglMakeCurrent(display,c->surface,c->surface,c->context)) {
        error("Cannot create SDL OpenGL/Zink context");
        if(c->context)eglDestroyContext(display,c->context);if(c->surface)eglDestroySurface(display,c->surface);free(c);return NULL;
    }
    SDL_PropertiesID props=ORIGINAL(SDL_GetWindowProperties,SDL_PropertiesID,(SDL_Window*))(window);
    void *native=ORIGINAL(SDL_GetPointerProperty,void*,(SDL_PropertiesID,const char*,void*))(props,"SDL.window.cocoa.window",NULL);
    c->layer=mc_cocoa_window_layer(native);
    if (!c->layer) { error("Cannot access SDL Cocoa window");eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(display,c->context);eglDestroySurface(display,c->surface);free(c);return NULL; }
    current=c;
    fprintf(stderr,"[NVK-SDL] Created OpenGL/Zink context %dx%d on ordinary Cocoa CALayer\n",c->width,c->height);
    return c;
}
bool SDL_GL_MakeCurrent(SDL_Window *window,void *context) {
    Context *c=context;
    bool ok=eglMakeCurrent(display,c?c->surface:EGL_NO_SURFACE,c?c->surface:EGL_NO_SURFACE,c?c->context:EGL_NO_CONTEXT);
    if(ok) {
        current=c;
        if(c&&window&&window!=c->window) {
            SDL_PropertiesID props=ORIGINAL(SDL_GetWindowProperties,SDL_PropertiesID,(SDL_Window*))(window);
            void *native=ORIGINAL(SDL_GetPointerProperty,void*,(SDL_PropertiesID,const char*,void*))(props,"SDL.window.cocoa.window",NULL);
            void *layer=mc_cocoa_window_layer(native);
            if(!layer)return error("Cannot bind new SDL Cocoa window");
            mc_cocoa_release(c->layer);c->layer=layer;c->window=window;
            fprintf(stderr,"[NVK-SDL] Rebound OpenGL/Zink context to game window\n");
        }
    }
    return ok;
}
void *SDL_GL_GetCurrentContext(void) { return current; }
SDL_Window *SDL_GL_GetCurrentWindow(void) { return current?current->window:NULL; }
bool SDL_GL_SetSwapInterval(int interval) { if(!current)return error("No current context");current->interval=interval;return true; }
bool SDL_GL_GetSwapInterval(int *interval) { if(!current||!interval)return error("No current context");*interval=current->interval;return true; }
static int release(const MCZinkImage *image,void *layer) { (void)image;(void)layer;return present(NULL,NULL); }
bool SDL_GL_SwapWindow(SDL_Window *window) {
    if(!current||current->window!=window||!present||!publish)return error("No current Zink window/presenter");
    void (*finish)(void)=(void*)eglGetProcAddress("glFinish");finish();
    int result=publish(present,current->layer);if(result)return error("NVK GPU presentation failed");
    int w,h;ORIGINAL(SDL_GetWindowSizeInPixels,bool,(SDL_Window*,int*,int*))(window,&w,&h);
    if(w>0&&h>0&&(w!=current->width||h!=current->height)) {
        publish(release,NULL);EGLint attrs[]={EGL_WIDTH,w,EGL_HEIGHT,h,EGL_NONE};
        EGLSurface next=eglCreatePbufferSurface(display,config,attrs);
        if(!next)return error("Cannot resize SDL EGL pbuffer");
        if(!eglMakeCurrent(display,next,next,current->context)){eglDestroySurface(display,next);return error("Cannot bind resized pbuffer");}
        eglDestroySurface(display,current->surface);current->surface=next;current->width=w;current->height=h;
        fprintf(stderr,"[NVK-SDL] Resized OpenGL/Zink pbuffer to %dx%d\n",w,h);
    }
    return true;
}
bool SDL_GL_DestroyContext(void *context) {
    Context *c=context;if(!c)return true;
    if(current==c){if(present&&publish)publish(release,NULL);eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);current=NULL;}
    eglDestroyContext(display,c->context);eglDestroySurface(display,c->surface);mc_cocoa_release(c->layer);free(c);return true;
}
bool SDL_GL_ExtensionSupported(const char *extension) {
    const unsigned char *(*getString)(unsigned)=(void*)eglGetProcAddress("glGetString");
    const char *all=(const char*)getString(0x1f03);return all&&extension&&strstr(all,extension);
}
