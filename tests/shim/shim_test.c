/* tests/shim/shim_test.c: the Psy-Q shim's deterministic pieces, on their own (no runtime, no game, no disc; built and
 * run by tests/shim_test.py): LIBAPI's events and root counter 3 on the vsync tick, LIBCARD and the BIOS's file calls
 * on a fresh memory card image (the same card LIBMCRD sees), LIBCD's CdSearchFile and CdRead over a small ISO 9660
 * image built here, LIBSPU's attributes and LIBSND's SEQ and own tick. One line per check, "ok" or "FAIL"; exit 1 on
 * any failure. The runtime's few symbols the shim needs are stubbed below (an SPU register file, the tag window). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psyq_internal.h"
#include "libsnd_internal.h"
#include "psxstack/psyq_names.h" /* the BIOS's open, read, ...: the shim's psyq_api_*, as in a game's unit */
#include "psxstack/psyq/libapi.h"
#include "psxstack/psyq/libcd.h"
#include "psxstack/psyq/libetc.h"
#include "psxstack/psyq/libmcrd.h"
#include "psxstack/psyq/libsnd.h"
#include "psxstack/psyq/libspu.h"

/* ---- Stubs for the runtime */

const uint8_t *port_tag_base;
uint32_t port_tag_span;
static u16 spu_regs[0x100];

uint32_t port_ptr_to_u32(const void *p) {
    return (uint32_t)(uintptr_t)p;
}

void port_unimplemented(const char *fn) {
    printf("FAIL port_unimplemented: %s\n", fn);
    exit(1);
}

void spu_write16(uint32_t offset, uint16_t value) {
    spu_regs[(offset & 0x1FF) / 2] = value;
}

uint16_t spu_read16(uint32_t offset) {
    return spu_regs[(offset & 0x1FF) / 2];
}

void spu_dma_write(const uint16_t *data, uint32_t halfwords) {
    (void)data;
    (void)halfwords;
}

void spu_cd_input(const int16_t *samples, int frames) {
    (void)samples;
    (void)frames;
}

/* ---- Checks */

static int failures;

static void check(int cond, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int cond, const char *fmt, ...) {
    va_list ap;

    printf("%s ", cond ? "ok  " : "FAIL");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    failures += !cond;
}

static void ticks(int n) {
    while (n-- > 0) {
        psyq_vsync_tick();
    }
}

/* ---- LIBAPI: events and root counter 3 */

static int callbacks, events, out_of_order;

static void on_vsync(void) {
    callbacks++;
}

static s32 on_rcnt3(void) {
    events++;
    if (events != callbacks) {
        out_of_order = 1; /* the VSyncCallback handler has not run before the event in this tick */
    }
    return 0;
}

static void test_events(void) {
    s32 intr, poll;

    VSyncCallback(on_vsync);
    check(EnterCriticalSection() == 1 && EnterCriticalSection() == 0, "EnterCriticalSection: 1, then 0 inside one");
    ExitCriticalSection();
    ExitCriticalSection();
    intr = OpenEvent(RCntCNT3, EvSpINT, EvMdINTR, on_rcnt3);
    poll = OpenEvent(RCntCNT3, EvSpINT, EvMdNOINTR, NULL);
    check(((u32)intr & 0xFFFF0000u) == 0xF1000000u && poll == intr + 1, "OpenEvent: handles %08X, %08X", (u32)intr,
          (u32)poll);
    check(EnableEvent(intr) == 1 && EnableEvent(poll) == 1 && EnableEvent(0x1234) == 0, "EnableEvent");
    ticks(3);
    check(events == 0 && TestEvent(poll) == 0, "no event before StartRCnt (%d)", events);
    check(SetRCnt(RCntCNT3, 1, RCntMdINTR) == 1 && StartRCnt(RCntCNT3) == 1, "SetRCnt, StartRCnt");
    callbacks = 0;
    ticks(5);
    check(events == 5 && callbacks == 5 && !out_of_order,
          "RCntCNT3: one event per vsync tick, after the VSyncCallback handler (%d events, %d callbacks)", events,
          callbacks);
    check(TestEvent(poll) == 1 && TestEvent(poll) == 0, "TestEvent: an EvMdNOINTR event, once, then cleared");
    check(DisableEvent(intr) == 1, "DisableEvent");
    ticks(2);
    check(events == 5 && TestEvent(poll) == 1, "a disabled event's handler does not run; the other event still comes");
    check(StopRCnt(RCntCNT3) == 1, "StopRCnt");
    ticks(2);
    check(TestEvent(poll) == 0, "no event once the counter is stopped");
    check(CloseEvent(intr) == 1 && CloseEvent(poll) == 1 && TestEvent(poll) == 0 && CloseEvent(poll) == 0, "CloseEvent");
    check(OpenEvent(RCntCNT3, EvSpINT, EvMdINTR, on_rcnt3) == intr, "a closed event's block is reused");
    VSyncCallback(NULL);
    psyq_api_reset();
}

