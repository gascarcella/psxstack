/* psyq/gte_shadow.c: the GTE's screen coordinates with their fractions (docs/PORT.md "Sub-pixel precision"), for the
 * hardware renderer above internal scale 1. The game sees the PS1's integer SX, SY, nothing else: this file keeps, beside
 * them, what gte.c cut them from, and finds it again when a primitive with those coordinates is drawn. It never
 * changes a value the game reads, so the command stream, the software picture and the frame hash are the same with it
 * on or off. Off (the default) it costs gte.c and gpu.c one branch each per vertex and the arena one per linked
 * primitive.
 *
 * The chain, as games build 3D primitives (the first consumer's battle: one RTPS per vertex into a per-mesh cache of
 * SXY words, then plain C copies of those words into the packets, then addPrim):
 *  1. gte.c's RTPS/RTPT computes OFX + IR * (H / SZ) in 16.16 fixed point and keeps its top half as SX, SY. The whole
 *     sum goes into a precise FIFO beside SXY0-2 (gte_shadow_rtp; a clamped value has none), with the vertex's SZ;
 *     a write to SXY0-2 or SXYP voids the slot.
 *  2. A store of SXY (swc2: tools/port_gen.py generates psyq_gte_swc2_, gte.c) records the precise value under the
 *     host address it wrote (the address table), and adds it to the current *batch*: a run of SXY stores to
 *     consecutive words, i.e. one mesh's vertex cache.
 *  3. The game's C copies the words into a packet; the shim cannot see C assignments. When the packet is linked into
 *     an ordering table (addPrim's setaddr goes through port_ptr_to_u32, runtime/arena.c), psyq_gte_shadow_link looks
 *     its vertex words up in the current batch by value: all found, each vertex word's address gets the value (a
 *     word two vertices of the batch share with different fractions, *ambiguous*, gets their mean: a function of the
 *     word, so a vertex shared by several faces lands at the same place in each, watertight); one not found, the
 *     packet is not this mesh's (a 2D primitive) and its vertex addresses are cleared.
 *  4. gpu.c, decoding a polygon, looks each vertex word up by the host address it was read from
 *     (psyq_gte_shadow_find). An entry counts only when its word is the word read and its integer part is the integer
 *     the GPU draws; the listener's GpuVertex then carries the fraction and SZ.
 * Entries live in two tables swapped every SHADOW_SWAP_VSYNCS vsyncs (an entry is found for one to two swap periods),
 * so a stale one cannot outlive the packet buffers it described.
 *
 * <PREFIX>_PORT_SUBPIXEL_LOG=path turns the shadow on from boot and writes a binary log for the first game's metric
 * (dw2003recomp tests/port/subpixel_jitter.py): 40-byte little-endian records {s32 frame; u32 n; s16 vx, vy, vz, sx,
 * sy, kind; f32 a, b, c, d, z}: kind 0 an RTPS vertex (n its place in the frame's sequence; the model-space vector;
 * the integer SX, SY; a, b the 16.16 sums / 65536; c, d the float projection OFX + H * MAC / MAC3; z MAC3 / 4096),
 * kind 1 a batch start (the store that starts it follows its own vertex's record), kind 2 a frame's polygon vertices
 * (n those drawn; a, b, c the precise, ambiguous and unmatched counts, as u32 bits). Frames
 * <PREFIX>_PORT_SUBPIXEL_LOG_FROM..TO only (the vsyncs since boot, the runtime's frame numbers). */
#include <stdlib.h>
#include <string.h>

#include "psyq_internal.h"

#define SHADOW_SWAP_VSYNCS 4
#define ADDR_BITS 15
#define ADDR_SIZE (1u << ADDR_BITS)
#define BATCH_BITS 12
#define BATCH_SIZE (1u << BATCH_BITS)

int psyq_gte_shadow_on;

/* gte.c's precise FIFO, beside SXY0-2. */
static GteShadowXY fifo[3];

typedef struct {
    const void *addr;
    u32 word;
    u32 gen;          /* the table's generation when written; another value: a free slot */
    s32 x, y;         /* 16.16 */
    u16 z;
    u8 valid;         /* 0: a cleared address (no precise value) */
    u8 ambiguous;
} AddrEntry;

static struct {
    AddrEntry slot[ADDR_SIZE];
    u32 gen;
    u32 used;
} tab[2];
static int cur;
static u32 next_gen = 1;
static unsigned vsyncs, since_swap;

