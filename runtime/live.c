/* The settings applied live (docs/LAUNCHER.md "In-game menu"): the in-game menu wrote the settings file (through
 * the launcher's own code, so it is well formed) and asks the runtime to follow it without a restart. What follows:
 * the window's scale, fullscreen and scaling, the hardware renderer's filter and sub-pixel mode (video.c), audio.mute
 * (audio.c), the pad map and the hotkeys (input.c: the table starts over, so the port's own actions and every
 * enabled mod's bindings are registered again) and the mods (mods.c port_mods_apply). What waits for the next start,
 * and the menu says so: the disc, the renderer and its internal scale, the rate, the memory cards, the watchdog, the
 * data mods (texture packs) and a mod whose `applies` is "restart". A file that does not read is logged and nothing
 * changes (settings.c port_settings_reload). */
#include "port_harness.h"
#include "port_runtime.h"
#include "settings.h"

int port_live_apply(void) {
    const PortSettings *s = &port_settings;
    PortFilter filter;
    if (!port_settings_reload()) {
        return 0;
    }
    filter.kind = s->filter;
    filter.scanlines = s->crt_scanlines;
    filter.mask = s->crt_mask;
    filter.curvature = s->crt_curvature;
    port_video_apply(s->scale, s->fullscreen, s->scaling, &filter, s->subpixel);
    port_audio_set_user_mute(s->mute);
    port_input_settings(s->input);
    port_mods_apply(s->mods);
    port_log("settings: applied live (the disc, the renderer, the rate, the memory cards and the data mods at the next start)");
    return 1;
}
