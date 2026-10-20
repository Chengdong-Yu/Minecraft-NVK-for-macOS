#pragma once
#include <stdint.h>
typedef struct {
    uintptr_t device,queue;
    uint64_t image,usage;
    uint32_t family,width,height,format,layout,pad;
} MCZinkImage;

typedef int (*MCZinkPresent)(const MCZinkImage*,void*);
int mc_zink_current_image(MCZinkImage*);
int mc_zink_present_current(MCZinkPresent,void*);
