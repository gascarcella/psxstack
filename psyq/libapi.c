/* psyq/libapi.c: LIBAPI's events, root counters and interrupt control (the BIOS's kernel calls the games make; the
 * file calls and the memory card are libcard.c). Written from the games' use of the API and psx-spx ("BIOS Event
 * Functions", "Timers"); deterministic, on the pump's vsync tick.
 *
 * Events: a table of PSYQ_API_EVENTS event control blocks, as the BIOS keeps them: OpenEvent(class, spec, mode, func)
 * takes a free one (status EvStWAIT, disabled) and returns its handle 0xF1000000 | index; EnableEvent makes it
 * EvStACTIVE, DisableEvent EvStWAIT again, CloseEvent frees it. A delivery (psyq_api_deliver: the root counter here,
 * the memory card in libcard.c) reaches every active event of that class and spec: an EvMdINTR event's handler runs
 * at once, inside the interrupt (the vsync tick); an EvMdNOINTR event becomes EvStALREADY, which TestEvent reports
 * (1) and clears back to EvStACTIVE. WaitEvent waits for that by running vsync ticks (the PS1 waits for an interrupt).
 *
 * Root counter 3 (RCntCNT3) is the vertical blank. SetRCnt(RCntCNT3, target, RCntMdINTR) + StartRCnt(RCntCNT3) make
 * every vsync tick deliver (RCntCNT3, EvSpINT): the point is libetc.c psyq_vsync_tick, after the game's VSyncCallback
 * handler and LIBSND's tick, before the runtime's per-frame work (pump.c port_frame). A handler runs on whichever
 * fiber ticked (its VSync, its PLATFORM_WAIT, LIBCD's StGetNext); one that calls port_fiber_preempt (a game's task
 * scheduler: Digimon Digital Card Battle's handleVsyncPreemption) is honoured at the end of that tick, as for a
 * VSyncCallback handler (docs/PORT.md "Fibers"). The counter's target is not used: the event comes every vblank (the
 * games set 1). Counters 0-2 (the pixel clock, the hblank, the system clock / 8) are hardware timers the shim does not
 * model: starting or reading one stops the run (port_unimplemented).
 *
 * Critical sections: EnterCriticalSection/ExitCriticalSection disable and enable the PS1's interrupts. Here no
 * interrupt runs except at the vsync tick, which a game reaches only through a wait, so they only count the nesting
 * (the trace shows a tick that runs inside one). EnterCriticalSection returns 1 when interrupts were enabled. */
#include <string.h>

#include "psyq_internal.h"
#include "psxstack/psyq_names.h" /* EnterCriticalSection, ...: the shim's psyq_api_* (Win32 has its own) */
#include "psxstack/psyq/libapi.h"

#define PSYQ_API_EVENTS 32
#define PSYQ_API_HANDLE 0xF1000000u

typedef struct PsyqEvent {
    u32 desc;
    s32 spec;
    s32 mode;
    s32 status; /* EvStUNUSED (free), EvStWAIT, EvStACTIVE, EvStALREADY */
    s32 (*func)();
} PsyqEvent;

typedef struct PsyqRCnt {
    u16 target;
    s32 mode;
    int started;
    u32 count; /* counter 3: vblanks since the last ResetRCnt */
} PsyqRCnt;

static PsyqEvent psyq_events[PSYQ_API_EVENTS];
static PsyqRCnt psyq_rcnt[4];
static int psyq_crit_depth;
static s32 psyq_clear_pad;

static PsyqEvent *psyq_event(s32 event) {
    u32 i = (u32)event & 0xFFFF;

    if (((u32)event & 0xFFFF0000u) != PSYQ_API_HANDLE || i >= PSYQ_API_EVENTS || psyq_events[i].status == EvStUNUSED) {
        return NULL;
    }
    return &psyq_events[i];
}

/* ---- Events */

s32 OpenEvent(u32 desc, s32 spec, s32 mode, s32 (*func)()) {
    int i;

    for (i = 0; i < PSYQ_API_EVENTS; i++) {
        if (psyq_events[i].status == EvStUNUSED) {
            psyq_events[i].desc = desc;
            psyq_events[i].spec = spec;
            psyq_events[i].mode = mode;
            psyq_events[i].func = func;
            psyq_events[i].status = EvStWAIT;
            PSYQ_TRACE("OpenEvent %08X spec %04X mode %04X func %u = %08X", desc, (unsigned)spec, (unsigned)mode,
                       PSYQ_PTR(func), PSYQ_API_HANDLE | (u32)i);
            return (s32)(PSYQ_API_HANDLE | (u32)i);
        }
    }
    PSYQ_TRACE("OpenEvent %08X spec %04X: no free event", desc, (unsigned)spec);
    return -1;
}

s32 CloseEvent(s32 event) {
    PsyqEvent *e = psyq_event(event);

    PSYQ_TRACE("CloseEvent %08X", (u32)event);
    if (e == NULL) {
        return 0;
    }
    memset(e, 0, sizeof(*e));
    return 1;
}

s32 EnableEvent(s32 event) {
    PsyqEvent *e = psyq_event(event);

    PSYQ_TRACE("EnableEvent %08X", (u32)event);
    if (e == NULL) {
        return 0;
    }
    if (e->status == EvStWAIT) {
        e->status = EvStACTIVE;
    }
    return 1;
}

s32 DisableEvent(s32 event) {
    PsyqEvent *e = psyq_event(event);

    PSYQ_TRACE("DisableEvent %08X", (u32)event);
    if (e == NULL) {
        return 0;
    }
    e->status = EvStWAIT;
    return 1;
}

