#ifndef MC_COCOA_BRIDGE_H
#define MC_COCOA_BRIDGE_H
#include <stdint.h>
#include <stddef.h>
void *mc_cocoa_surface_create(const void *container);
void mc_cocoa_surface_destroy(void *surface);
void mc_cocoa_surface_publish(void *surface, void *image);
void mc_cocoa_surface_size(void *surface, uint32_t *width, uint32_t *height);
void *mc_cocoa_iosurface_create(uint32_t width, uint32_t height, size_t pitch, size_t size);
void mc_cocoa_runloop(double seconds);
int mc_cocoa_capture(void *surface, void *image, uint32_t width, uint32_t height, size_t pitch, size_t size, const char *path);
void *mc_cocoa_window_layer(void *window);
void *mc_cocoa_view_layer(void *view);
void mc_cocoa_view_size(void *view, int *width, int *height);
void mc_cocoa_release(void *object);
int mc_cocoa_audit_install(void (*callback)(uint32_t));
#endif
