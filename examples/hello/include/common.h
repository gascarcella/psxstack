/* examples/hello: the game-side header, what a game's common.h is: the PS1-style type names (psxstack/types.h
 * defines the same under the same guard) and the hook macros for the host (psxstack/hooks.h). hello is host-only, so
 * there is no PS1 side; its Psy-Q declarations are the stack's (include/psxstack/psyq/). */
#ifndef COMMON_H
#define COMMON_H

#include "psxstack/types.h"

#ifndef NULL
#define NULL 0
#endif

#include "psxstack/hooks.h"

#endif /* COMMON_H */
