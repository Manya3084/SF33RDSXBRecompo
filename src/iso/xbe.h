#ifndef SF_XBE_H
#define SF_XBE_H

#include <stddef.h>

#define SF_SOUND_BANKS 21

/* Writes the game's 21 XACT sound banks (cue tables, "SDBK") from
 * sf33rd.xbe to out_dir as 00.sdbk to 20.sdbk: 00 is the shared bank
 * ("SE"), 01 to 20 the characters' ("pl00" to "pl19"). The banks are
 * found by their signature and named by their own header, so any release
 * of the disc works. Returns 0 when all 21 are written. */
int sf_xbe_sound_banks(const char *xbe_path, const char *out_dir, char *err, size_t err_n);

/* Writes sf33rd.xbe's sections as they load (each at its address, zeroed
 * past its file data) to out_path: data/xbe/image.bin. The game reads a
 * few tables past their ends, into what follows them in retail's image
 * (src/port/retail.c); a build of its own lays its globals out otherwise,
 * so those bytes come from here. "SF3IMG1", a count, then per section its
 * address, size and offset in the file, then the sections. 0 when written. */
int sf_xbe_image(const char *xbe_path, const char *out_path, char *err, size_t err_n);

#endif
