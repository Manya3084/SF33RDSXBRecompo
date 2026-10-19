/* Xbox Live, offline. The port has no Live; the game asks about it at a
 * few points, and these give it the offline answers.
 *
 * Before each match retail runs a settings exchange (Game01 in Game.c:
 * xb_NetSync_Step, xb_NetSync_CheckPlayers) even offline, where both ends
 * are this console: it packs the local settings, parses them back, and
 * writes them to save_w[2]. The parsing is Live code the decomp has no C
 * for (xb_NetSync_ParsePacket), so the values it writes offline are not
 * known from the code; with both ends local they are the settings save_w
 * already holds, and here the exchange finishes at once and leaves them.
 * Whether it also changes permission_player offline is unconfirmed. */

#include "types.h"

static s32 s_sync_done;

s32 xb_GetOnlineFlag(void)
{
    return 0;
}

s32 xb_NetSync_IsDone(void)
{
    return s_sync_done;
}

void xb_NetSync_SetDone(s32 v)
{
    s_sync_done = v;
}

void xb_NetSync_Step(void)
{
}

s32 xb_NetSync_CheckPlayers(void)
{
    return 1;
}