/* ---- LIBCARD and the BIOS's file calls on a fresh card, the card LIBMCRD sees too */

static u8 card[0x20000];
static int card_writes;
static s32 ev_ioe, ev_err, ev_timeout, ev_new, hw_ioe, hw_err, hw_timeout, hw_new;

static void card_written(int slot) {
    if (slot == 0) {
        card_writes++;
    }
}

/* The software event that came (after a tick): 0 IOE, 1 ERROR, 2 TIMOUT, 3 NEW, -1 none. */
static int sw_event(void) {
    int r = -1;
    if (TestEvent(ev_ioe) == 1) {
        r = 0;
    } else if (TestEvent(ev_err) == 1) {
        r = 1;
    } else if (TestEvent(ev_timeout) == 1) {
        r = 2;
    } else if (TestEvent(ev_new) == 1) {
        r = 3;
    }
    return r;
}

static int hw_event(void) {
    int r = -1;
    if (TestEvent(hw_ioe) == 1) {
        r = 0;
    } else if (TestEvent(hw_err) == 1) {
        r = 1;
    } else if (TestEvent(hw_timeout) == 1) {
        r = 2;
    } else if (TestEvent(hw_new) == 1) {
        r = 3;
    }
    return r;
}

static int dir_checksums_ok(void) {
    int f, i;
    for (f = 0; f < 16; f++) {
        u8 x = 0;
        for (i = 0; i < 0x7F; i++) {
            x ^= card[f * 0x80 + i];
        }
        if (x != card[f * 0x80 + 0x7F]) {
            return 0;
        }
    }
    return 1;
}

