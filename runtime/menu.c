/* The in-game menu's stand-ins for a build without it (include/psxstack/menu.h): a build without PSXSTACK_MENU,
 * headless or with the window, links these so that the runtime's call sites (video.c, pump.c, input.c) need no
 * #ifdef. With PSXSTACK_MENU, launcher/ui/menu.cpp defines them and this unit is empty. */
#include "menu.h"

#ifndef PSXSTACK_MENU

int port_menu_compiled(void) {
    return 0;
}

void port_menu_open_renderer(struct SDL_Window *window, struct SDL_Renderer *renderer) {
    (void)window;
    (void)renderer;
}

void port_menu_open_gpu(struct SDL_Window *window, struct SDL_GPUDevice *device) {
    (void)window;
    (void)device;
}

void port_menu_close(void) {
}

int port_menu_shown(void) {
    return 0;
}

void port_menu_show(int on) {
    (void)on;
}

void port_menu_event(const union SDL_Event *e) {
    (void)e;
}

int port_menu_frame(void) {
    return PORT_MENU_NONE;
}

void port_menu_render_renderer(struct SDL_Renderer *renderer) {
    (void)renderer;
}

void port_menu_render_gpu(struct SDL_GPUCommandBuffer *cb, struct SDL_GPUTexture *swapchain) {
    (void)cb;
    (void)swapchain;
}

int port_menu_shot_due(void) {
    return 0;
}

void port_menu_shot_save(const void *pixels, int w, int h, int bgra) {
    (void)pixels;
    (void)w;
    (void)h;
    (void)bgra;
}

#else
typedef int psxstack_menu_c_is_empty; /* ISO C forbids an empty translation unit */
#endif
