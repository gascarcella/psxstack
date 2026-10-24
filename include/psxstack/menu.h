/* The in-game menu (docs/LAUNCHER.md "In-game menu"): the launcher's screens drawn inside the game's window while the
 * game is paused, opened by the `menu` hotkey (input.c). Implemented in C++ over Dear ImGui by launcher/ui/menu.cpp
 * in a build configured with -DPSXSTACK_MENU=ON (PSXSTACK_MENU, which needs PSXSTACK_SDL); runtime/menu.c
 * supplies the do-nothing stubs otherwise, so the runtime calls these without an #ifdef. The window (video.c) opens
 * and closes it and gives it the present to draw into; the pump (pump.c) shows it in the pause and runs its frames;
 * input.c hands it the window's events while it is shown. */
#pragma once

struct SDL_Window;
struct SDL_Renderer;
struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;
union SDL_Event;

#ifdef __cplusplus
extern "C" {
#endif

/* What the menu's frame asks of the pump. */
enum {
    PORT_MENU_NONE = 0,
    PORT_MENU_RESUME, /* close the menu and go on with the game */
    PORT_MENU_QUIT,   /* end the run (status 0) */
};

int port_menu_compiled(void); /* 1 in a build with PSXSTACK_MENU */
/* Opens the menu on the window once its present path is known: the software renderer's SDL_Renderer, or the
 * hardware renderer's device (the window claimed). Both log what they did. */
void port_menu_open_renderer(struct SDL_Window *window, struct SDL_Renderer *renderer);
void port_menu_open_gpu(struct SDL_Window *window, struct SDL_GPUDevice *device);
void port_menu_close(void); /* before the renderer or the device goes */

int port_menu_shown(void); /* the menu is up (the pump holds the game paused meanwhile) */
void port_menu_show(int on);
/* While shown, every event of the window's poll (input.c) goes here before the runtime's own handling. */
void port_menu_event(const union SDL_Event *e);
/* While shown, once per poll of the pause (pump.c): builds the menu's frame; the next present draws it. Returns a
 * PORT_MENU_* request. */
int port_menu_frame(void);
/* The draw, from inside the present (video.c, render_gpu.c): nothing unless shown and a frame was built. The
 * software path draws over the game's texture before SDL_RenderPresent; the hardware path records a pass over the
 * swapchain texture into the present's command buffer, before its submit. */
void port_menu_render_renderer(struct SDL_Renderer *renderer);
void port_menu_render_gpu(struct SDL_GPUCommandBuffer *cb, struct SDL_GPUTexture *swapchain);
/* The menu's picture from the hardware renderer (<PREFIX>_PORT_MENU_SHOT), whose swapchain cannot be read back:
 * port_menu_shot_due says this present is the one to take it, and render_gpu.c then draws the picture and the menu
 * into a texture of the swapchain's format, blits that into the swapchain and reads it back; port_menu_shot_save
 * takes its pixels (4 bytes a pixel, w * 4 a row, B, G, R in memory when bgra, else R, G, B; the fourth ignored). */
int port_menu_shot_due(void);
void port_menu_shot_save(const void *pixels, int w, int h, int bgra);

#ifdef __cplusplus
}
#endif