static void test_card(void) {
    static u8 data[0x4000], back[0x4000];
    DIRENTRY dir[16], mdir[15];
    s32 fd, n, cmds, result, files, r;
    int i, w;

    psyq_mcrd_format_image(card);
    psyq_mcrd_set_card(0, card, card_written);
    psyq_mcrd_set_card(1, NULL, NULL);
    InitCARD(0);
    ev_ioe = OpenEvent(SwCARD, EvSpIOE, EvMdNOINTR, NULL);
    ev_err = OpenEvent(SwCARD, EvSpERROR, EvMdNOINTR, NULL);
    ev_timeout = OpenEvent(SwCARD, EvSpTIMOUT, EvMdNOINTR, NULL);
    ev_new = OpenEvent(SwCARD, EvSpNEW, EvMdNOINTR, NULL);
    hw_ioe = OpenEvent(HwCARD, EvSpIOE, EvMdNOINTR, NULL);
    hw_err = OpenEvent(HwCARD, EvSpERROR, EvMdNOINTR, NULL);
    hw_timeout = OpenEvent(HwCARD, EvSpTIMOUT, EvMdNOINTR, NULL);
    hw_new = OpenEvent(HwCARD, EvSpNEW, EvMdNOINTR, NULL);
    StartCARD();
    _bu_init();
    EnableEvent(ev_ioe);
    EnableEvent(ev_err);
    EnableEvent(ev_timeout);
    EnableEvent(ev_new);
    EnableEvent(hw_ioe);
    EnableEvent(hw_err);
    EnableEvent(hw_timeout);
    EnableEvent(hw_new);

    /* the card's status, as the second game's getMemoryCardStatus asks for it */
    check(_card_info(0x00) == 1 && sw_event() == -1, "_card_info: accepted, nothing before the tick");
    check(_card_load(0x00) == 0, "a second command while one runs is refused");
    ticks(1);
    check(sw_event() == 3 && _card_chan() == 0x00, "_card_info on a card just inserted: SwCARD EvSpNEW");
    _card_info(0x00);
    ticks(1);
    check(sw_event() == 3, "_card_info again: still EvSpNEW (only a write clears the flag)");
    _card_clear(0x00);
    ticks(1);
    check(hw_event() == 0 && sw_event() == -1, "_card_clear: HwCARD EvSpIOE");
    _card_info(0x00);
    ticks(1);
    check(sw_event() == 0, "_card_info after it: SwCARD EvSpIOE");
    _card_load(0x00);
    ticks(1);
    check(sw_event() == 0, "_card_load of a formatted card: EvSpIOE");
    _card_info(0x10);
    ticks(1);
    check(sw_event() == 2 && _card_chan() == 0x10, "_card_info of an empty slot: EvSpTIMOUT");
    _card_clear(0x10);
    ticks(1);
    check(hw_event() == 2, "_card_clear of an empty slot: HwCARD EvSpTIMOUT");

    /* create, write, read back (synchronous) */
    for (i = 0; i < (int)sizeof(data); i++) {
        data[i] = (u8)(i * 7 + (i >> 8));
    }
    w = card_writes;
    fd = open("bu00:BASLUS-TEST0001", (2 << 16) | FCREAT);
    check(fd >= 2 && card_writes > w, "open with FCREAT: 2 blocks created (fd %d)", fd);
    check(close(fd) == fd, "close");
    check(open("bu00:BASLUS-TEST0001", (1 << 16) | FCREAT) == -1, "FCREAT of an existing name fails");
    check(open("bu00:BASLUS-NONE", FREAD) == -1, "open of a missing file fails");
    check(open("bu10:BASLUS-TEST0001", FREAD) == -1, "open on the empty slot fails");
    check(open("sim:C:\\DEVELOP\\X.BIN", FREAD) == -1, "open on another device (sim:) fails");
    fd = open("bu00:BASLUS-TEST0001", FWRITE);
    check(fd >= 2, "open for writing");
    check(write(fd, data, 0x80) == 0x80 && lseek(fd, 0, 1) == 0x80, "write: a frame, the position after it");
    check(write(fd, data, 100) == -1, "write: not a whole frame: -1");
    check(lseek(fd, 0x3F80, 0) == 0x3F80 && write(fd, data, 0x100) == -1, "write: past the file's end: -1");
    check(lseek(fd, 0x80, 0) == 0x80 && write(fd, data + 0x80, 0x3F80) == 0x3F80, "write: the rest of the file");
    check(lseek(fd, 1, 2) == -1 && lseek(fd, -1, 0) == -1, "lseek: only SEEK_SET and SEEK_CUR, inside the file");
    close(fd);
    check(dir_checksums_ok(), "the directory frames' checksums");
    fd = open("bu00:BASLUS-TEST0001", FREAD);
    memset(back, 0, sizeof(back));
    n = read(fd, back, 0x80);
    check(n == 0x80 && lseek(fd, 0x1F80, 1) == 0x2000 && read(fd, back + 0x2000, 0x2000) == 0x2000,
          "read: a frame, lseek SEEK_CUR, the second block");
    check(memcmp(back, data, 0x80) == 0 && memcmp(back + 0x2000, data + 0x2000, 0x2000) == 0,
          "read back what was written, across the block boundary");
    close(fd);

    /* asynchronous (FASYNC), as the second game's stepMemoryCardSave/Load */
    fd = open("bu00:BASLUS-TEST0001", FASYNC | FWRITE);
    memset(back, 0x5A, 0x100);
    check(write(fd, back, 0x100) == 0x100 && sw_event() == -1, "an asynchronous write returns at once");
    check(card[psyq_card_find(card, "BASLUS-TEST0001") * 0x2000] == data[0], "the card is unchanged before the tick");
    ticks(1);
    check(sw_event() == 0 && card[psyq_card_find(card, "BASLUS-TEST0001") * 0x2000] == 0x5A,
          "the write lands at the tick: SwCARD EvSpIOE");
    close(fd);
    fd = open("bu00:BASLUS-TEST0001", FASYNC | FREAD);
    memset(back, 0, 0x100);
    check(lseek(fd, 0x80, 0) == 0x80 && read(fd, back, 0x80) == 0x80 && back[0] == 0, "an asynchronous read");
    ticks(1);
    check(sw_event() == 0 && back[0] == 0x5A && back[0x7F] == 0x5A, "the data arrives at the tick: EvSpIOE");
    close(fd);

    /* the directory */
    fd = open("bu00:BASLUS-TWO", (1 << 16) | FCREAT);
    close(fd);
    memset(dir, 0, sizeof(dir));
    n = 0;
    if (firstfile("bu00:*", &dir[0]) == &dir[0]) {
        do {
            n++;
        } while (n < 16 && nextfile(&dir[n]) == &dir[n]);
    }
    check(n == 2 && strcmp(dir[0].name, "BASLUS-TEST0001") == 0 && dir[0].size == 0x4000 && dir[0].head == 1 &&
          strcmp(dir[1].name, "BASLUS-TWO") == 0 && dir[1].size == 0x2000 && dir[1].head == 3,
          "firstfile/nextfile: %d files (%s %d, %s %d)", n, dir[0].name, dir[0].size, dir[1].name, dir[1].size);
    check(firstfile("bu00:BASLUS-T?O", &dir[0]) == &dir[0] && strcmp(dir[0].name, "BASLUS-TWO") == 0 &&
          nextfile(&dir[1]) == NULL, "firstfile with '?'");
    check(firstfile("bu10:*", &dir[0]) == NULL, "firstfile on the empty slot");

    /* LIBMCRD sees the same card */
    files = 0;
    r = MemCardGetDirentry(0x00, "*", mdir, &files, 0, 15);
    check(r == 0 && files == 2 && mdir[0].size == 0x4000, "LIBMCRD's directory of the same card: %d files", files);
    check(MemCardReadFile(0x00, "BASLUS-TEST0001", (u32 *)back, 0x100, 0x80) == 1, "MemCardReadFile");
    while (MemCardSync(1, &cmds, &result) == 0) {
    }
    check(result == 0 && memcmp(back, data + 0x100, 0x80) == 0, "LIBMCRD reads what the BIOS calls wrote (result %d)",
          result);

    /* erase, format, an unformatted card */
    check(erase("bu00:BASLUS-TWO") == 1 && erase("bu00:BASLUS-TWO") == 0 && card[3 * 0x80] == 0xA1,
          "erase: the block marked deleted (A1)");
    check(firstfile("bu00:*", &dir[0]) == &dir[0] && nextfile(&dir[1]) == NULL, "one file left");
    fd = open("bu00:BASLUS-AGAIN", (1 << 16) | FCREAT);
    check(fd >= 2 && psyq_card_find(card, "BASLUS-AGAIN") == 3, "an erased block is free again");
    close(fd);
    MemCardUnformat(0x00);
    _card_load(0x00);
    ticks(1);
    check(sw_event() == 3, "_card_load of an unformatted card: EvSpNEW");
    check(open("bu00:BASLUS-TEST0001", FREAD) == -1 && firstfile("bu00:*", &dir[0]) == NULL,
          "no file on an unformatted card");
    check(_card_format(0x00) == 1 && _card_format(0x10) == 0 && card[0] == 'M' && card[1] == 'C',
          "_card_format: 1, 0 without a card");
    MemCardUnformat(0x00);
    check(format("bu00:") == 1 && card[0] == 'M' && firstfile("bu00:*", &dir[0]) == NULL, "format(\"bu00:\")");
    psyq_card_reset();
    psyq_api_reset();
}