typedef struct {
    u32 word;
    u32 id;           /* the batch it belongs to; another value: a free slot */
    s64 sx, sy;
    u32 sz, n;
    s32 x0, y0;
    u8 valid, ambiguous;
} BatchEntry;

static BatchEntry batch[BATCH_SIZE];
static u32 batch_id = 1;
static u32 batch_used;
static const u8 *batch_last;

static PsyqShadowStats stats;

/* The log (<PREFIX>_PORT_SUBPIXEL_LOG). */
typedef struct {
    s32 frame;
    u32 n;
    s16 vx, vy, vz, sx, sy, kind;
    float a, b, c, d, z;
} LogRecord;
static FILE *log_file;
static long log_from, log_to = -1;
static u32 log_ordinal;
static PsyqShadowStats log_frame;
static int log_decided;

static unsigned hash_ptr(const void *p) {
    uintptr_t v = (uintptr_t)p >> 2;
    return (unsigned)(((u64)v * 0x9E3779B97F4A7C15ull) >> (64 - ADDR_BITS));
}

static unsigned hash_word(u32 w) {
    return (unsigned)((w * 0x9E3779B1u) >> (32 - BATCH_BITS));
}

static int log_active(void) {
    return log_file != NULL && (long)vsyncs >= log_from && (log_to < 0 || (long)vsyncs <= log_to);
}

static void log_write(const LogRecord *r) {
    if (fwrite(r, sizeof(*r), 1, log_file) != 1) {
        fclose(log_file);
        log_file = NULL;
    }
}

static void log_decide(void) {
    const char *path = getenv(PSXSTACK_GAME_ENV_PREFIX "_PORT_SUBPIXEL_LOG");
    const char *from = getenv(PSXSTACK_GAME_ENV_PREFIX "_PORT_SUBPIXEL_LOG_FROM");
    const char *to = getenv(PSXSTACK_GAME_ENV_PREFIX "_PORT_SUBPIXEL_LOG_TO");

    log_decided = 1;
    if (path == NULL || *path == 0) {
        return;
    }
    log_file = fopen(path, "wb");
    if (log_file == NULL) {
        fprintf(stderr, "subpixel: cannot write %s\n", path);
        return;
    }
    log_from = from != NULL ? atol(from) : 0;
    log_to = to != NULL ? atol(to) : -1;
    psyq_gte_shadow_enable(1);
}

/* The frame's polygon counts into the log. */
static void log_frame_end(void) {
    LogRecord r;
    u32 bits[3];

    if (!log_active() || log_frame.drawn == 0) {
        return;
    }
    memset(&r, 0, sizeof(r));
    r.frame = (s32)vsyncs;
    r.n = (u32)log_frame.drawn;
    r.kind = 2;
    bits[0] = (u32)log_frame.precise;
    bits[1] = (u32)log_frame.ambiguous;
    bits[2] = (u32)(log_frame.drawn - log_frame.precise);
    memcpy(&r.a, &bits[0], 4);
    memcpy(&r.b, &bits[1], 4);
    memcpy(&r.c, &bits[2], 4);
    log_write(&r);
}

static void swap_tables(void) {
    cur ^= 1;
    tab[cur].gen = next_gen++;
    tab[cur].used = 0;
    since_swap = 0;
}

void psyq_gte_shadow_enable(int on) {
    if (on && !psyq_gte_shadow_on) {
        /* Nothing recorded while off is trusted. */
        memset(fifo, 0, sizeof(fifo));
        swap_tables();
        swap_tables();
        batch_last = NULL;
        batch_id++;
    }
    psyq_gte_shadow_on = on;
}

void psyq_gte_shadow_stats(PsyqShadowStats *out) {
    *out = stats;
}

void psyq_gte_shadow_tick(void) {
    if (!log_decided) {
        log_decide();
    }
    if (log_file != NULL) {
        log_frame_end();
        memset(&log_frame, 0, sizeof(log_frame));
        log_ordinal = 0;
    }
    vsyncs++;
    if (psyq_gte_shadow_on && ++since_swap >= SHADOW_SWAP_VSYNCS) {
        swap_tables();
    }
}

void psyq_gte_shadow_reset(void) {
    memset(fifo, 0, sizeof(fifo));
    if (psyq_gte_shadow_on) {
        swap_tables();
        swap_tables();
    }
    batch_last = NULL;
    batch_id++;
}

