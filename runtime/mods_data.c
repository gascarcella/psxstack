/* The data mods (docs/LAUNCHER.md "Data mods"): mods made of files, not code, found in the directory `--mods-dir DIR`
 * names (the launcher passes its settings directory's mods/). A data mod is DIR/<mod id>/mod.json with "kind": "data"
 * (schema 1, its id the directory's name, requires_port at most PSXSTACK_API); the only kind of data so far is
 * "textures", a texture pack (docs/RUNTIME.md "Texture packs"). The settings switch it on as they switch a built-in
 * mod on (`mods.<mod id>.enabled`), and `mod_order` lists data mods by priority: the first wins where two replace the
 * same thing; data mods it does not name come after, by id. Off under --script unless --script-mods, as every mod.
 * Only the SDL build has them (texture packs are drawn by the hardware renderer); the headless build refuses
 * --mods-dir. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "port_harness.h"
#include "port_runtime.h"
#include "psxstack/game.h"
#include "settings.h"

#ifdef PSXSTACK_SDL
#include <SDL3/SDL.h>

#define MAX_DATA_MODS 64

typedef struct {
    char id[64];
    char *dir;
    int textures;           /* it has a "textures" object: a texture pack */
    int enabled;
    int rank;               /* its place in mod_order, or MAX_DATA_MODS + the id's order */
} DataMod;

static DataMod data_mods[MAX_DATA_MODS];
static int data_mod_count;

static SDL_EnumerationResult SDLCALL data_mod_found(void *userdata, const char *dirname, const char *fname) {
    char path[1200], err[256];
    FILE *f;
    long size;
    char *text;
    PortJson *root;
    const PortJson *v;
    const char *why = NULL;
    DataMod *m;
    (void)userdata;

    if (data_mod_count == MAX_DATA_MODS) {
        port_log("mods: more than %d data mods in %s; %s and later ones ignored", MAX_DATA_MODS, dirname, fname);
        return SDL_ENUM_SUCCESS;
    }
    snprintf(path, sizeof(path), "%s%s/mod.json", dirname, fname);
    if ((f = fopen(path, "rb")) == NULL) {
        return SDL_ENUM_CONTINUE; /* not a mod's directory */
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = size >= 0 ? malloc((size_t)size + 1) : NULL;
    if (text == NULL || fread(text, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(text);
        port_log("mods: %s: unreadable; ignored", path);
        return SDL_ENUM_CONTINUE;
    }
    fclose(f);
    root = port_json_parse(text, (size_t)size, err, sizeof(err));
    free(text);
    if (root == NULL) {
        port_log("mods: %s: %s; ignored", path, err);
        return SDL_ENUM_CONTINUE;
    }
    if ((v = port_json_get(root, "schema")) == NULL || v->type != PORT_JSON_NUMBER || v->number != 1) {
        why = "not schema 1";
    } else if ((v = port_json_get(root, "id")) == NULL || v->type != PORT_JSON_STRING || strcmp(v->string, fname) != 0) {
        why = "its id is not its directory's name";
    } else if (strlen(fname) >= sizeof(data_mods[0].id)) {
        why = "its id is too long";
    } else if ((v = port_json_get(root, "kind")) == NULL || v->type != PORT_JSON_STRING || strcmp(v->string, "data")) {
        why = "not a data mod (a built-in mod comes with the game)";
    } else if ((v = port_json_get(root, "requires_port")) != NULL &&
               (v->type != PORT_JSON_NUMBER || v->number > PSXSTACK_API)) {
        why = "it needs a newer game (requires_port)";
    }
    if (why != NULL) {
        port_log("mods: %s: %s; ignored", path, why);
        port_json_free(root);
        return SDL_ENUM_CONTINUE;
    }
    m = &data_mods[data_mod_count++];
    snprintf(m->id, sizeof(m->id), "%s", fname);
    m->dir = malloc(strlen(dirname) + strlen(fname) + 1);
    if (m->dir == NULL) {
        port_fatal("mods: out of memory");
    }
    sprintf(m->dir, "%s%s", dirname, fname);
    v = port_json_get(root, "textures");
    m->textures = v != NULL && v->type == PORT_JSON_OBJECT;
    port_json_free(root);
    return SDL_ENUM_CONTINUE;
}

static int data_mod_compare(const void *a, const void *b) {
    const DataMod *x = a, *y = b;
    return x->rank != y->rank ? (x->rank < y->rank ? -1 : 1) : strcmp(x->id, y->id);
}

int port_mods_data_scan(const char *dir) {
    if (!SDL_EnumerateDirectory(dir, data_mod_found, NULL)) {
        port_log("mods: %s: %s (no data mods)", dir, SDL_GetError());
        return 0;
    }
    port_log("mods: %s: %d data mod%s", dir, data_mod_count, data_mod_count == 1 ? "" : "s");
    return 1;
}

int port_mods_data_known(const char *id) {
    int i;
    for (i = 0; i < data_mod_count && strcmp(data_mods[i].id, id) != 0; i++) {
    }
    return i < data_mod_count;
}

void port_mods_data_settings(const PortJson *mods, const PortJson *order) {
    int i;
    size_t k;
    for (i = 0; i < data_mod_count; i++) {
        DataMod *m = &data_mods[i];
        const PortJson *s = port_json_get(mods, m->id), *on = port_json_get(s, "enabled");
        char where[128];
        snprintf(where, sizeof(where), "mods.%.63s", m->id); /* id is char[64]: gcc 13 -O3 cannot see the bound */
        if (s != NULL && s->type != PORT_JSON_OBJECT) {
            port_settings_fail(where, "an object ({ \"enabled\": ... })");
        }
        if (on != NULL && on->type != PORT_JSON_BOOL) {
            snprintf(where, sizeof(where), "mods.%.63s.enabled", m->id);
            port_settings_fail(where, "true or false");
        }
        m->enabled = on != NULL && on->boolean;
        m->rank = MAX_DATA_MODS;
        for (k = 0; order != NULL && k < order->count; k++) {
            if (strcmp(order->items[k].string, m->id) == 0) {
                m->rank = (int)k;
                break;
            }
        }
    }
    qsort(data_mods, (size_t)data_mod_count, sizeof(DataMod), data_mod_compare);
}

void port_mods_data_start(int active) {
    int i;
    for (i = 0; i < data_mod_count; i++) {
        const DataMod *m = &data_mods[i];
        if (!m->enabled || !active) {
            continue;
        }
        if (!m->textures) {
            port_log("mods: %s: nothing this game can use (only \"textures\" data mods so far); ignored", m->id);
        } else if (!port_video_texture_pack(m->dir)) {
            port_log("mods: %s: not loaded", m->id);
        }
    }
}
#else
int port_mods_data_scan(const char *dir) {
    (void)dir;
    return 0;
}

int port_mods_data_known(const char *id) {
    (void)id;
    return 0;
}

void port_mods_data_settings(const PortJson *mods, const PortJson *order) {
    (void)mods;
    (void)order;
}

void port_mods_data_start(int active) {
    (void)active;
}
#endif
