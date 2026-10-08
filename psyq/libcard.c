/* psyq/libcard.c: the memory card through the BIOS: LIBCARD's calls (InitCARD, StartCARD, _bu_init, _card_info,
 * _card_load, _card_clear, _card_format, ...) and the BIOS's file calls on the card devices "bu00:" (port 1) and
 * "bu10:" (port 2): open, read, write, lseek, close, firstfile, nextfile, erase, format. The cards are LIBMCRD's
 * (libmcrd.c, the card store: one image per slot, written back to its .mcd file after every change; the new-card flag
 * shared), so a game may use either library. Written from the games' use of the API (Digimon Digital Card Battle's
 * memcard.c) and psx-spx ("BIOS Memory Card Functions", "BIOS File Functions", "Memory Card Data Format").
 *
 * The file calls. A name is "<device><unit>:<file>": the device's letters, then its unit in hexadecimal (bu00 = port
 * 1, bu10 = port 2; the units of a multitap's other slots have no card here). Only "bu" is a device: any other name
 * fails (-1), as the retail BIOS has no "sim:" (the first game's SHOCKTST opens "sim:C:\..." and expects that), and
 * "cdrom:" stops the run (port_unimplemented: no game reads the disc that way yet). open's mode: FREAD 1, FWRITE 2,
 * FCREAT 0x200 with the file's block count in bits 16-31 (the file is created in the first free blocks, its data left
 * as it was; it fails when the name exists or the card is full), FASYNC 0x8000. A file descriptor is 2..15 (the BIOS's
 * sixteen file control blocks, the first two the console's). read and write move whole 128-byte frames at a position
 * that is a multiple of 128, inside the file (else -1), and advance the position; lseek takes SEEK_SET (0) and
 * SEEK_CUR (1) and returns the new position. firstfile/nextfile walk the directory for a name with '?' and '*' (the
 * search is the BIOS's, global: nextfile continues the last firstfile whatever entry it is given): the entry's name,
 * size in bytes, attr (the first block's allocation state, 0x51), head (its block, 1-15), next NULL.
 *
 * Asynchronous calls (a file opened with FASYNC, and _card_info, _card_load, _card_clear) return at once and
 * complete at the next vsync tick (libetc.c psyq_vsync_tick: psyq_card_vsync, after LIBSND's tick, before the root
 * counter's events), which delivers the result as an event: the card's file transfers and _card_info/_card_load on
 * the software class SwCARD, _card_clear on the hardware class HwCARD (what the game waits on after each), with
 * EvSpIOE (done), EvSpERROR, EvSpTIMOUT (no card) or EvSpNEW. One command at a time: another one while it runs is
 * refused (-1 for a file call, 0 for a _card_ call). A synchronous read or write completes inside the call and
 * delivers no event.
 *  - _card_info(chan): EvSpTIMOUT without a card, EvSpNEW while the card's new-card flag is set (from its insertion or
 *    the console's reset until a _card_clear or a write: the hardware's flag, which only a write clears), else EvSpIOE.
 *  - _card_load(chan) (reads the directory): EvSpTIMOUT without a card, EvSpNEW for a card that is not formatted, else
 *    EvSpIOE.
 *  - _card_clear(chan) (a write to the card that clears the flag): HwCARD EvSpTIMOUT without a card, else EvSpIOE.
 *  - _card_format(chan): synchronous, 1 (formatted, as PCSX-Redux formats a new card: libmcrd.c) or 0 (no card).
 * The timing (one tick) is a deterministic stand-in: the PS1's card takes a few milliseconds per 128-byte frame. */
#include <string.h>

#include "psyq_internal.h"
#include "psxstack/psyq_names.h" /* open, read, ... are the shim's psyq_api_*: the host libc keeps its own */
#include "psxstack/psyq/libapi.h"

#define CARD_FDS 16
#define CARD_FIRST_FD 2

enum { CARD_OP_NONE, CARD_OP_INFO, CARD_OP_LOAD, CARD_OP_CLEAR, CARD_OP_READ, CARD_OP_WRITE };

