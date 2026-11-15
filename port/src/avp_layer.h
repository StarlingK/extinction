#ifndef XBOX_FB_LAYER_H
#define XBOX_FB_LAYER_H
#include <stdint.h>

#define XBOX_FB_LAYER_SLOTS 2

typedef struct XboxFbLayer {
    uint32_t *px;
    int w, h;
    int slot;
    int vx, vy, vw, vh;
    int full;
    int dx0, dy0, dx1, dy1;
    int cx0, cy0, cx1, cy1;
} XboxFbLayer;

typedef void (*xbox_fb_layer_hook)(XboxFbLayer *layer);

void xbox_FramebufferSetLayerHook(int slot, xbox_fb_layer_hook fn);
int  xbox_FramebufferLayerReady(void);

#endif
