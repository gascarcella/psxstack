/* Texture packs (render_gpu_textures.h; docs/RUNTIME.md "Texture packs"; the first game's issue #70): replacements for
 * the textures the game samples, found by key (render_gpu_textures.c) and drawn by the rasteriser
 * (shaders/raster.frag.hlsl's F_REPLACED path).
 *
 * A pack is a data mod: DIR/mod.json ({"schema": 1, "id", "name", "kind": "data", "textures": {"dir", "filter"}}) and
 * PNGs anywhere under its textures directory (default "textures"), each named by the key it replaces, as the dump names
 * them: <image>-<clut>-<4|8>bpp-<w>x<h>.png, <image>-15bpp-<w>x<h>.png, or with "@<u>,<v>,<uw>x<vh>" before ".png"
 * a sub-rectangle of that image (in its texels): an atlas's one sprite. Directory names do not matter. A file is any
 * size; it is stretched over the whole image or the sub-rectangle. A unit takes the first pack's file whose rectangle
 * holds all its texels, a sub-rectangle's before the whole image's; without one it samples the VRAM as before.
 *
 * Files are indexed at the start (names only) and decoded on first use, then uploaded with their mipmaps from a
 * command buffer of their own, submitted before the frame's (the decode's time is logged when it is long: a hitch).
 * Decoded textures are kept up to a budget; past it, those unused for longest are released after a frame. */
#ifdef PSXSTACK_SDL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "json.h"
#include "port_runtime.h"
#include "render_gpu_textures.h"

#define MAX_PACKS 32
#define BUDGET (1024ull << 20) /* bytes of decoded textures kept (mipmaps included) */
#define SLOW_NS 4000000ll      /* a decode this long is logged */

typedef struct {
    char *path;
    int pack;                  /* its priority: the index of its pack */
    int sub, su, sv, sw, sh;   /* a sub-rectangle file: its rectangle in the image's texels */
    u64 image, clut;
    int depth, w, h;
    int next;                  /* the next file with the same key, -1 */
    SDL_GPUTexture *texture;
    int failed;
    u64 bytes;
    u32 used;                  /* the frame it was last used in */
} PackFile;

static struct {
    int npacks;
    struct {
        char id[64];
        int linear;
    } packs[MAX_PACKS];
    PackFile *files;
    int nfiles, capfiles;
    int *slots;                /* open addressing by key: the first file's index + 1, 0 empty */
    int capslots;
    u64 bytes;
    u32 frame;
    int uploads, slow;
} p;

static size_t pack_key_hash(u64 image, u64 clut, int depth, int w, int h) {
    u64 v = image * 0x9E3779B97F4A7C15ull ^ clut ^ (u64)depth << 56 ^ (u64)w << 32 ^ (u64)h;
    return (size_t)(v ^ v >> 29);
}

static int same_key(const PackFile *f, u64 image, u64 clut, int depth, int w, int h) {
    return f->image == image && f->clut == clut && f->depth == depth && f->w == w && f->h == h;
}

/* The slot of a key: its first file's index + 1, or the empty slot where it would go. */
static int *slot_of(u64 image, u64 clut, int depth, int w, int h) {
    size_t i, mask = (size_t)p.capslots - 1;
    for (i = pack_key_hash(image, clut, depth, w, h) & mask; p.slots[i] != 0; i = (i + 1) & mask) {
        if (same_key(&p.files[p.slots[i] - 1], image, clut, depth, w, h)) {
            break;
        }
    }
    return &p.slots[i];
}

static void slots_grow(void) {
    int *old = p.slots, cap = p.capslots, i;
    p.capslots = p.capslots ? p.capslots * 2 : 4096;
    p.slots = calloc((size_t)p.capslots, sizeof(int));
    if (p.slots == NULL) {
        port_fatal("texture packs: out of memory (%d keys)", p.capslots);
    }
    for (i = 0; i < cap; i++) {
        if (old[i] != 0) {
            const PackFile *f = &p.files[old[i] - 1];
            *slot_of(f->image, f->clut, f->depth, f->w, f->h) = old[i];
        }
    }
    free(old);
}

/* 16 hex digits at s into *v; the characters read, 0 when they are not. */
static int hex16(const char *s, u64 *v) {
    int i;
    *v = 0;
    for (i = 0; i < 16; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) {
            return 0;
        }
        *v = *v << 4 | (u64)d;
    }
    return 16;
}