typedef struct PsyqCardFile {
    int used;
    int slot;
    int first;  /* the file's first block */
    s32 size;   /* bytes */
    s32 pos;
    s32 mode;   /* open's flags */
    char name[21];
} PsyqCardFile;

typedef struct PsyqCardOp {
    int kind;   /* CARD_OP_*; NONE: idle */
    s32 chan;
    int slot;   /* -1: a unit without a card */
    int fd;
    u8 *buf;    /* a transfer's buffer (game memory), its file offset and size */
    s32 pos, bytes;
} PsyqCardOp;

static PsyqCardFile psyq_card_files[CARD_FDS];
static PsyqCardOp psyq_card_op;
static u32 psyq_card_last_chan;
static int psyq_card_started;
static struct {
    int slot;          /* -1: no search */
    int next;          /* the next directory frame to look at */
    char pattern[21];
} psyq_card_search = { -1, 0, "" };

/* The slot of a channel (0x00 port 1, 0x10 port 2), -1 for any other. */
static int psyq_card_slot_of(s32 chan) {
    return chan == 0x00 ? 0 : chan == 0x10 ? 1 : -1;
}

/* "<device><unit>:<file>" -> 1 and the device's letters, its unit and the file's name; 0 when the name has no ':'. */
static int psyq_card_parse(const char *name, char dev[8], s32 *unit, const char **file) {
    const char *colon = strchr(name, ':');
    int n = 0;
    s32 u = 0;
    const char *p = name;

    if (colon == NULL) {
        return 0;
    }
    while (p < colon && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) && n < 7) {
        dev[n++] = (char)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
        p++;
    }
    dev[n] = '\0';
    for (; p < colon; p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10
              : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
        if (d < 0) {
            return 0;
        }
        u = u * 16 + d;
    }
    *unit = u;
    *file = colon + 1;
    return 1;
}

/* The formatted card of a "bu" name, or NULL (and the reason traced). `file` gets the part after the ':'. */
static u8 *psyq_card_named(const char *who, const char *name, int *slot, const char **file) {
    char dev[8];
    s32 unit;
    u8 *image;

    if (name == NULL || !psyq_card_parse(name, dev, &unit, file)) {
        PSYQ_TRACE("%s \"%s\": no device", who, name != NULL ? name : "(null)");
        return NULL;
    }
    if (strcmp(dev, "cdrom") == 0) {
        port_unimplemented("the BIOS's cdrom: device (open/firstfile on the disc)");
    }
    if (strcmp(dev, "bu") != 0) {
        PSYQ_TRACE("%s \"%s\": no device %s", who, name, dev);
        return NULL;
    }
    *slot = psyq_card_slot_of(unit);
    image = psyq_card_image(*slot);
    if (image == NULL || !psyq_card_formatted(image)) {
        PSYQ_TRACE("%s \"%s\": %s", who, name, image == NULL ? "no card" : "the card is not formatted");
        return NULL;
    }
    return image;
}

static PsyqCardFile *psyq_card_fd(s32 fd) {
    if (fd < CARD_FIRST_FD || fd >= CARD_FDS || !psyq_card_files[fd].used) {
        return NULL;
    }
    return &psyq_card_files[fd];
}

/* ---- The asynchronous commands */

/* Registers a command for the next tick: 1, or 0 while another one runs. */
static int psyq_card_start(int kind, s32 chan, int slot, int fd, u8 *buf, s32 pos, s32 bytes) {
    if (psyq_card_op.kind != CARD_OP_NONE) {
        PSYQ_TRACE("card: command %d refused, %d running", kind, psyq_card_op.kind);
        return 0;
    }
    psyq_card_op.kind = kind;
    psyq_card_op.chan = chan;
    psyq_card_op.slot = slot;
    psyq_card_op.fd = fd;
    psyq_card_op.buf = buf;
    psyq_card_op.pos = pos;
    psyq_card_op.bytes = bytes;
    return 1;
}

