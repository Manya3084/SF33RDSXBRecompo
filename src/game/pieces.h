#ifndef SF_PIECES_H
#define SF_PIECES_H

#include <stddef.h>
#include <stdint.h>

/* effect_48 type 0 is the slogan. effect_F6 type 0 is the first sketch.
   effect_F6 type 1 is the same cell, baked at y -224 and scale 128/64.
   sf_pieces_compose places the sketch at shift_y and the slogan at shift_x.
   Type 1 uses the sketch slot with the slogan pointer null.
   Scene 102 draws effect_F6 type 2 and the 13 effect_48 type 1 letters.
   sf_pieces_line_compose slides the sketch by shift_x and the letters by shift_y. */

enum {
    SF_PIECE_W = 384,
    SF_PIECE_H = 224
};

int sf_pieces_linear(const uint8_t *bg, uint32_t bg_size, int code, uint8_t *dst, int dst_n, int *side);
int sf_pieces_layers(const uint8_t *bg, uint32_t bg_size, const uint8_t *title, uint32_t title_size, uint8_t *sketch, uint8_t *slogan, char *err, size_t err_n);
int sf_pieces_next(const uint8_t *bg, uint32_t bg_size, const uint8_t *title, uint32_t title_size, uint8_t *rgba, char *err, size_t err_n);
int sf_pieces_line(const uint8_t *bg, uint32_t bg_size, const uint8_t *title, uint32_t title_size, uint8_t *art, uint8_t *words, char *err, size_t err_n);
void sf_pieces_line_compose(const uint8_t *art, const uint8_t *words, int shift_x, int shift_y, uint8_t *rgba);
void sf_pieces_compose(const uint8_t *sketch, const uint8_t *slogan, int shift_x, int shift_y, uint8_t *rgba);
void sf_pieces_overlay(const uint8_t *sketch, const uint8_t *slogan, int shift_x, int shift_y, uint8_t *rgba);
int sf_slogan_shift(int32_t layer_x);
int sf_ground_load(const char *install, uint8_t **left, uint8_t **right, char *err, size_t err_n);
void sf_ground_compose(const uint8_t *left, const uint8_t *right, int off_y, uint8_t *rgba);
int sf_pieces_view(const uint8_t *bg, uint32_t bg_size, const uint8_t *title, uint32_t title_size, uint8_t *rgba, char *err, size_t err_n);
int sf_pieces_load(const char *install, uint8_t **sketch, uint8_t **slogan, uint8_t **next, char *err, size_t err_n);

#endif
