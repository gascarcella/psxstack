/* psxstack/hooks.h: the host side of the PC port's hook macros and the port_* interface the game's C calls
 * (GAME_CONTRACT.md "2. The hook header"; docs/PORT.md "Hook macros"). The game's include/port.h keeps the PS1 side of
 * every macro (without PC_PORT each expands to the original code) and includes this file under PC_PORT; the runtime
 * includes it through port_runtime.h. The game's constants (the slots, the heap, the brand) come from the generated
 * psxstack_game_gen.h (tools/game_gen.py, from the game's game.json): nothing here names a game.
 *
 * Addresses: the host keeps the PS1's addresses as the *names* of things that live in the overlay slots. A function in
 * a slot is a *tag*: the PS1 address as an integer in a function-pointer variable (SLOT_FUNC), a constant expression
 * for the static tables. A tag is never called directly: OVERLAY_FN turns it into the host function at the call. Data
 * in a slot is slot-relative (SLOT_PTR). `tier` is the slot's 1-based index in game.json's memory.slots. */
#ifndef PSXSTACK_HOOKS_H
#define PSXSTACK_HOOKS_H

#include <stddef.h>
#include <stdint.h>

#include "psxstack_game_gen.h"

/* --- Waits: the body of a busy-wait that only an interrupt (vsync, CD, GPU) can end; port_wait() runs the pending
 * "interrupts" so that the condition can change. PLATFORM_HALT: a deliberate endless loop: reports the place, stops. */
#define PLATFORM_WAIT() port_wait()
#define PLATFORM_HALT() port_halt(__FILE__, __LINE__)

/* --- The scratchpad stack: nothing on the host, the object runs on the normal stack. */
#define PORT_SCRATCHPAD_STACK_ENTER(top) ((void)0)
#define PORT_SCRATCHPAD_STACK_LEAVE() ((void)0)

/* --- Overlay loads: the file becomes the tier's current overlay (its .data/.bss restored to the load-time contents);
 * a data file is copied into the slot buffer. All five arguments are evaluated once. */
#define OVERLAY_COPY(tier, file, dst, src, size) port_overlay_load((tier), (file), (dst), (src), (size))

/* --- Pointers and integers. */
#define PTR_ADD(type, ofs, base) ((type)((char *)(base) + (ofs)))
#define PTR_TO_S32(p) port_ptr_to_s32(p)
#define S32_TO_PTR(type, v) ((type)port_s32_to_ptr(v))
/* A pointer becomes its word offset in the tag window; a tag already (addPrim's setaddr(p, getaddr(ot))) is kept. The
 * branch is chosen at compile time by the argument's type (5: a pointer, __builtin_classify_type); the other is never
 * evaluated. */
#define PTR_TO_U32(p)                                                                                       \
    __builtin_choose_expr(__builtin_classify_type(p) == 5, port_ptr_to_u32((const void *)(uintptr_t)(p)), \
                          (uint32_t)(uintptr_t)(p))

/* --- Things at fixed addresses. */
#define SLOT_FUNC(type, addr) ((type)(uintptr_t)(addr))
#define OVERLAY_FN(tier, fn) ((__typeof__(fn))port_overlay_resolve((tier), (uintptr_t)(fn)))

