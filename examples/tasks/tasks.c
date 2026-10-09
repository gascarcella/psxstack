/* examples/tasks: a disc-free Psy-Q program whose work runs in tasks of its own, on psxstack's fibers (hooks.h
 * "Fibers"; docs/PORT.md "Fibers"): the stack's test of them (tests/tasks_test.py). It is the shape of a game that
 * schedules its tasks itself: a task record per task, a vblank handler that counts the frames a sleeping task waits
 * and preempts a running one, and a main loop that gives every ready task its turn and then draws the frame.
 *
 * The tasks: "quad" moves a quad and yields every turn; "blink" changes a colour, sleeps three vblanks, and after ten
 * changes ends (port_fiber_exit), whereupon main drops "quad" (port_fiber_destroy) and creates "busy" in its place;
 * "busy" never yields: it draws and waits for the vblank (VSync), and the handler preempts it back to main at the
 * end of that tick (port_fiber_preempt). So every operation runs: create, switch, exit, destroy (and a slot reused),
 * and the preemption from inside a vsync tick. The picture at a given vsync and the frame log are the same on every
 * run, and a run resumed from a save state continues them (the test checks both). hello's common.h serves this
 * example too (examples/hello/include); the Psy-Q declarations are the stack's (include/psxstack/psyq/).
 *
 * The vblank handler is a root counter 3 event, as Digimon Digital Card Battle installs its scheduler's
 * (OpenEvent(RCntCNT3, EvSpINT, EvMdINTR, handler), SetRCnt, StartRCnt, inside a critical section); a VSyncCallback
 * handler counts the vsyncs too, and both counts must equal VSync's every frame: the event fires once per tick, after
 * the VSyncCallback handler (psyq/libapi.c). A frame where they differ gets a red bar, which the picture's hash
 * would show. */
#include "common.h"
#include "psxstack/psyq/libapi.h"
#include "psxstack/psyq/libetc.h"
#include "psxstack/psyq/libgpu.h"

#define OT_LEN 8
#define TASKS 4

typedef struct Task {
    PortFiber *fiber;
    const char *name;
    int alive;
    int wait; /* vblanks still to sleep (the handler counts them down) */
} Task;

static Task tasks[TASKS];
static PortFiber *main_fiber;
static int vblanks;      /* the handler's count */
static int callbacks;    /* the VSyncCallback handler's count */
static int callback_last; /* the handler's count when the VSyncCallback handler last ran (it runs first) */
static int out_of_order; /* the counts disagreed: the red bar */
static int preemptions;  /* how often the handler found a task running */
static POLY_F4 bar[2];
static u32 ot[2][OT_LEN];
static POLY_F4 quad[2];
static POLY_G4 grad[2];
static POLY_F4 blink[2];
static DISPENV disp[2];
static DRAWENV draw[2];
static int db;

/* The vblank handler (the root counter 3 event): it runs inside the vsync tick, on whichever fiber ticked. A sleeping
 * task sleeps one vblank less; a task that is running (not main) is preempted at the end of the tick. */
static s32 vblank(void) {
    int i;
    vblanks++;
    if (callbacks != vblanks || callback_last != vblanks - 1) {
        out_of_order = 1; /* the VSyncCallback handler has not run first in this tick */
    }
    for (i = 0; i < TASKS; i++) {
        if (tasks[i].alive && tasks[i].wait > 0) {
            tasks[i].wait--;
        }
    }
    if (port_fiber_current() != main_fiber) {
        preemptions++;
        port_fiber_preempt(main_fiber);
    }
    return 0;
}

/* The VSyncCallback handler: counts. */
static void count_vsync(void) {
    callbacks++;
    callback_last = vblanks;
}

static void task_yield(void) {
    port_fiber_switch(main_fiber);
}

static void task_sleep(Task *t, int frames) {
    t->wait = frames;
    port_fiber_switch(main_fiber);
}

static void task_end(Task *t) {
    t->alive = 0;
    port_fiber_exit(main_fiber);
}

static Task *task_start(const char *name, void (*entry)(void *)) {
    int i;
    for (i = 0; i < TASKS; i++) {
        if (!tasks[i].alive) {
            tasks[i].name = name;
            tasks[i].alive = 1;
            tasks[i].wait = 0;
            tasks[i].fiber = port_fiber_create(entry, &tasks[i]);
            return &tasks[i];
        }
    }
    return NULL;
}

/* quad's turn: the quad at *x, then one pixel right, wrapping. Out of line, so that x is a local whose address
   escapes. */