/* ---- LIBCD over a small ISO 9660 image */

#define ISO_SECTORS 40
static u8 iso[ISO_SECTORS][2352];

static u8 *iso_data(u32 lba) {
    return iso[lba] + 24;
}

static void put_both32(u8 *p, u32 v) {
    p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24);
    p[4] = (u8)(v >> 24), p[5] = (u8)(v >> 16), p[6] = (u8)(v >> 8), p[7] = (u8)v;
}

/* A directory record at p: its length. */
static int iso_record(u8 *p, const char *name, int nlen, u32 extent, u32 size, int dir) {
    int len = 33 + nlen + ((33 + nlen) & 1);
    memset(p, 0, (size_t)len);
    p[0] = (u8)len;
    put_both32(p + 2, extent);
    put_both32(p + 10, size);
    p[25] = dir ? 2 : 0;
    p[32] = (u8)nlen;
    memcpy(p + 33, name, (size_t)nlen);
    return len;
}

static int iso_read(unsigned lba, u8 *sector) {
    if (lba >= ISO_SECTORS) {
        return 0;
    }
    memcpy(sector, iso[lba], 2352);
    return 1;
}

static void build_iso(void) {
    u8 *d;
    int ofs, i;
    u32 lba;
    char name[16];

    memset(iso, 0, sizeof(iso));
    for (lba = 0; lba < ISO_SECTORS; lba++) {
        CdlLOC loc;
        CdIntToPos((int)lba, &loc);
        memset(iso[lba] + 1, 0xFF, 10); /* the sync pattern */
        iso[lba][12] = loc.minute, iso[lba][13] = loc.second, iso[lba][14] = loc.sector, iso[lba][15] = 2;
        iso[lba][18] = iso[lba][22] = 0x08; /* Form 1 data */
    }
    d = iso_data(16); /* the primary volume descriptor */
    d[0] = 1;
    memcpy(d + 1, "CD001", 5);
    d[6] = 1;
    iso_record(d + 156, "\0", 1, 18, 2048, 1);
    d = iso_data(17); /* the terminator */
    d[0] = 255;
    memcpy(d + 1, "CD001", 5);
    d = iso_data(18); /* the root: ., .., DATA, P.DRV;1 */
    ofs = iso_record(d, "\0", 1, 18, 2048, 1);
    ofs += iso_record(d + ofs, "\1", 1, 18, 2048, 1);
    ofs += iso_record(d + ofs, "DATA", 4, 19, 4096, 1);
    ofs += iso_record(d + ofs, "P.DRV;1", 7, 22, 5000, 0);
    d = iso_data(19); /* DATA: two sectors, MOVIE.STR;1 in the second */
    ofs = iso_record(d, "\0", 1, 19, 4096, 1);
    ofs += iso_record(d + ofs, "\1", 1, 18, 2048, 1);
    for (i = 0; ofs + 50 < 2048; i++) {
        snprintf(name, sizeof(name), "F%02d.BIN;1", i);
        ofs += iso_record(d + ofs, name, (int)strlen(name), 30, 16, 0);
    }
    iso_record(iso_data(20), "MOVIE.STR;1", 11, 25, 3 * 2048, 0);
    for (lba = 22; lba < 28; lba++) {
        for (i = 0; i < 2048; i++) {
            iso_data(lba)[i] = (u8)(lba * 31 + i);
        }
    }
}

