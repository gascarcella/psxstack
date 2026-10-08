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
 * example too (examples/hello/include); the Psy-Q declarations are the stack's (include/psxstack/psyq/). */
#include "common.h"
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
static int preemptions;  /* how often the handler found a task running */
static u32 ot[2][OT_LEN];
static POLY_F4 quad[2];
static POLY_G4 grad[2];
static POLY_F4 blink[2];
static DISPENV disp[2];
static DRAWENV draw[2];
static int db;

/* The vblank handler (VSyncCallback): it runs inside the vsync tick, on whichever fiber ticked. A sleeping task
 * sleeps one vblank less; a task that is running (not main) is preempted at the end of the tick. */
static void vblank(void) {
    int i;
    vblanks++;
    for (i = 0; i < TASKS; i++) {
        if (tasks[i].alive && tasks[i].wait > 0) {
            tasks[i].wait--;
        }
    }
    if (port_fiber_current() != main_fiber) {
        preemptions++;
        port_fiber_preempt(main_fiber);
    }
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

/* "quad": a quad that moves right one pixel a turn, wrapping; yields every turn. */
static void quad_task(void *arg) {
    int x = 0;
    for (;;) {
        POLY_F4 *q = &quad[db];
        setPolyF4(q);
        setRGB0(q, 200, 120, 40);
        setXY4(q, x, 20, x + 60, 20, x, 80, x + 60, 80);
        addPrim(ot[db] + 1, q);
        x = (x + 1) % 260;
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
    VSyncCallback(vblank);
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
        SetDrawEnv(&draw[db].dr_env, &draw[db]);
        addPrim(ot[db] + OT_LEN - 1, &draw[db].dr_env);
        DrawSync(0);
        VSync(0);
        PutDispEnv(&disp[db]);
        DrawOTag(ot[db] + OT_LEN - 1);
        db ^= 1;
    }
}
