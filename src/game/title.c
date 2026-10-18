#include "game/title.h"

uint8_t Country;

uint32_t sf_title_fnum(uint8_t country)
{
    if (country == 1 || country == 2) {
        return 0x54;
    }
    return 0x53;
}