static void test_cd(void) {
    CdlFILE f, *found;
    CdlLOC loc;
    static u8 buf[4 * 2048];
    int t, left, first;
    s32 r;

    build_iso();
    psyq_cd_set_reader(iso_read);
    found = CdSearchFile(&f, "\\P.DRV;1");
    check(found == &f && CdPosToInt(&f.pos) == 22 && f.size == 5000 && strcmp(f.name, "P.DRV;1") == 0,
          "CdSearchFile \\P.DRV;1: sector %d, %u bytes, %s", CdPosToInt(&f.pos), f.size, f.name);
    found = CdSearchFile(&f, "\\DATA\\MOVIE.STR;1");
    check(found == &f && CdPosToInt(&f.pos) == 25 && f.size == 3 * 2048,
          "CdSearchFile in a subdirectory's second sector: sector %d", CdPosToInt(&f.pos));
    check(CdSearchFile(&f, "\\NOPE.BIN;1") == NULL && CdSearchFile(&f, "\\DATA\\P.DRV;1") == NULL &&
          CdSearchFile(&f, "\\P.DRV") == NULL && CdSearchFile(&f, "\\P.DRV;1\\X") == NULL,
          "CdSearchFile: missing names, the wrong directory, no version: NULL");
    psyq_cd_set_reader(NULL);
    check(CdSearchFile(&f, "\\P.DRV;1") == (CdlFILE *)-1, "CdSearchFile without a disc: -1");
    psyq_cd_set_reader(iso_read);

    /* CdRead, as cd_file.c reads: Setloc, CdRead, CdReadSync(1) per frame */
    for (first = 0; first < 2; first++) {
        CdIntToPos(22, &loc);
        memset(buf, 0, sizeof(buf));
        check(CdControlB(0x02, (u8 *)&loc, NULL) == 1 && CdRead(3, (u32 *)buf, 0x80) == 1, "CdRead started");
        for (t = 0; (left = CdReadSync(1, NULL)) > 0 && t < 100; t++) {
            psyq_vsync_tick();
        }
        check(left == 0 && memcmp(buf, iso_data(22), 2048) == 0 && memcmp(buf + 4096, iso_data(24), 2048) == 0 &&
              buf[3 * 2048] == 0, "CdRead: 3 sectors in %d ticks (the seek, then 3 a tick at double speed)", t);
        check(CdReadSync(0, NULL) == 0, "CdReadSync(0) after the end: 0");
    }
    CdIntToPos(ISO_SECTORS - 1, &loc);
    CdControlB(0x02, (u8 *)&loc, NULL);
    CdRead(3, (u32 *)buf, 0x80);
    r = CdReadSync(0, NULL);
    check(r == -1, "CdRead past the disc's end: CdReadSync -1");
    check(CdSync(1, NULL) == 2, "CdSync: nothing pending: CdlComplete");
    CdControlF(0x09, NULL);
    check(CdSync(1, NULL) == 0 && CdSync(0, NULL) == 2 && CdSync(1, NULL) == 2,
          "CdSync: CdlNoIntr while a CdControlF command is pending, CdlComplete once it has run");
    psyq_cd_set_reader(NULL);
    psyq_cd_reset();
}

