/* playing cards for the solitaire modules (KLONDIKE, SPIDER): include cards.c from one module source
 * (after game.c: it draws through gm_pix), this header from the others.
 * A card is one byte: rank 1..13 (A..K), suit 0..3 (spades, hearts, clubs, diamonds: odd = red),
 * CARD_UP = face up. 0 = no card. Own tiny glyphs: 3x5 ranks ("10" 5x5), 5x5 suits. */
#ifndef ZOS_CARDS_H
#define ZOS_CARDS_H
#include <stdint.h>

#define CARD(rank, suit) ((uint8_t)((rank) | ((suit) << 4)))
#define CARD_RANK(c) ((c) & 0x0F)
#define CARD_SUIT(c) (((c) >> 4) & 3)
#define CARD_RED(c)  ((c) & 0x10)
#define CARD_UP      0x80

#define CD_SEL 1                                  /* picked up: tinted face */
#define CD_CUR 2                                  /* cursor: yellow frame */
#define CARD_TABLE RGB(0, 92, 48)                 /* the green table */

/* draws the top vh rows of a w x ch card (vh == ch: the whole card, with the big centre suit) */
void card_draw(uint8_t x, uint8_t y, uint8_t w, uint8_t ch, uint8_t vh, uint8_t card, uint8_t fl);
/* empty pile outline, suit 0..3 shown faintly inside (foundations) or 0xFF */
void card_slot(uint8_t x, uint8_t y, uint8_t w, uint8_t ch, uint8_t suit, uint8_t fl);
/* offsets of nd face-down + nu face-up cards so that the column fits in `avail` rows:
 * face down *dd (2..3) apart, face up *du (3..7) apart */
void card_layout(uint8_t nd, uint8_t nu, uint8_t avail, uint8_t ch, uint8_t *dd, uint8_t *du);
#endif