/* A file name as a key (and a sub-rectangle); 0 when it is not one. */
static int parse_name(const char *name, PackFile *f) {
    const char *s = name;
    int n = 0, depth;
    if (!hex16(s, &f->image) || s[16] != '-') {
        return 0;
    }
    s += 17;
    f->clut = 0;
    if (strncmp(s, "15bpp-", 6) == 0) {
        depth = 15;
        s += 6;
    } else {
        if (!hex16(s, &f->clut) || s[16] != '-' || (s[17] != '4' && s[17] != '8') || strncmp(s + 18, "bpp-", 4) != 0) {
            return 0;
        }
        depth = s[17] - '0';
        s += 22;
    }
    f->depth = depth;
    if (sscanf(s, "%dx%d%n", &f->w, &f->h, &n) != 2 || f->w <= 0 || f->h <= 0) {
        return 0;
    }
    s += n;
    f->sub = 0;
    if (*s == '@') {
        if (sscanf(s + 1, "%d,%d,%dx%d%n", &f->su, &f->sv, &f->sw, &f->sh, &n) != 4 || f->su < 0 || f->sv < 0 ||
            f->sw <= 0 || f->sh <= 0 || f->su + f->sw > f->w || f->sv + f->sh > f->h) {
            return 0;
        }
        f->sub = 1;
        s += 1 + n;
    } else {
        f->su = f->sv = 0;
        f->sw = f->w;
        f->sh = f->h;
    }
    return SDL_strcasecmp(s, ".png") == 0;
}

static char *read_text(const char *path, size_t *size) {
    FILE *fp = fopen(path, "rb");
    long n;
    char *text;
    if (fp == NULL) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    text = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (text != NULL && fread(text, 1, (size_t)n, fp) != (size_t)n) {
        free(text);
        text = NULL;
    }
    fclose(fp);
    *size = (size_t)n;
    return text;
}

int render_gpu_tex_pack_add(const char *dir) {
    char path[1200], err[256], **names;
    const char *tdir = "textures";
    size_t size;
    char *text;
    PortJson *root;
    const PortJson *v, *tex;
    int linear = 1, count = 0, i, files = 0, skipped = 0;

    if (p.npacks == MAX_PACKS) {
        port_log("texture packs: at most %d; %s ignored", MAX_PACKS, dir);
        return 0;
    }
    snprintf(path, sizeof(path), "%s/mod.json", dir);
    text = read_text(path, &size);
    if (text == NULL) {
        port_log("texture packs: %s: no mod.json; ignored", dir);
        return 0;
    }
    root = port_json_parse(text, size, err, sizeof(err));
    free(text);
    if (root == NULL) {
        port_log("texture packs: %s: %s; ignored", path, err);
        return 0;
    }
    v = port_json_get(root, "schema");
    tex = port_json_get(root, "textures");
    if (v == NULL || v->type != PORT_JSON_NUMBER || v->number != 1 || (v = port_json_get(root, "kind")) == NULL ||
        v->type != PORT_JSON_STRING || strcmp(v->string, "data") != 0 || tex == NULL || tex->type != PORT_JSON_OBJECT) {
        port_log("texture packs: %s: not schema 1, kind \"data\" with \"textures\"; ignored", path);
        port_json_free(root);
        return 0;
    }
    v = port_json_get(root, "id");
    snprintf(p.packs[p.npacks].id, sizeof(p.packs[0].id), "%s",
             v != NULL && v->type == PORT_JSON_STRING ? v->string : dir);
    if ((v = port_json_get(tex, "dir")) != NULL && v->type == PORT_JSON_STRING) {
        tdir = v->string;
    }
    if ((v = port_json_get(tex, "filter")) != NULL) {
        if (v->type != PORT_JSON_STRING || (strcmp(v->string, "linear") != 0 && strcmp(v->string, "nearest") != 0)) {
            port_log("texture packs: %s: textures.filter is \"linear\" or \"nearest\"; linear", path);
        } else {
            linear = v->string[0] == 'l';
        }
    }
    p.packs[p.npacks].linear = linear;
    snprintf(path, sizeof(path), "%s/%s", dir, tdir);
    port_json_free(root);
    names = SDL_GlobDirectory(path, NULL, 0, &count);
    for (i = 0; names != NULL && i < count; i++) {
        PackFile f;
        const char *base = strrchr(names[i], '/');
        int *slot;
        size_t len = strlen(names[i]);
        base = base != NULL ? base + 1 : names[i];
        if (len < 4 || SDL_strcasecmp(names[i] + len - 4, ".png") != 0) {
            continue;
        }
        memset(&f, 0, sizeof(f));
        if (!parse_name(base, &f)) {
            skipped++;
            continue;
        }
        f.path = malloc(strlen(path) + len + 2);
        if (f.path == NULL) {
            port_fatal("texture packs: out of memory (%d files)", p.nfiles);
        }
        sprintf(f.path, "%s/%s", path, names[i]);
        f.pack = p.npacks;
        f.next = -1;
        if (p.nfiles == p.capfiles) {
            int cap = p.capfiles ? p.capfiles * 2 : 1024;
            PackFile *nf = realloc(p.files, (size_t)cap * sizeof(*nf));
            if (nf == NULL) {
                port_fatal("texture packs: out of memory (%d files)", cap);
            }
            p.files = nf;
            p.capfiles = cap;
        }
        if ((p.nfiles + 1) * 2 > p.capslots) {
            slots_grow();
        }
        p.files[p.nfiles] = f;
        slot = slot_of(f.image, f.clut, f.depth, f.w, f.h);
        if (*slot == 0) {
            *slot = p.nfiles + 1;
        } else {
            int j = *slot - 1;
            while (p.files[j].next >= 0) {
                j = p.files[j].next;
            }
            p.files[j].next = p.nfiles;
        }
        p.nfiles++;
        files++;
    }
    if (names == NULL) {
        port_log("texture packs: %s: %s", path, SDL_GetError());
    }
    SDL_free(names);
    port_log("texture packs: %s (%s): %d textures%s%s%s", p.packs[p.npacks].id, dir, files, linear ? "" : ", nearest",
             skipped ? ", other PNGs ignored: " : "", skipped ? "names that are not keys" : "");
    p.npacks++;
    render_gpu_tex_track();
    return 1;
}