/* ---- gte.c's side ---- */

void gte_shadow_rtp(int valid, s64 sx, s64 sy, u32 sz) {
    fifo[0] = fifo[1];
    fifo[1] = fifo[2];
    fifo[2].x = (s32)sx;
    fifo[2].y = (s32)sy;
    fifo[2].z = (u16)sz;
    fifo[2].valid = (u8)valid;
}

void gte_shadow_write(int reg) {
    if (reg == 15) {
        fifo[0] = fifo[1];
        fifo[1] = fifo[2];
        fifo[2].valid = 0;
    } else if (reg >= 12 && reg <= 14) {
        fifo[reg - 12].valid = 0;
    }
}

void gte_shadow_clear(void) {
    memset(fifo, 0, sizeof(fifo));
}

void gte_shadow_log_rtp(const s32 v[3], s32 x, s32 y, s64 sx, s64 sy, const s64 mac[3], s32 ofx, s32 ofy, u32 h) {
    LogRecord r;
    double z;

    if (!log_active()) {
        return;
    }
    z = (double)mac[2] / 4096.0;
    memset(&r, 0, sizeof(r));
    r.frame = (s32)vsyncs;
    r.n = log_ordinal++;
    r.vx = (s16)v[0];
    r.vy = (s16)v[1];
    r.vz = (s16)v[2];
    r.sx = (s16)x;
    r.sy = (s16)y;
    r.a = (float)((double)sx / 65536.0);
    r.b = (float)((double)sy / 65536.0);
    r.c = z > 0 ? (float)((double)ofx / 65536.0 + (double)mac[0] / 4096.0 * h / z) : 0.0f;
    r.d = z > 0 ? (float)((double)ofy / 65536.0 + (double)mac[1] / 4096.0 * h / z) : 0.0f;
    r.z = (float)z;
    log_write(&r);
}

/* ---- the address table ---- */

static void addr_put(const void *addr, u32 word, const GteShadowXY *s, int ambiguous) {
    AddrEntry *t = tab[cur].slot;
    unsigned i = hash_ptr(addr), n;

    for (n = 0; n < ADDR_SIZE; n++, i = (i + 1) & (ADDR_SIZE - 1)) {
        if (t[i].gen != tab[cur].gen) {
            if (tab[cur].used >= ADDR_SIZE / 4 * 3) {
                stats.dropped++;
                return;
            }
            tab[cur].used++;
            break;
        }
        if (t[i].addr == addr) {
            break;
        }
    }
    t[i].addr = addr;
    t[i].gen = tab[cur].gen;
    t[i].word = word;
    t[i].valid = s != NULL && s->valid;
    t[i].ambiguous = (u8)ambiguous;
    t[i].x = s != NULL ? s->x : 0;
    t[i].y = s != NULL ? s->y : 0;
    t[i].z = s != NULL ? s->z : 0;
}

static const AddrEntry *addr_get(const void *addr) {
    int k;

    for (k = 0; k < 2; k++) {
        int ti = k == 0 ? cur : cur ^ 1;
        const AddrEntry *t = tab[ti].slot;
        unsigned i = hash_ptr(addr), n;

        for (n = 0; n < ADDR_SIZE; n++, i = (i + 1) & (ADDR_SIZE - 1)) {
            if (t[i].gen != tab[ti].gen) {
                break;
            }
            if (t[i].addr == addr) {
                return &t[i];
            }
        }
    }
    return NULL;
}

/* ---- the batch ---- */

static void batch_add(u32 word, const GteShadowXY *s) {
    unsigned i = hash_word(word), n;

    for (n = 0; n < BATCH_SIZE; n++, i = (i + 1) & (BATCH_SIZE - 1)) {
        BatchEntry *e = &batch[i];
        if (e->id != batch_id) {
            if (batch_used >= BATCH_SIZE / 4 * 3) {
                stats.dropped++;
                return;
            }
            batch_used++;
            e->id = batch_id;
            e->word = word;
            e->sx = e->sy = 0;
            e->sz = e->n = 0;
            e->x0 = s->x;
            e->y0 = s->y;
            e->valid = 1;
            e->ambiguous = 0;
        } else if (e->word != word) {
            continue;
        }
        if (!s->valid) {
            e->valid = 0;
        } else if (s->x != e->x0 || s->y != e->y0) {
            e->ambiguous = 1;
        }
        e->sx += s->x;
        e->sy += s->y;
        e->sz += s->z;
        e->n++;
        return;
    }
}

