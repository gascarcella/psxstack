/* The present's filters by name (port_harness.h "PortFilter"; docs/RUNTIME.md "The window"): `--filter
 * NAME[:KEY=V,...]` and video.filter. In every build: the headless build reads the same settings and options, and
 * only the hardware renderer (render_gpu_present.c) draws them. */
#include <stdio.h>
#include <string.h>

#include "port_harness.h"

const char *const port_filter_names[PORT_FILTER_COUNT] = { "none", "sharp" };

int port_filter_from_name(const char *name) {
    int i;
    for (i = 0; i < PORT_FILTER_COUNT; i++) {
        if (strcmp(name, port_filter_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

const char *port_filter_choices(int quoted) {
    static char text[2][160];
    char *out = text[quoted != 0];
    const char *q = quoted ? "\"" : "";
    int i;
    out[0] = '\0';
    for (i = 0; i < PORT_FILTER_COUNT; i++) {
        size_t n = strlen(out);
        snprintf(out + n, sizeof(text[0]) - n, "%s%s%s%s", i == 0 ? "" : i == PORT_FILTER_COUNT - 1 ? " or " : ", ", q,
                 port_filter_names[i], q);
    }
    return out;
}

int port_filter_parse(const char *spec, PortFilter *f, char *err, size_t err_size) {
    char name[32];
    const char *colon = strchr(spec, ':');
    size_t n = colon != NULL ? (size_t)(colon - spec) : strlen(spec);
    memset(f, 0, sizeof(*f));
    if (n >= sizeof(name)) {
        n = sizeof(name) - 1;
    }
    memcpy(name, spec, n);
    name[n] = '\0';
    f->kind = port_filter_from_name(name);
    if (f->kind < 0) {
        snprintf(err, err_size, "%s, not \"%s\"", port_filter_choices(0), name);
        return 0;
    }
    if (colon != NULL) {
        snprintf(err, err_size, "%s takes no parameters", name);
        return 0;
    }
    return 1;
}
