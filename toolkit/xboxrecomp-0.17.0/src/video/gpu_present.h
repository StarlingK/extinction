#ifndef XBOX_GPU_PRESENT_H
#define XBOX_GPU_PRESENT_H
#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>

int  gpu_present_enabled(void);
int  gpu_present_init(HWND hwnd, uint32_t width, uint32_t height);

int  gpu_present_frame(const uint32_t *pixels);

int  gpu_present_frame_size(const uint32_t *pixels, uint32_t w, uint32_t h);
int  gpu_present_active(void);
void gpu_present_set_vsync(int on);
int  gpu_present_set_exclusive(int on, uint32_t w, uint32_t h);
int  gpu_present_is_exclusive(void);
void gpu_present_shutdown(void);
#endif
#endif
