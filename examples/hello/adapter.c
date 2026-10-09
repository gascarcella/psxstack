/* examples/hello's adapter (psxstack/game.h): a 16-byte probe at a PS1 address of the example's own, outside its arena,
 * which a script's wait_mem reads (game_state_read) and its write_mem writes (game_state_host): tests/hello_test.py's
 * script check. Everything else keeps the runtime's defaults (runtime/game_defaults.c). */
#include <stdint.h>

#include "psxstack/game.h"

#define HELLO_PROBE_ADDR 0x80010000u

static uint8_t hello_probe[16];

static uint8_t *hello_probe_bytes(uint32_t addr, int size) {
    if ((size != 1 && size != 2 && size != 4) || addr < HELLO_PROBE_ADDR ||
        addr - HELLO_PROBE_ADDR > sizeof(hello_probe) - (uint32_t)size) {
        return 0;
    }
    return hello_probe + (addr - HELLO_PROBE_ADDR);
}

void *game_state_host(uint32_t addr, int size) {
    return hello_probe_bytes(addr, size);
}

int game_state_read(uint32_t addr, int size, int is_signed, int32_t *out) {
    const uint8_t *p = hello_probe_bytes(addr, size);
    uint32_t v = 0;
    int i;
    if (p == 0) {
        return 0;
    }
    for (i = 0; i < size; i++) {
        v |= (uint32_t)p[i] << (8 * i);
    }
    if (is_signed && size < 4 && (v & (1u << (8 * size - 1))) != 0) {
        v |= ~0u << (8 * size);
    }
    *out = (int32_t)v;
    return 1;
}
