#ifndef PSYQ_LIBC2_H
#define PSYQ_LIBC2_H

/* psxstack's declarations of the Psy-Q 4.7 LIBC2 interface, as its games use it: written from the games' use of the
 * API and public documentation, no Sony header (DECISIONS "Psy-Q declarations: the stack's"). The shim implements
 * these; a game's own recovered declarations must agree with them in ABI (tools/psyq_decls.py). */

#include "psxstack/types.h"

/* A game's memcpy calls are GCC's built-in (its argument evaluation order), which needs a prototype compatible with
 * the built-in: an unsigned length. */

s32 strlen(const char *s);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, s32 n);
void *memcpy(void *dst, const void *src, u32 n);
s32 strcspn(const char *s, const char *reject);
s32 atoi(const char *s);

#endif /* PSYQ_LIBC2_H */