/* A transfer of whole frames inside the file: 0, or -1. */
static int psyq_card_io(int slot, int first, s32 size, s32 pos, u8 *buf, s32 bytes, int to_card) {
    u8 *image = psyq_card_image(slot);

    if (image == NULL || bytes <= 0 || pos < 0 || (pos | bytes) % PSYQ_CARD_FRAME != 0 || pos + bytes > size) {
        return -1;
    }
    if (psyq_card_transfer(image, first, pos, buf, bytes, to_card) != 0) {
        return -1;
    }
    if (to_card) {
        psyq_card_set_fresh(slot, 0); /* a write clears the card's new-card flag */
        psyq_card_written(slot);
    }
    return 0;
}

/* The vsync tick: the command registered before it completes and delivers its event. */
void psyq_card_vsync(void) {
    PsyqCardOp op = psyq_card_op;
    u8 *image = psyq_card_image(op.slot);
    u32 desc = SwCARD;
    s32 spec;

    if (op.kind == CARD_OP_NONE) {
        return;
    }
    psyq_card_op.kind = CARD_OP_NONE;
    psyq_card_last_chan = (u32)op.chan;
    switch (op.kind) {
    case CARD_OP_INFO:
        spec = image == NULL ? EvSpTIMOUT : psyq_card_fresh(op.slot) ? EvSpNEW : EvSpIOE;
        break;
    case CARD_OP_LOAD:
        spec = image == NULL ? EvSpTIMOUT : !psyq_card_formatted(image) ? EvSpNEW : EvSpIOE;
        break;
    case CARD_OP_CLEAR:
        desc = HwCARD;
        spec = image == NULL ? EvSpTIMOUT : EvSpIOE;
        if (image != NULL) {
            psyq_card_set_fresh(op.slot, 0);
        }
        break;
    default: {
        const PsyqCardFile *f = &psyq_card_files[op.fd];
        int r = psyq_card_io(op.slot, f->first, f->size, op.pos, op.buf, op.bytes, op.kind == CARD_OP_WRITE);
        spec = image == NULL ? EvSpTIMOUT : r == 0 ? EvSpIOE : EvSpERROR;
        break;
    }
    }
    PSYQ_TRACE("card: command %d on %02X done: %s %04X", op.kind, (u32)op.chan, desc == SwCARD ? "SwCARD" : "HwCARD",
               (unsigned)spec);
    psyq_api_deliver(desc, spec);
}

/* ---- LIBCARD */

void InitCARD(s32 val) {
    PSYQ_TRACE("InitCARD %d", val);
    psyq_card_op.kind = CARD_OP_NONE;
    psyq_card_started = 0;
}

s32 StartCARD(void) {
    PSYQ_TRACE("StartCARD");
    psyq_card_started = 1;
    return 1;
}

s32 StopCARD(void) {
    PSYQ_TRACE("StopCARD");
    psyq_card_started = 0;
    return 1;
}

void _bu_init(void) {
    PSYQ_TRACE("_bu_init");
}

s32 _card_info(s32 chan) {
    PSYQ_TRACE("_card_info %02X", (u32)chan);
    return psyq_card_start(CARD_OP_INFO, chan, psyq_card_slot_of(chan), 0, NULL, 0, 0);
}

s32 _card_load(s32 chan) {
    PSYQ_TRACE("_card_load %02X", (u32)chan);
    return psyq_card_start(CARD_OP_LOAD, chan, psyq_card_slot_of(chan), 0, NULL, 0, 0);
}

s32 _card_clear(s32 chan) {
    PSYQ_TRACE("_card_clear %02X", (u32)chan);
    return psyq_card_start(CARD_OP_CLEAR, chan, psyq_card_slot_of(chan), 0, NULL, 0, 0);
}

s32 _card_format(s32 chan) {
    int slot = psyq_card_slot_of(chan);
    u8 *image = psyq_card_image(slot);

    PSYQ_TRACE("_card_format %02X", (u32)chan);
    if (image == NULL) {
        return 0;
    }
    psyq_mcrd_format_image(image);
    psyq_card_set_fresh(slot, 0);
    psyq_card_written(slot);
    return 1;
}

/* 1 when no command runs (ready), 0x11 while one does (busy). */
s32 _card_status(s32 drv) {
    (void)drv;
    return psyq_card_op.kind == CARD_OP_NONE ? 1 : 0x11;
}

