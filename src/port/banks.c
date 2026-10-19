/* The sound bank buffers the game loads its banks into (color3rd.c's
 * init_trans_color_ram), at retail's sizes: three bank slots of 0x8E590
 * bytes, and the three resident wave banks retail registers with XACT
 * (3s-decomp-ogxb, Source/XBOX/xb_0F4537.c, xb_LoadSoundBanks). The
 * native sound code reads them from here. */

#include "types.h"

u8 D_009CA4C0[3 * 0x8E590] __attribute__((aligned(16)));
u8 D_007C5CE0[0xBB6FC] __attribute__((aligned(16)));
u8 D_00881400[0x1490B4] __attribute__((aligned(16)));
u8 D_00B84200[0xC7CC4] __attribute__((aligned(16)));