/* ---- LIBSPU and LIBSND */

static const u8 seq_file[] = {
    'p', 'Q', 'E', 'S', 0, 0, 0, 1, /* the magic, the version */
    0x00, 0x30, 0x07, 0xA1, 0x20, 0x04, 0x02, /* resolution 48, tempo 500000 us (120 bpm), rhythm */
    0x00, 0x90, 0x3C, 0x40, 0x30, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00, /* a note, its end, the end of the track */
};

static void test_sound(void) {
    SpuVoiceAttr va;
    SpuCommonAttr ca;
    static u8 table[0x200];
    s16 l, r, a;
    int v, ok;

    memset(spu_regs, 0xFF, sizeof(spu_regs));
    memset(&va, 0, sizeof(va));
    va.mask = SPU_VOICE_ADSR_RR;
    va.voice = 0xFFFFFF;
    va.rr = 0;
    SpuSetVoiceAttr(&va);
    for (ok = 1, v = 0; v < 24; v++) {
        ok &= spu_read16((u32)v * 16 + 10) == 0xFFE0 && spu_read16((u32)v * 16 + 8) == 0xFFFF;
    }
    check(ok, "SpuSetVoiceAttr: the release rate of all 24 voices, the rest of ADSR kept");
    memset(&ca, 0, sizeof(ca));
    ca.mask = SPU_COMMON_MVOLL | SPU_COMMON_MVOLR | SPU_COMMON_CDVOLL | SPU_COMMON_CDVOLR | SPU_COMMON_CDMIX;
    ca.mvol.left = ca.mvol.right = 0x3FFF;
    ca.cd.volume.left = ca.cd.volume.right = 0x2000;
    ca.cd.mix = 0;
    SpuSetCommonAttr(&ca);
    check(spu_read16(0x180) == 0x3FFF && spu_read16(0x1B0) == 0x2000 && spu_read16(0x1B2) == 0x2000 &&
          spu_read16(0x1AA) == 0xFFFE, "SpuSetCommonAttr: main and CD volumes, SPUCNT's CD bit cleared");
    check(SpuClearReverbWorkArea(3) == 0 && SpuClearReverbWorkArea(10) == -1, "SpuClearReverbWorkArea");

    SsInit();
    SsSetTableSize(table, 2, 1);
    SsSetTickMode(1); /* SS_TICK60 */
    SsStart();
    a = SsSeqOpen((u32 *)seq_file, 0);
    check(a == 0 && SsSeqOpen((u32 *)seq_file, 0) == 1, "SsSeqOpen: access numbers 0 and 1");
    SsSeqClose(1);
    SsSeqSetVol(a, 100, 90);
    SsSeqGetVol(a, 0, &l, &r);
    check(l == 100 && r == 90, "SsSeqSetVol/SsSeqGetVol (%d, %d)", l, r);
    SsSeqPlay(a, 1, 1);
    check(snd_seq(a, 0)->flags == SND_SEQ_PLAYING, "SsSeqPlay");
    ticks(20);
    check(snd_seq(a, 0)->flags == SND_SEQ_PLAYING, "playing after 20 ticks (the note lasts 30)");
    ticks(20);
    check(snd_seq(a, 0)->flags == 0, "SsStart's own tick (SS_TICK60, one per vsync) played it to its end");
    SsSetTickMode(0x1000); /* SS_NOTICK: the game ticks */
    SsStart();
    SsSeqPlay(a, 1, 1);
    ticks(60);
    check(snd_seq(a, 0)->flags == SND_SEQ_PLAYING, "with SS_NOTICK the vsync ticks nothing");
    SsSeqStop(a);
    check(!(snd_seq(a, 0)->flags & SND_SEQ_PLAYING), "SsSeqStop");
    SsSeqClose(a);
    check(SsSeqOpen((u32 *)seq_file, 0) == 0, "SsSeqClose freed the access number");
    SsSetMono();
    check(snd.mono == 1, "SsSetMono");
    SsSetStereo();
    check(snd.mono == 0, "SsSetStereo");
    psyq_snd_reset();
}

/* The runtime's per-frame hook (pump.c port_frame): the CD's tick. */
static void frame(void) {
    psyq_cd_tick();
}

int main(void) {
    psyq_set_vsync_hook(frame);
    test_events();
    test_card();
    test_cd();
    test_sound();
    printf("shim test: %s (%d failure(s))\n", failures ? "FAIL" : "pass", failures);
    return failures ? 1 : 0;
}
