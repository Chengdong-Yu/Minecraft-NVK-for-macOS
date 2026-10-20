#include <EGL/egl.h>
#include <stdio.h>

void *OSMesaGetProcAddress(const char *name) {
    return (void *)eglGetProcAddress(name);
}

/* LWJGL 3.2 on macOS looks up GL symbols directly, without an extension resolver. */
#define GL_PROC(name) \
    void *name(void) { return (void *)eglGetProcAddress(#name); } \
    __asm__(".symbol_resolver _" #name);
#include "gl_exports.inc"