int render_gpu_tex_packs(void) {
    return p.npacks;
}

/* The file decoded and uploaded with its mipmaps; 0 (logged once) when it cannot be. */
static int pack_upload(SDL_GPUDevice *device, PackFile *f) {
    long long t0 = SDL_GetTicksNS();
    SDL_Surface *s = SDL_LoadPNG(f->path), *c;
    SDL_GPUTextureCreateInfo ti;
    SDL_GPUTransferBufferCreateInfo bi;
    SDL_GPUTransferBuffer *tb;
    SDL_GPUCommandBuffer *cb;
    SDL_GPUCopyPass *copy;
    SDL_GPUTextureTransferInfo src;
    SDL_GPUTextureRegion dst;
    Uint8 *map;
    int levels = 1, y, side;

    f->failed = 1;
    if (s == NULL) {
        port_log("texture packs: %s: %s", f->path, SDL_GetError());
        return 0;
    }
    c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888); /* R, G, B, A in memory: R8G8B8A8 */
    SDL_DestroySurface(s);
    if (c == NULL) {
        port_log("texture packs: %s: %s", f->path, SDL_GetError());
        return 0;
    }
    for (side = SDL_max(c->w, c->h); side > 1; side >>= 1) {
        levels++;
    }
    memset(&ti, 0, sizeof(ti));
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; /* the mipmaps are rendered */
    ti.width = (Uint32)c->w;
    ti.height = (Uint32)c->h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = (Uint32)levels;
    f->texture = SDL_CreateGPUTexture(device, &ti);
    memset(&bi, 0, sizeof(bi));
    bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    bi.size = (Uint32)(c->w * c->h * 4);
    tb = f->texture != NULL ? SDL_CreateGPUTransferBuffer(device, &bi) : NULL;
    map = tb != NULL ? SDL_MapGPUTransferBuffer(device, tb, false) : NULL;
    cb = map != NULL ? SDL_AcquireGPUCommandBuffer(device) : NULL;
    if (cb == NULL) {
        port_log("texture packs: %s (%dx%d): %s", f->path, c->w, c->h, SDL_GetError());
        if (map != NULL) {
            SDL_UnmapGPUTransferBuffer(device, tb);
        }
        if (tb != NULL) {
            SDL_ReleaseGPUTransferBuffer(device, tb);
        }
        if (f->texture != NULL) {
            SDL_ReleaseGPUTexture(device, f->texture);
            f->texture = NULL;
        }
        SDL_DestroySurface(c);
        return 0;
    }
    for (y = 0; y < c->h; y++) {
        memcpy(map + (size_t)y * (size_t)c->w * 4, (Uint8 *)c->pixels + (size_t)y * (size_t)c->pitch, (size_t)c->w * 4);
    }
    SDL_UnmapGPUTransferBuffer(device, tb);
    copy = SDL_BeginGPUCopyPass(cb);
    memset(&src, 0, sizeof(src));
    src.transfer_buffer = tb;
    src.pixels_per_row = (Uint32)c->w;
    src.rows_per_layer = (Uint32)c->h;
    memset(&dst, 0, sizeof(dst));
    dst.texture = f->texture;
    dst.w = (Uint32)c->w;
    dst.h = (Uint32)c->h;
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    if (levels > 1) {
        SDL_GenerateMipmapsForGPUTexture(cb, f->texture);
    }
    SDL_SubmitGPUCommandBuffer(cb); /* before the frame's units, which are submitted later on the same device */
    SDL_ReleaseGPUTransferBuffer(device, tb);
    f->bytes = (u64)c->w * (u64)c->h * 4 * 4 / 3;
    p.bytes += f->bytes;
    p.uploads++;
    SDL_DestroySurface(c);
    f->failed = 0;
    if (SDL_GetTicksNS() - t0 > SLOW_NS) {
        p.slow++;
        port_log("texture packs: %s decoded in %.1f ms", f->path, (double)(SDL_GetTicksNS() - t0) / 1e6);
    }
    return 1;
}