/* Waits for the running command (vsync ticks: its completion is one). 1. */
s32 _card_wait(s32 drv) {
    (void)drv;
    while (psyq_card_op.kind != CARD_OP_NONE) {
        psyq_vsync_tick();
    }
    return 1;
}

/* The channel of the last command that completed. */
u32 _card_chan(void) {
    return psyq_card_last_chan;
}

/* ---- The BIOS's file calls (named psyq_api_* at link time: psyq_names.h) */

s32 open(char *name, s32 mode) {
    const char *file;
    int slot = -1, first, fd;
    u8 *image = psyq_card_named("open", name, &slot, &file);
    PsyqCardFile *f;

    PSYQ_TRACE("open \"%s\" mode %X", name != NULL ? name : "(null)", (u32)mode);
    if (image == NULL || file[0] == '\0' || strlen(file) > 20) {
        return -1;
    }
    for (fd = CARD_FIRST_FD; fd < CARD_FDS && psyq_card_files[fd].used; fd++) {
    }
    if (fd == CARD_FDS) {
        PSYQ_TRACE("open \"%s\": no free file descriptor", name);
        return -1;
    }
    if (mode & FCREAT) {
        s32 r = psyq_card_create(image, file, (s32)((u32)mode >> 16));
        if (r != 0) {
            PSYQ_TRACE("open \"%s\": cannot create %d block(s) (%d)", name, (int)((u32)mode >> 16), r);
            return -1;
        }
        psyq_card_written(slot);
    }
    first = psyq_card_find(image, file);
    if (first == 0) {
        PSYQ_TRACE("open \"%s\": no such file", name);
        return -1;
    }
    f = &psyq_card_files[fd];
    memset(f, 0, sizeof(*f));
    f->used = 1;
    f->slot = slot;
    f->first = first;
    f->size = (s32)(image[first * PSYQ_CARD_FRAME + 4] | image[first * PSYQ_CARD_FRAME + 5] << 8 |
                    image[first * PSYQ_CARD_FRAME + 6] << 16 | (u32)image[first * PSYQ_CARD_FRAME + 7] << 24);
    f->mode = mode;
    strncpy(f->name, file, 20);
    PSYQ_TRACE("open \"%s\" = %d (block %d, %d bytes)", name, fd, first, f->size);
    return fd;
}

static s32 psyq_card_rw(const char *who, s32 fd, void *buf, s32 n, int to_card) {
    PsyqCardFile *f = psyq_card_fd(fd);
    s32 pos;

    PSYQ_TRACE("%s %d %u bytes 0x%X", who, fd, PSYQ_PTR(buf), (u32)n);
    if (f == NULL || n <= 0 || f->pos < 0 || (f->pos | n) % PSYQ_CARD_FRAME != 0 || f->pos + n > f->size) {
        PSYQ_TRACE("%s %d: refused (position 0x%X)", who, fd, f != NULL ? (u32)f->pos : 0u);
        return -1;
    }
    pos = f->pos;
    if (f->mode & FASYNC) {
        if (!psyq_card_start(to_card ? CARD_OP_WRITE : CARD_OP_READ, f->slot << 4, f->slot, fd, buf, pos, n)) {
            return -1;
        }
    } else if (psyq_card_io(f->slot, f->first, f->size, pos, buf, n, to_card) != 0) {
        return -1;
    }
    f->pos = pos + n;
    return n;
}

s32 read(s32 fd, void *buf, s32 n) {
    return psyq_card_rw("read", fd, buf, n, 0);
}

s32 write(s32 fd, void *buf, s32 n) {
    return psyq_card_rw("write", fd, buf, n, 1);
}

s32 lseek(s32 fd, s32 offset, s32 whence) {
    PsyqCardFile *f = psyq_card_fd(fd);
    s32 pos;

    PSYQ_TRACE("lseek %d 0x%X %d", fd, (u32)offset, whence);
    if (f == NULL || (whence != 0 && whence != 1)) {
        return -1;
    }
    pos = whence == 0 ? offset : f->pos + offset;
    if (pos < 0 || pos > f->size) {
        return -1;
    }
    f->pos = pos;
    return pos;
}

