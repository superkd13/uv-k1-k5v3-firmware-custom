/* Common decoded radiosonde fields (RS41 app). */
#ifndef SONDE_INFO_H
#define SONDE_INFO_H
#include <stdint.h>

typedef struct {
    char id[12];
    int32_t latS, lonS, alt_m;
    int16_t vs_dm;          /* vertical speed, dm/s (with SONDE_HAS_POS) */
    uint8_t flags;
    uint8_t sats;           /* GPS satellites used (with SONDE_HAS_POS) */
} sonde_info_t;

enum {
    SONDE_HAS_POS = 1u,
    SONDE_HAS_ALT = 2u,
    SONDE_HAS_ID  = 32u
};
#endif
