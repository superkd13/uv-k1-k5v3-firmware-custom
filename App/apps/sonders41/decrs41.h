#ifndef DECRS41_H
#define DECRS41_H
#include <stdint.h>
#include <stdbool.h>
#include "sonde_info.h"

#define RS41_FRAME_LEN 320u
#define RS41_BAUD      4800u
#define RS41_NSL       2u      /* hysteresis slicers, thresholds 2/8 and 4/8 */
#define RS41_SYNC_ERR  6u      /* wrong bits tolerated in the 64-bit sync word */
#define RS41_ARC_N     14u     /* CORDIC steps */

typedef struct {
    int32_t  phase;            /* DPLL phase, 65536 per bit */
    uint32_t sr0, sr1;         /* last 64 bits, the oldest in bit 0 of sr0 */
    uint16_t nbit;
    uint8_t  state, inv, lev, num;
    uint8_t  frm[RS41_FRAME_LEN];
} rs41sl_t;

typedef struct {
    int32_t  dcQ8, amp;        /* DC level and peak of |y|, both Q8 */
    rs41sl_t sl[RS41_NSL];
    /* Constant tables kept out of the 4 KiB overlay: the caller fills them
     * after decrs41_init (the app copies them from its assets in one read,
     * arc then mask, contiguous). */
    uint32_t arc[RS41_ARC_N];  /* atan(2^-i) in arc-seconds */
    uint8_t  mask[64];         /* de-whitening mask */
} decrs41_t;

void decrs41_init(decrs41_t *d);
void decrs41_rearm(decrs41_t *d);
bool decrs41_busy(const decrs41_t *d);
/* True when a slicer holds a complete frame: call decrs41_parse. */
bool decrs41_push(decrs41_t *d, uint16_t sample);
/* CRC-checked STATUS and GPS blocks; no Reed-Solomon correction in v0.1.
 * Rearms the slicers it consumed (all of them on success). */
bool decrs41_parse(decrs41_t *d, sonde_info_t *out);
#endif