static const BatchEntry *batch_get(u32 word) {
    unsigned i = hash_word(word), n;

    for (n = 0; n < BATCH_SIZE; n++, i = (i + 1) & (BATCH_SIZE - 1)) {
        const BatchEntry *e = &batch[i];
        if (e->id != batch_id) {
            return NULL;
        }
        if (e->word == word) {
            return e;
        }
    }
    return NULL;
}

/* gte.c's swc2 of SXY0-2 or SXYP (the value v) to p: the address table and the batch. */
void gte_shadow_store(void *p, u32 v, int reg) {
    const GteShadowXY *s = &fifo[reg == 15 ? 2 : reg - 12];

    if ((const u8 *)p != batch_last + 4) {
        batch_id++;
        if (batch_id == 0) {
            memset(batch, 0, sizeof(batch));
            batch_id = 1;
        }
        batch_used = 0;
        if (log_active()) {
            LogRecord r;
            memset(&r, 0, sizeof(r));
            r.frame = (s32)vsyncs;
            r.kind = 1;
            log_write(&r);
        }
    }
    batch_last = p;
    batch_add(v, s);
    addr_put(p, v, s, 0);
}

/* A primitive linked into an ordering table (runtime/arena.c, port_ptr_to_u32): a polygon's vertex words resolved
 * against the current batch. Reads the packet's words (at most 13 after its tag). */
void psyq_gte_shadow_link(const void *packet) {
    const u8 *b = packet;
    const u32 *w = (const u32 *)(b + 4);
    u32 code = b[7];
    int gouraud, textured, nverts, i, k = 0;
    const u32 *at[4];
    const BatchEntry *e[4];
    int found = 1;

    if ((code & 0xE0) != 0x20) {
        return;
    }
    gouraud = (code & 0x10) != 0;
    textured = (code & 0x04) != 0;
    nverts = (code & 0x08) ? 4 : 3;
    for (i = 0; i < nverts; i++) {
        if (i == 0 || gouraud) {
            k++;
        }
        at[i] = &w[k++];
        if (textured) {
            k++;
        }
        e[i] = batch_get(*at[i]);
        found = found && e[i] != NULL;
    }
    for (i = 0; i < nverts; i++) {
        if (found) {
            GteShadowXY s;
            s.valid = e[i]->valid;
            s.x = (s32)(e[i]->sx / (s64)e[i]->n);
            s.y = (s32)(e[i]->sy / (s64)e[i]->n);
            s.z = (u16)(e[i]->sz / e[i]->n);
            /* The mean stays within the integer pixel the candidates share (s64 division truncates toward zero:
             * a negative mean is floored back into it). */
            if (s.x >> 16 != e[i]->x0 >> 16) {
                s.x = e[i]->x0 & ~0xFFFF;
            }
            if (s.y >> 16 != e[i]->y0 >> 16) {
                s.y = e[i]->y0 & ~0xFFFF;
            }
            addr_put(at[i], *at[i], &s, e[i]->ambiguous);
        } else {
            addr_put(at[i], *at[i], NULL, 0);
        }
    }
}

/* gpu.c: the precise vertex of the polygon vertex word `word` read from `src`: 1 and the fraction (0..65535 in 1/65536
 * pixel) and SZ when the shadow has one whose word and integer part agree, else 0. */
int psyq_gte_shadow_find(const void *src, u32 word, int *fx, int *fy, int *z) {
    const AddrEntry *e;
    s32 x = (s32)(word << 21) >> 21, y = (s32)(word << 5) >> 21;
    int hit;

    stats.drawn++;
    e = src != NULL ? addr_get(src) : NULL;
    hit = e != NULL && e->valid && e->word == word && e->x >> 16 == x && e->y >> 16 == y;
    if (log_file != NULL) {
        log_frame.drawn++;
        log_frame.precise += hit;
        log_frame.ambiguous += hit && e->ambiguous;
    }
    if (!hit) {
        return 0;
    }
    stats.precise++;
    stats.ambiguous += e->ambiguous;
    *fx = e->x & 0xFFFF;
    *fy = e->y & 0xFFFF;
    *z = e->z;
    return 1;
}
