/* examples/hello: the game-side header the recovered Psy-Q headers include (`#include "common.h"`): the PS1-style
 * type names (psxstack/types.h defines the same under the same guard) and, as the first game's common.h does, the
 * hook macros for the host (psxstack/hooks.h: the recovered libgpu.h's setaddr uses PTR_TO_U32). hello is host-only,
 * so there is no PS1 side. */
#ifndef COMMON_H
#define COMMON_H

#include "psxstack/types.h"

#ifndef NULL
#define NULL 0
#endif

#include "psxstack/hooks.h"

#endif /* COMMON_H */
