/* examples/hello: a disc-free Psy-Q program on psxstack, the stack's own smoke test (tests/hello_test.py). It needs
 * no game and no disc: the recovered Psy-Q headers under include/psyq/ (the first game's, MIT), a common.h that
 * provides the type names and the host hooks, and this file. The build renames main() to game_main (the runtime
 * owns main). It draws two polygons that change with the frame and the pad, double-buffered as a PS1 program would,
 * initialises the sound library and sets the master volume (a key-on needs a VAB, which the example has none of). */
#include "common.h"
#include "psyq/libetc.h"
#include "psyq/libgpu.h"
#include "psyq/libpad.h"
#include "psyq/libsnd.h"

#define OT_LEN 8

static u32 hello_ot[2][OT_LEN];
static POLY_F4 hello_quad[2];
static POLY_G4 hello_grad[2];
static DISPENV hello_disp[2];
static DRAWENV hello_draw[2];
static u8 hello_pad0[34], hello_pad1[34];

int main(void) {
    int frame = 0, db = 0;

    ResetGraph(0);
    SetDefDispEnv(&hello_disp[0], 0, 0, 320, 240);
    SetDefDispEnv(&hello_disp[1], 0, 240, 320, 240);
    SetDefDrawEnv(&hello_draw[0], 0, 240, 320, 240);
    SetDefDrawEnv(&hello_draw[1], 0, 0, 320, 240);
    hello_draw[0].isbg = hello_draw[1].isbg = 1;
    setRGB0(&hello_draw[0], 20, 24, 48);
    setRGB0(&hello_draw[1], 20, 24, 48);
    SetDispMask(1);
    PadInitDirect(hello_pad0, hello_pad1);
    PadStartCom();
    SsInit();
    SsSetMVol(96, 96);
    for (;;) {
        u32 *ot = hello_ot[db];
        u16 buttons = (u16)~(((u16)hello_pad0[2] << 8) | hello_pad0[3]); /* active high: a pressed button tints */
        POLY_F4 *q = &hello_quad[db];
        POLY_G4 *g = &hello_grad[db];

        ClearOTagR(ot, OT_LEN);
        setPolyF4(q);
        setRGB0(q, 200, (u8)(frame * 3), (u8)(60 + (buttons & 0xFF)));
        setXY4(q, 40, 40, 160, 40, 40, 160, 160, 160);
        addPrim(ot + 1, q);
        setPolyG4(g);
        setRGB0(g, 255, 0, 0);
        setRGB1(g, 0, 255, 0);
        setRGB2(g, 0, 0, 255);
        setRGB3(g, (u8)(frame * 5), 255, 255);
        setXY4(g, 190, 50, 300, 70, 180, 180, 290, 200);
        addPrim(ot + 2, g);
        SetDrawEnv(&hello_draw[db].dr_env, &hello_draw[db]);
        addPrim(ot + OT_LEN - 1, &hello_draw[db].dr_env);
        DrawSync(0);
        VSync(0);
        PutDispEnv(&hello_disp[db]);
        DrawOTag(ot + OT_LEN - 1);
        db ^= 1;
        frame++;
    }
}