/* 1 when the event has come since the last test (and clears it), else 0. Not traced: games poll it in loops. */
s32 TestEvent(s32 event) {
    PsyqEvent *e = psyq_event(event);

    if (e == NULL || e->status != EvStALREADY) {
        return 0;
    }
    e->status = EvStACTIVE;
    return 1;
}

/* Waits for the event (vsync ticks, as the PS1 waits for interrupts): 1, or 0 for an event that is not enabled. */
s32 WaitEvent(s32 event) {
    PsyqEvent *e = psyq_event(event);

    PSYQ_TRACE("WaitEvent %08X", (u32)event);
    if (e == NULL || e->status == EvStWAIT) {
        return 0;
    }
    while (e->status != EvStALREADY) {
        psyq_vsync_tick();
        if (e->status == EvStUNUSED || e->status == EvStWAIT) {
            return 0; /* closed or disabled by a handler meanwhile */
        }
    }
    e->status = EvStACTIVE;
    return 1;
}

void psyq_api_deliver(u32 desc, s32 spec) {
    int i;

    for (i = 0; i < PSYQ_API_EVENTS; i++) {
        PsyqEvent *e = &psyq_events[i];

        if (e->desc != desc || e->spec != spec || (e->status != EvStACTIVE && e->status != EvStALREADY)) {
            continue;
        }
        if (e->mode == EvMdINTR) {
            if (e->func != NULL) {
                e->func();
            }
        } else {
            e->status = EvStALREADY;
        }
    }
}

/* ---- Root counters */

static int psyq_rcnt_index(u32 spec) {
    return (spec & 0xFFFFFFFCu) == RCntCNT0 ? (int)(spec & 3) : -1;
}

s32 SetRCnt(u32 spec, u16 target, s32 mode) {
    int i = psyq_rcnt_index(spec);

    PSYQ_TRACE("SetRCnt %08X target %u mode %04X", spec, target, (unsigned)mode);
    if (i < 0) {
        return 0;
    }
    psyq_rcnt[i].target = target;
    psyq_rcnt[i].mode = mode;
    return 1;
}

s32 StartRCnt(u32 spec) {
    int i = psyq_rcnt_index(spec);

    PSYQ_TRACE("StartRCnt %08X", spec);
    if (i < 0) {
        return 0;
    }
    if (i != 3) {
        port_unimplemented("StartRCnt: root counters 0-2 (hardware timers)");
    }
    psyq_rcnt[i].started = 1;
    return 1;
}

s32 StopRCnt(u32 spec) {
    int i = psyq_rcnt_index(spec);

    PSYQ_TRACE("StopRCnt %08X", spec);
    if (i < 0) {
        return 0;
    }
    psyq_rcnt[i].started = 0;
    return 1;
}

s32 GetRCnt(u32 spec) {
    int i = psyq_rcnt_index(spec);

    if (i < 0) {
        return -1;
    }
    if (i != 3) {
        port_unimplemented("GetRCnt: root counters 0-2 (hardware timers)");
    }
    return (s32)(psyq_rcnt[3].count & 0xFFFF);
}

s32 ResetRCnt(u32 spec) {
    int i = psyq_rcnt_index(spec);

    PSYQ_TRACE("ResetRCnt %08X", spec);
    if (i < 0) {
        return 0;
    }
    psyq_rcnt[i].count = 0;
    return 1;
}

/* The vsync tick's root counter 3 (libetc.c psyq_vsync_tick, after the VSyncCallback handler and LIBSND's tick). */
void psyq_api_vsync(void) {
    PsyqRCnt *c = &psyq_rcnt[3];

    c->count++;
    if (psyq_crit_depth > 0) {
        PSYQ_TRACE("vsync tick inside a critical section (depth %d)", psyq_crit_depth);
    }
    if (c->started && (c->mode & RCntMdINTR)) {
        psyq_api_deliver(RCntCNT3, EvSpINT);
    }
}

/* ---- Interrupts */

s32 EnterCriticalSection(void) {
    s32 was_enabled = psyq_crit_depth == 0;

    psyq_crit_depth++;
    PSYQ_TRACE("EnterCriticalSection (depth %d)", psyq_crit_depth);
    return was_enabled;
}

void ExitCriticalSection(void) {
    if (psyq_crit_depth > 0) {
        psyq_crit_depth--;
    }
    PSYQ_TRACE("ExitCriticalSection (depth %d)", psyq_crit_depth);
}

/* Whether the BIOS's own vblank handler acknowledges the interrupt (0: it does; LIBETC and the pads set it): nothing
 * to acknowledge here. */
void ChangeClearPad(s32 val) {
    PSYQ_TRACE("ChangeClearPad %d", val);
    psyq_clear_pad = val;
}

/* ---- The console's reset and the save state */

/* The console's reset (psyq.c psyq_reset): no event open, the counters stopped, interrupts enabled. */
void psyq_api_reset(void) {
    memset(psyq_events, 0, sizeof(psyq_events));
    memset(psyq_rcnt, 0, sizeof(psyq_rcnt));
    psyq_crit_depth = 0;
    psyq_clear_pad = 0;
}

/* A save state (psyq_internal.h): the events (their handlers are code addresses of the image), the counters. */
void psyq_api_state(PortState *s) {
    PORT_STATE_VAR(s, psyq_events);
    PORT_STATE_VAR(s, psyq_rcnt);
    PORT_STATE_VAR(s, psyq_crit_depth);
    PORT_STATE_VAR(s, psyq_clear_pad);
}
