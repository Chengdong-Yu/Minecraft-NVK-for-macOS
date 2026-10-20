#include "internal.h"
#include "mc_zink_image.h"
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
static MCZinkPresent present;
static int(*publish)(MCZinkPresent,void*);
static int release_callback(const MCZinkImage*image,void*layer){(void)image;(void)layer;return present(NULL,NULL);}
void _glfwZinkRelease(_GLFWwindow*window){(void)window;if(present&&publish)publish(release_callback,NULL);}
void _glfwZinkPresent(_GLFWwindow*window){
    static int initialized;
    static int(*image)(MCZinkImage*);
    static void(*finish)(void);
    if(!initialized){
        publish=(void*)_glfwPlatformGetModuleSymbol(_glfw.egl.handle,"mc_zink_present_current");
        image=(void*)_glfwPlatformGetModuleSymbol(_glfw.egl.handle,"mc_zink_current_image");
        void*bridge=dlopen(getenv("MC_NVK_LIBRARY") ? getenv("MC_NVK_LIBRARY") : "@loader_path/libvulkan.1.dylib",RTLD_NOW|RTLD_LOCAL);
        present=bridge?dlsym(bridge,"mc_nvk_present_zink"):NULL;
        finish=(void*)eglGetProcAddress("glFinish");
        const unsigned char*(*string)(unsigned)=(void*)eglGetProcAddress("glGetString");
        fprintf(stderr,"[ZINK-GLFW] renderer %s; version %s; direct image query %s; GPU presenter %s\n",string(0x1f01),string(0x1f02),image?"available":"MISSING",present?"available":"MISSING");
        initialized=1;
    }
    if(!image||!present||!publish||!finish){fprintf(stderr,"[ZINK-GLFW] native output unavailable\n");return;}
    finish();MCZinkImage frame={0};
    if(!image(&frame)){fprintf(stderr,"[ZINK-GLFW] no Zink framebuffer image\n");return;}
    int result=publish(present,window->ns.layer);
    if(result){fprintf(stderr,"[ZINK-GLFW] GPU presentation failed %d\n",result);return;}
    static unsigned long frames;frames++;
    if(getenv("MC_ZINK_RESIZE_TEST")&&(frames==600||frames==1200))_glfw.platform.setWindowSize(window,frames==600?1024:1280,frames==600?576:720);
    int w,h;_glfw.platform.getFramebufferSize(window,&w,&h);
    if(w>0&&h>0&&(w!=frame.width||h!=frame.height)){
        _glfwZinkRelease(window);
        typedef EGLSurface(*Create)(EGLDisplay,EGLConfig,const EGLint*);
        Create create=(void*)_glfwPlatformGetModuleSymbol(_glfw.egl.handle,"eglCreatePbufferSurface");
        EGLint attrs[]={0x3057,w,0x3056,h,EGL_NONE};
        EGLSurface next=create(_glfw.egl.display,window->context.egl.config,attrs);
        if(next){EGLSurface old=window->context.egl.surface;window->context.egl.surface=next;
            eglMakeCurrent(_glfw.egl.display,next,next,window->context.egl.handle);eglDestroySurface(_glfw.egl.display,old);}
    }
}
