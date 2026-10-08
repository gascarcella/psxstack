#ifndef PSYQ_LIBETC_H
#define PSYQ_LIBETC_H

/* psxstack's declarations of the Psy-Q 4.7 LIBETC interface, as its games use it: written from the games' use of the
 * API and public documentation, no Sony header (DECISIONS "Psy-Q declarations: the stack's"). The shim implements
 * these; a game's own recovered declarations must agree with them in ABI (tools/psyq_decls.py). */

#include "psxstack/types.h"

#define MODE_NTSC 0
#define MODE_PAL 1

int VSyncCallback(void (*f)(void));
int VSync(int mode);
int ResetCallback(void);
int SetVideoMode(int mode);

#endif /* PSYQ_LIBETC_H */