int render_gpu_tex_replacement(SDL_GPUDevice *device, const GpuEvent *ev, RenderTexReplacement *out) {
    RenderTexKey k;
    int range[4], lu0, lu1, lv0, lv1, j, best = -1;
    int *slot;

    if (p.npacks == 0 || !render_gpu_tex_unit_key(ev, &k, range)) {
        return 0;
    }
    if (ev->u_and != 0xFF || ev->u_or != 0 || ev->v_and != 0xFF || ev->v_or != 0) {
        return 0; /* a texture window repeats a part of the page: not a whole image's coordinates */
    }
    slot = slot_of(k.image, k.clut, k.depth, k.w, k.h);
    if (*slot == 0) {
        return 0;
    }
    lu0 = range[0] - k.u0, lu1 = range[1] - k.u0, lv0 = range[2] - k.v0, lv1 = range[3] - k.v0;
    for (j = *slot - 1; j >= 0; j = p.files[j].next) {
        const PackFile *f = &p.files[j];
        if (f->failed || lu0 < f->su || lv0 < f->sv || lu1 >= f->su + f->sw || lv1 >= f->sv + f->sh) {
            continue;
        }
        if (best < 0 || f->pack < p.files[best].pack || (f->pack == p.files[best].pack && f->sub && !p.files[best].sub)) {
            best = j;
        }
    }
    if (best < 0 || (p.files[best].texture == NULL && !pack_upload(device, &p.files[best]))) {
        return 0;
    }
    p.files[best].used = p.frame;
    out->texture = p.files[best].texture;
    out->linear = p.packs[p.files[best].pack].linear;
    out->u0 = k.u0 + p.files[best].su;
    out->v0 = k.v0 + p.files[best].sv;
    out->w = p.files[best].sw;
    out->h = p.files[best].sh;
    return 1;
}

void render_gpu_tex_packs_frame(SDL_GPUDevice *device) {
    p.frame++;
    while (p.bytes > BUDGET) {
        int i, oldest = -1;
        for (i = 0; i < p.nfiles; i++) {
            if (p.files[i].texture != NULL && p.files[i].used != p.frame - 1 &&
                (oldest < 0 || p.files[i].used < p.files[oldest].used)) {
                oldest = i;
            }
        }
        if (oldest < 0) {
            break; /* everything kept was used in the last frame */
        }
        SDL_ReleaseGPUTexture(device, p.files[oldest].texture); /* SDL keeps it until the GPU is done with it */
        p.files[oldest].texture = NULL;
        p.bytes -= p.files[oldest].bytes;
    }
}

void render_gpu_tex_packs_release(SDL_GPUDevice *device) {
    int i;
    for (i = 0; i < p.nfiles; i++) {
        if (p.files[i].texture != NULL) {
            SDL_ReleaseGPUTexture(device, p.files[i].texture);
            p.files[i].texture = NULL;
        }
    }
    if (p.uploads > 0) {
        port_log("texture packs: %d textures uploaded (%d slow decodes)", p.uploads, p.slow);
    }
    p.bytes = 0;
}
#endif
