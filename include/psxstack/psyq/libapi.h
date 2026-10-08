#ifndef PSYQ_LIBAPI_H
#define PSYQ_LIBAPI_H

/* psxstack's declarations of the Psy-Q 4.7 LIBAPI interface, as its games use it: written from the games' use of the
 * API and public documentation, no Sony header (DECISIONS "Psy-Q declarations: the stack's"). The shim implements
 * these; a game's own recovered declarations must agree with them in ABI (tools/psyq_decls.py). */

#include "psxstack/types.h"

s32 open(char *name, s32 mode);
s32 read(s32 fd, void *buf, s32 n);
s32 write(s32 fd, void *buf, s32 n);
s32 close(s32 fd);

#endif /* PSYQ_LIBAPI_H */