static __attribute__((noinline)) void quad_step(int *x) {
    POLY_F4 *q = &quad[db];
    setPolyF4(q);
    setRGB0(q, 200, 120, 40);
    setXY4(q, *x, 20, *x + 60, 20, *x, 80, *x + 60, 80);
    addPrim(ot[db] + 1, q);
    *x = (*x + 1) % 260;
}

/* "quad": a quad that moves right one pixel a turn, wrapping; yields every turn. Its position is a local whose
   address escapes (quad_step), so under ASan its frame, at the top of the fiber's stack, has redzones: they are
   still poisoned when main destroys the task suspended in that frame, and the slot's next fiber (busy) must start
   on a clean stack. */
static void quad_task(void *arg) {
    int x = 0;
    for (;;) {
        quad_step(&x);
        task_yield();
    }
    (void)arg;
}

/* "blink": a square whose colour flips, three vblanks apart, ten times; then it ends. */
static void blink_task(void *arg) {
    Task *self = arg;
    int flips = 0;
    while (flips < 10) {
        POLY_F4 *p = &blink[db];
        setPolyF4(p);
        setRGB0(p, (u8)(flips & 1 ? 255 : 0), (u8)(flips & 1 ? 0 : 255), 60);
        setXY4(p, 220, 20, 300, 20, 220, 100, 300, 100);
        addPrim(ot[db] + 2, p);
        flips++;
        task_sleep(self, 3);
    }
    task_end(self);
}

/* "busy": a gradient whose top colour follows the vblank count; it never yields, it waits for the vblank itself. */
static void busy_task(void *arg) {
    for (;;) {
        POLY_G4 *g = &grad[db];
        setPolyG4(g);
        setRGB0(g, (u8)(vblanks * 4), 0, 0);
        setRGB1(g, 0, 255, 0);
        setRGB2(g, 0, 0, 255);
        setRGB3(g, 255, 255, 255);
        setXY4(g, 40, 120, 280, 130, 30, 220, 290, 230);
        addPrim(ot[db] + 3, g);
        VSync(0); /* the handler preempts this task at the end of this tick; the next turn resumes here */
    }
    (void)arg;
}

int main(void) {
    Task *quad_t, *blink_t, *busy_t = NULL;
    int i;

    main_fiber = port_fiber_main();
    ResetGraph(0);
    SetDefDispEnv(&disp[0], 0, 0, 320, 240);
    SetDefDispEnv(&disp[1], 0, 240, 320, 240);
    SetDefDrawEnv(&draw[0], 0, 240, 320, 240);
    SetDefDrawEnv(&draw[1], 0, 0, 320, 240);
    draw[0].isbg = draw[1].isbg = 1;
    setRGB0(&draw[0], 16, 16, 32);
    setRGB0(&draw[1], 16, 16, 32);
    SetDispMask(1);
    VSyncCallback(count_vsync);
    EnterCriticalSection();
    EnableEvent(OpenEvent(RCntCNT3, EvSpINT, EvMdINTR, vblank));
    SetRCnt(RCntCNT3, 1, RCntMdINTR);
    StartRCnt(RCntCNT3);
    ExitCriticalSection();
    quad_t = task_start("quad", quad_task);
    blink_t = task_start("blink", blink_task);
    for (;;) {
        ClearOTagR(ot[db], OT_LEN);
        /* every ready task gets its turn: it runs until it yields, sleeps, ends or is preempted */
        for (i = 0; i < TASKS; i++) {
            if (tasks[i].alive && tasks[i].wait == 0) {
                port_fiber_switch(tasks[i].fiber);
            }
        }
        if (!blink_t->alive && busy_t == NULL) {
            /* blink is gone: drop quad, and start busy (which takes a freed slot: fiber slots are reused) */
            quad_t->alive = 0;
            port_fiber_destroy(quad_t->fiber);
            busy_t = task_start("busy", busy_task);
        }
        if (out_of_order) {
            setPolyF4(&bar[db]);
            setRGB0(&bar[db], 255, 0, 0);
            setXY4(&bar[db], 0, 230, 320, 230, 0, 240, 320, 240);
            addPrim(ot[db], &bar[db]);
        }
        SetDrawEnv(&draw[db].dr_env, &draw[db]);
        addPrim(ot[db] + OT_LEN - 1, &draw[db].dr_env);
        DrawSync(0);
        if (VSync(0) != vblanks) {
            out_of_order = 1; /* the event did not fire once per tick */
        }
        PutDispEnv(&disp[db]);
        DrawOTag(ot[db] + OT_LEN - 1);
        db ^= 1;
    }
}