/* The enum keeps the tier and the address (less the RAM base, to stay an int) under the function's name. */
#define LATE_FUNC(tier, addr, ret, name, params) \
    typedef ret name##_late_fn params;           \
    enum { name##_late_tier = (tier), name##_late_ofs = (int)((addr) - PSXSTACK_GAME_RAM_BASE) }
#define LATE_CALL(name) \
    ((name##_late_fn *)port_overlay_resolve(name##_late_tier, (uintptr_t)PSXSTACK_GAME_RAM_BASE + name##_late_ofs))

#define SLOT_PTR(tier, type, addr) ((type)(port_slot##tier + ((addr) - PORT_SLOT##tier##_BASE)))

#if PORT_HEAP_PRESENT
#define HEAP_START(type) ((type)port_heap_start)
#define HEAP_END(type) ((type)port_heap_end)
#define HEAP_SIZE_FROM(first) ((uint32_t)(port_heap_end - (uint8_t *)(first)))
#define HEAP_ADDR(addr) (port_heap_start + ((addr) - PORT_HEAP_START_ADDR))
#else
/* The description has no memory.heap (GAME_CONTRACT.md "1. game.json": the game's heap is its own data), so a use
 * of a heap hook is a mistake: each expands to the sizeof of an incomplete type, whose name says why it fails. */
#define PSXSTACK_NO_HEAP_ERROR sizeof(struct psxstack_error_the_game_description_has_no_memory_heap)
#define HEAP_START(type) ((type)PSXSTACK_NO_HEAP_ERROR)
#define HEAP_END(type) ((type)PSXSTACK_NO_HEAP_ERROR)
#define HEAP_SIZE_FROM(first) ((uint32_t)PSXSTACK_NO_HEAP_ERROR)
#define HEAP_ADDR(addr) ((uint8_t *)PSXSTACK_NO_HEAP_ERROR)
#endif

#define BIOS_PTR(type, addr) ((type)port_bios_ptr(addr))

/* ================================================= What the port implements ================================================ */

/* Runs the pending vsync/CD/GPU "interrupts"; may sleep until the next one is due. */
void port_wait(void);
/* Reports an endless loop the game entered on purpose and stops. */
void port_halt(const char *file, int line) __attribute__((noreturn));
/* Logs that the game is about to free file `id` while the CD is still reading into its buffer, and waits for the
 * read. */
void port_file_wait_read(int32_t id);

/* The overlay manager. `tier` is a slot's 1-based index.
 * port_overlay_load: `file` (a file ID) becomes the tier's current overlay, with its .data/.bss as at load time; the
 * caller has already checked that it is not the resident one. `dst`/`src`/`size` are the PS1's memcpy arguments, for
 * files that are data. Returns `dst`, as memcpy does.
 * port_overlay_resolve: the host function for `addr` in the tier's current overlay. An `addr` outside the PS1's RAM
 * is a host function pointer already and is returned unchanged; an address the current overlay does not define is a
 * fatal error. */
void *port_overlay_load(int tier, int32_t file, void *dst, const void *src, uint32_t size);
void (*port_overlay_resolve(int tier, uintptr_t addr))(void);

/* The memory arena (docs/PORT.md "Memory arena"): one static block, port_arena, that stands for the PS1's RAM from the
 * first slot up, at the PS1's distances: the slots, then the heap (larger than the PS1's) when the description has
 * one (PORT_HEAP_PRESENT). The regions are macros on port_arena (port_slot<n>, PORT_SLOT<n>_OFS and PORT_HEAP_OFS
 * are generated), so that their addresses stay constant expressions for static initializers. No alignment is
 * assumed: an arena pointer's PS1-style address is PORT_SLOT1_BASE + its offset, and an ordering-table tag is an
 * offset in the tag window (below). */
extern uint8_t port_arena[PORT_ARENA_SIZE];
#if PORT_HEAP_PRESENT
#define port_heap_start (port_arena + PORT_HEAP_OFS)
#define port_heap_end (port_arena + PORT_ARENA_SIZE)
#endif

/* NULL <-> 0; an arena pointer <-> its PS1-style address; anything else is a fatal error. */
int32_t port_ptr_to_s32(const void *p);
void *port_s32_to_ptr(int32_t v);

/* The tag window: the game's whole writable memory on the host, the units' .data/.bss and the arena, which
 * port_overlay_init measures and port_tag_window_set records. A 24-bit ordering-table tag is a word offset from
 * port_tag_base (entries and primitives are word-aligned), so the window may span up to 64 MB, and the shim's DrawOTag
 * follows tags inside the window only. port_ptr_to_u32: a pointer in the window -> its word offset (PTR_TO_U32);
 * anything else is a fatal error. */
#define PORT_TAG_SHIFT 2
extern const uint8_t *port_tag_base;
extern uint32_t port_tag_span;
void port_tag_window_set(const void *lo, const void *hi);
uint32_t port_ptr_to_u32(const void *p);

/* The byte at the PS1 address `addr` (0x1FC00000..) of the BIOS ROM's stand-in (PSXSTACK_GAME_BIOS_STANDINS). */
void *port_bios_ptr(uint32_t addr);

/* --- Fibers (runtime/fiber.c; docs/PORT.md "Fibers"): a game's own tasks, each on a host stack of the runtime's. A
 * game whose scheduler switches tasks itself (hand-written context switches, Psy-Q's OpenTh/ChangeTh) replaces its
 * switch glue under PC_PORT with these; the task records, priorities, waits and wake-ups stay the game's C.
 * port_fiber_create: a fiber that runs entry(arg) on its own stack when first switched to; it must end with
 *   port_fiber_exit (an entry that returns is fatal). Fatal when PORT_FIBER_MAX fibers are alive.
 * port_fiber_switch: suspends the current fiber and resumes `to` (a no-op for the current one); returns when something
 *   switches back. port_fiber_main: the fiber game_main runs on; port_fiber_current: the running one;
 *   port_fiber_index: 0 for the main fiber, else 1.. (logs).
 * port_fiber_exit: ends the current fiber (its slot is free for port_fiber_create) and resumes `to`; never returns.
 * port_fiber_destroy: drops a suspended fiber that will never be resumed (a killed task); the current one is fatal.
 * port_fiber_preempt: from a vblank handler (it runs inside the vsync tick, from VSync, PLATFORM_WAIT or the shim's
 *   own ticks): the switch to `to` happens at the very end of that tick, once the tick's work is done, so the
 *   interrupted fiber is suspended right after its tick, as the PS1's task right after the interrupt. The last call
 *   in a tick wins; a call outside a tick takes effect at the next tick's end.
 * Every switch happens where the game asks for it or at a tick's end: a run is as deterministic as without fibers.
 * Save states hold every live fiber (docs/RUNTIME.md "Save states"); the console's reset drops them all. */
typedef struct PortFiber PortFiber;
PortFiber *port_fiber_create(void (*entry)(void *), void *arg);
void port_fiber_switch(PortFiber *to);
PortFiber *port_fiber_main(void);
PortFiber *port_fiber_current(void);
int port_fiber_index(const PortFiber *f);
void port_fiber_exit(PortFiber *to) __attribute__((noreturn));
void port_fiber_destroy(PortFiber *f);
void port_fiber_preempt(PortFiber *to);

/* --- LIBC2's generator (psyq/libc2.c): rand's 32-bit state, what the PS1 keeps in its LIBC2 variable (0 at power-on,
 * srand's seed, then each draw's), for a game's adapter: its game_state_random_index when the game's random index is
 * rand's state (GAME_CONTRACT.md "4. The adapter units"). */
uint32_t port_rand_seed(void);

#endif /* PSXSTACK_HOOKS_H */