s32 close(s32 fd) {
    PsyqCardFile *f = psyq_card_fd(fd);

    PSYQ_TRACE("close %d", fd);
    if (f == NULL) {
        return -1;
    }
    f->used = 0;
    return fd;
}

/* The next directory entry of the search that matches, into `dir`; NULL at the end. */
static DIRENTRY *psyq_card_next_match(DIRENTRY *dir) {
    u8 *image = psyq_card_image(psyq_card_search.slot);

    while (image != NULL && psyq_card_search.next < PSYQ_CARD_BLOCKS) {
        int block = psyq_card_search.next++;
        char name[21];
        s32 size, state;

        if (psyq_card_dir_entry(image, block, name, &size, &state) && psyq_card_match(psyq_card_search.pattern, name)) {
            memset(dir, 0, sizeof(*dir));
            memcpy(dir->name, name, sizeof(dir->name));
            dir->attr = state;
            dir->size = size;
            dir->next = NULL;
            dir->head = block;
            return dir;
        }
    }
    psyq_card_search.slot = -1;
    return NULL;
}

DIRENTRY *firstfile(char *name, DIRENTRY *dir) {
    const char *file;
    int slot = -1;
    u8 *image = psyq_card_named("firstfile", name, &slot, &file);
    DIRENTRY *r;

    psyq_card_search.slot = -1;
    if (image == NULL) {
        return NULL;
    }
    psyq_card_search.slot = slot;
    psyq_card_search.next = 1;
    strncpy(psyq_card_search.pattern, file[0] != '\0' ? file : "*", 20);
    psyq_card_search.pattern[20] = '\0';
    r = psyq_card_next_match(dir);
    PSYQ_TRACE("firstfile \"%s\" = %s", name, r != NULL ? r->name : "none");
    return r;
}

DIRENTRY *nextfile(DIRENTRY *dir) {
    DIRENTRY *r = psyq_card_search.slot >= 0 ? psyq_card_next_match(dir) : NULL;

    PSYQ_TRACE("nextfile = %s", r != NULL ? r->name : "none");
    return r;
}

/* 1 = erased, 0 = no such file (or no card). */
s32 erase(char *name) {
    const char *file;
    int slot = -1, first;
    u8 *image = psyq_card_named("erase", name, &slot, &file);

    PSYQ_TRACE("erase \"%s\"", name != NULL ? name : "(null)");
    if (image == NULL || (first = psyq_card_find(image, file)) == 0) {
        return 0;
    }
    psyq_card_erase(image, first);
    psyq_card_set_fresh(slot, 0);
    psyq_card_written(slot);
    return 1;
}

/* "bu00:" or "bu10:": the card formatted (1), or 0 without one. */
s32 format(char *name) {
    char dev[8];
    s32 unit;
    const char *file;

    PSYQ_TRACE("format \"%s\"", name != NULL ? name : "(null)");
    if (name == NULL || !psyq_card_parse(name, dev, &unit, &file) || strcmp(dev, "bu") != 0) {
        return 0;
    }
    return _card_format(unit);
}

/* ---- The console's reset and the save state */

/* The console's reset (psyq.c psyq_reset): no file open, no command running, no search; the cards' new-card flags
 * are libmcrd.c's (psyq_mcrd_reset). */
void psyq_card_reset(void) {
    memset(psyq_card_files, 0, sizeof(psyq_card_files));
    memset(&psyq_card_op, 0, sizeof(psyq_card_op));
    psyq_card_last_chan = 0;
    psyq_card_started = 0;
    psyq_card_search.slot = -1;
    psyq_card_search.next = 0;
    memset(psyq_card_search.pattern, 0, sizeof(psyq_card_search.pattern));
}

/* A save state (psyq_internal.h): the open files, the command running (its buffer is game memory), the search. */
void psyq_card_state(PortState *s) {
    PORT_STATE_VAR(s, psyq_card_files);
    PORT_STATE_VAR(s, psyq_card_op);
    PORT_STATE_VAR(s, psyq_card_last_chan);
    PORT_STATE_VAR(s, psyq_card_started);
    PORT_STATE_VAR(s, psyq_card_search);
}
