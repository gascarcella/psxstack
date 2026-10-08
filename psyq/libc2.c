/* psyq/libc2.c: LIBC2's rand and srand, the PS1's generator. The rest of LIBC2 (the string and memory functions,
 * sprintf, printf, abs, ...) is the host libc's (README.md "LIBC2, and LIBAPI's names the host has too").
 *
 * rand is a linear congruential generator over a 32-bit state: state = state * 0x41C64E6D + 12345, and it returns
 * bits 16-30 of the new state (0..0x7FFF: Psy-Q's RAND_MAX is 32767). srand sets the state. On the PS1 the state is a
 * variable in the executable's .bss (the second game's lies past the end of its EXE file), so a program that never
 * calls srand starts from 0, not from C's 1: the second game's states recorded in the emulator are 2.55 million draws
 * from 0 by its vsync 827 (its main loop spins on rand: about 3,000 a vsync), and would be 1.2 billion from 1. The
 * console's reset clears it again (the PS1's RAM is cleared) and a save state holds it.
 *
 * The host's names: rand and srand are also libc's, and a definition named rand in the executable would replace
 * libc's for the runtime and every shared library in the process. So, as for LIBAPI's file calls, the game's units
 * and this file see include/psxstack/psyq_names.h, which renames both to psyq_c2_*: the game's calls reach these, and
 * everyone else keeps the host's (DECISIONS "LIBC2's rand: the shim's, renamed"). The adapter reads the state through
 * the runtime's port_rand_seed (hooks.h): a game's game_state_random_index. */
#include "psyq_internal.h"
#include "psxstack/psyq_names.h" /* rand, srand: the shim's psyq_c2_* (libc has its own) */
/* The header's string functions have the PS1's s32 lengths, which the compiler's built-ins (strlen, memcpy, ...) would
 * flag here: this file defines none of them, it includes the header for rand's and srand's prototypes. */
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wincompatible-library-redeclaration"
#else
#pragma GCC diagnostic ignored "-Wbuiltin-declaration-mismatch"
#endif
#include "psxstack/psyq/libc2.h"
#pragma GCC diagnostic pop

static u32 psyq_rand_state; /* the generator's state: 0 at power-on (the PS1's .bss) */

s32 rand(void) {
    psyq_rand_state = psyq_rand_state * 0x41C64E6Du + 12345u;
    return (s32)((psyq_rand_state >> 16) & 0x7FFF);
}

void srand(u32 seed) {
    PSYQ_TRACE("srand %u", seed);
    psyq_rand_state = seed;
}

u32 psyq_rand_seed(void) {
    return psyq_rand_state;
}

/* The console's reset (psyq_internal.h): the PS1's RAM is cleared, the state with it. */
void psyq_c2_reset(void) {
    psyq_rand_state = 0;
}

/* A save state (psyq_internal.h): the generator's state. */
void psyq_c2_state(PortState *s) {
    PORT_STATE_VAR(s, psyq_rand_state);
}
