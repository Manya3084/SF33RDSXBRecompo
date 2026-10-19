#ifndef SF_PIECES_H
#define SF_PIECES_H

#include <stddef.h>
#include <stdint.h>

/* The hold at trigger 101. effect_48 type 0 is the slogan (21 cells).
   effect_F6 type 0 is the sketch. Both use texture group 0x8C40 in
   bg20.bin. Type 1 is initialized on this beat, but its old_rno[2] is 1
   and the opening index is still 0, so it is not drawn yet.
   Layer 2 keeps walking, so the slogan moves right by sf_slogan_shift.
   The sketch stays on layer 1. */

enum {
    SF_PIECE_W = 384,
    SF_PIECE_H = 224
};

int sf_pieces_linear(const uint8_t *bg, uint32_t bg_size, int code, uint8_t *dst, int dst_n, int *side);
int sf_pieces_layers(const uint8_t *bg, uint32_t bg_size, const uint8_t *title, uint32_t title_size, uint8_t *sketch, uint8_t *slogan, char *err, size_t err_n);
void sf_pieces_compose(const uint8_t *sketch, const uint8_t *slogan, int shift, uint8_t *rgba);
int sf_slogan_shift(int32_t layer_x);
int sf_pieces_view(const uint8_t *bg, uint32_t bg_size, const uint8_t *title, uint32_t title_size, uint8_t *rgba, char *err, size_t err_n);
int sf_pieces_load(const char *install, uint8_t **sketch, uint8_t **slogan, char *err, size_t err_n);

#endif
