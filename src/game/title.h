#ifndef SF_TITLE_H
#define SF_TITLE_H

#include <stdint.h>

/* Country is the byte at 0x7A8748. The retail image leaves it 0.
   TITLE_Init at 0x69786 loads archive file 0x54 when Country is 1 or 2,
   and file 0x53 otherwise. Those members are Title_R.ppg and TitleTM.ppg. */

extern uint8_t Country;

uint32_t sf_title_fnum(uint8_t country);

#endif
