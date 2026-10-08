/* psxstack/psyq_names.h: the Psy-Q functions whose names are also the host's (libc's file calls; Win32's
 * EnterCriticalSection, in kernel32), sent to the shim's psyq_api_*. Forced into every game unit (cmake/psxstack.cmake:
 * -include) and included by the shim's psyq/libapi.c and psyq/libcard.c before their declarations, so a game's
 * open("bu00:...") reaches the shim's BIOS file calls (libcard.c) while the runtime, SDL and libc keep the host's open:
 * a definition named `open` in the executable would replace libc's for the whole process, and one named
 * EnterCriticalSection clashes with kernel32's at the Windows link (docs/PORT.md "The Psy-Q shim"; DECISIONS "LIBAPI's
 * clashing names: renamed in the game's units").
 *
 * Object-like macros: every use of the name in a game unit is renamed, the calls, the prototypes and the address, and
 * also a struct member or a variable of that name (the first game's cdload_reader.read), consistently in every unit,
 * so the layouts do not change. A game unit that also includes the host's <unistd.h> or <windows.h> would see
 * their declarations renamed too: none does (their prototypes clash with the PS1's). */
#ifndef PSXSTACK_PSYQ_NAMES_H
#define PSXSTACK_PSYQ_NAMES_H

#define open psyq_api_open
#define read psyq_api_read
#define write psyq_api_write
#define lseek psyq_api_lseek
#define close psyq_api_close
#define EnterCriticalSection psyq_api_EnterCriticalSection
#define ExitCriticalSection psyq_api_ExitCriticalSection

#endif /* PSXSTACK_PSYQ_NAMES_H */
