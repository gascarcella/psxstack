/* The present's filters by name (port_harness.h "PortFilter"; docs/RUNTIME.md "The window"): `--filter
 * NAME[:KEY=V,...]` and video.filter. In every build: the headless build reads the same settings and options, and
 * only the hardware renderer (render_gpu_present.c) draws them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_harness.h"

const char *const port_filter_names[PORT_FILTER_COUNT] = { "none", "sharp", "scanlines", "crt", "smooth" };

/* The parameters a filter takes (`--filter NAME:KEY=V`): a bit per entry of keys[]. */
static const char *const keys[] = { "scanlines", "mask", "curvature" };
static const unsigned takes[PORT_FILTER_COUNT] = { 0, 0, 1, 7, 0 };

static int *filter_param(PortFilter *f, int key) {
    return key == 0 ? &f->scanlines : key == 1 ? &f->mask : &f->curvature;
}

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

/* What filter `kind` takes, for a message: "scanlines" or "scanlines, mask, curvature". */
static void takes_text(int kind, char *out, size_t size) {
    size_t i;
    out[0] = '\0';
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (takes[kind] & (1u << i)) {
            size_t n = strlen(out);
            snprintf(out + n, size - n, "%s%s", n ? ", " : "", keys[i]);
        }
    }
}

int port_filter_parse(const char *spec, PortFilter *f, char *err, size_t err_size) {
    char name[32], list[64];
    const char *p = strchr(spec, ':');
    size_t n = p != NULL ? (size_t)(p - spec) : strlen(spec);
    int kind;
    if (n >= sizeof(name)) {
        n = sizeof(name) - 1;
    }
    memcpy(name, spec, n);
    name[n] = '\0';
    kind = port_filter_from_name(name);
    if (kind < 0) {
        snprintf(err, err_size, "%s, not \"%s\"", port_filter_choices(0), name);
        return 0;
    }
    takes_text(kind, list, sizeof(list));
    while (p != NULL) {
        char key[32], *end;
        const char *eq = strchr(p + 1, '='), *next = strchr(p + 1, ',');
        size_t i, k = eq != NULL ? (size_t)(eq - p - 1) : 0;
        long v;
        if (takes[kind] == 0) {
            snprintf(err, err_size, "%s takes no parameters", name);
            return 0;
        }
        if (eq == NULL || (next != NULL && next < eq) || k == 0 || k >= sizeof(key)) {
            snprintf(err, err_size, "%s: KEY=V,... (%s)", name, list);
            return 0;
        }
        memcpy(key, p + 1, k);
        key[k] = '\0';
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]) && strcmp(key, keys[i]) != 0; i++) {
        }
        if (i == sizeof(keys) / sizeof(keys[0]) || !(takes[kind] & (1u << i))) {
            snprintf(err, err_size, "%s takes %s, not %s", name, list, key);
            return 0;
        }
        v = strtol(eq + 1, &end, 10);
        if (end == eq + 1 || (*end != '\0' && *end != ',') || v < 0 || v > 100) {
            snprintf(err, err_size, "%s: %s from 0 to 100", name, key);
            return 0;
        }
        *filter_param(f, (int)i) = (int)v;
        p = *end == ',' ? end : NULL;
    }
    f->kind = kind;
    return 1;
}
