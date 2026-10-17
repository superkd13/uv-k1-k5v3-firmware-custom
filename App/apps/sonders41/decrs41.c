/* Vaisala RS41 decoder.
 *
 * RS41 sends 4800 bit/s NRZ/GFSK, least-significant bit first. The standard
 * burst is 320 bytes including the 8-byte sync. Bytes after reception are
 * de-whitened with the protocol's repeating 64-byte mask. This compact first
 * version validates the STATUS and GPS sub-block CRCs but deliberately omits
 * Reed-Solomon correction to stay inside the 4 KiB overlay.
 *
 * The RAW audio path cuts the low frequencies (corner near 1 kHz, as found
 * with POCSAG RX): a high-passed NRZ level droops back to zero within a few
 * bits, so a level (sign) decision fails on the runs of the sync word. Each
 * slicer instead flips its level when the signal passes +/- num/8 of its
 * peak and holds it in between. Two slicers run, at 2/8 and 4/8: in the
 * channel model (test/model_rx.py) they cover a corner from 30 Hz to 1.5 kHz
 * between them, and those two are the ones that read POCSAG on the radio.
 * Each slicer has its own DPLL, pulled by its level changes, and looks for
 * the 64-bit sync word on hard bits (either polarity, a few wrong bits
 * allowed): a click or the start of a carrier can no longer pass for a sync,
 * which held the decoder busy and the keys unread for half a second each.
 */
#include "decrs41.h"

#define FS        19200u
#define STEP      ((int32_t)((65536u * RS41_BAUD) / FS))  /* 4 samples per bit */
/* A level change pulls the phase toward 3/4: the bit is read one sample
 * after the change the loop locks on (best phase in the model sweep). */
#define PLL_CTR   49152
#define PLL_SHIFT 2
#define DC_SH     8
#define AMP_DECAY 8
enum { ST_SRCH = 0, ST_DATA = 1, ST_DONE = 2 };

/* On-air sync 10 B6 CA 11 22 96 12 F8, first bit in bit 0. */
#define SYNC0 0x11CAB610u
#define SYNC1 0xF8129622u
/* At most RS41_SYNC_ERR bits set in the 64 bits a:b. */
static bool near64(uint32_t a, uint32_t b)
{
    uint8_t n = 0;
    while (a) { a &= a - 1u; if (++n > RS41_SYNC_ERR) return false; }
    while (b) { b &= b - 1u; if (++n > RS41_SYNC_ERR) return false; }
    return true;
}

static void slRearm(rs41sl_t *s)
{
    s->state = ST_SRCH;
    s->nbit = 0;
}

/* One bit from a slicer; true when its frame is complete. */
static bool slBit(rs41sl_t *s, uint8_t bit)
{
    if (s->state == ST_SRCH) {
        uint32_t x0, x1;
        s->sr0 = (s->sr0 >> 1) | (s->sr1 << 31);
        s->sr1 = (s->sr1 >> 1) | ((uint32_t)bit << 31);
        if (s->nbit < 64u) { s->nbit++; return false; }
        x0 = s->sr0 ^ SYNC0;
        x1 = s->sr1 ^ SYNC1;
        if (near64(x0, x1)) s->inv = 0;
        else if (near64(~x0, ~x1)) s->inv = 1u;
        else return false;
        s->state = ST_DATA;            /* nbit = 64: the data start at byte 8 */
        return false;
    }
    bit ^= s->inv;
    if (!(s->nbit & 7u)) s->frm[s->nbit >> 3] = 0;
    if (bit) s->frm[s->nbit >> 3] |= (uint8_t)(1u << (s->nbit & 7u));
    if (++s->nbit < RS41_FRAME_LEN * 8u) return false;
    s->state = ST_DONE;
    return true;
}

void decrs41_init(decrs41_t *d)
{
    uint8_t *p = (uint8_t *)d;
    for (uint16_t i = 0; i < sizeof *d; i++) p[i] = 0;
    d->dcQ8 = -1;
    for (uint8_t k = 0; k < RS41_NSL; k++) d->sl[k].num = (uint8_t)(2u << k);
}

void decrs41_rearm(decrs41_t *d)
{
    for (uint8_t k = 0; k < RS41_NSL; k++) slRearm(&d->sl[k]);
}

bool decrs41_busy(const decrs41_t *d)
{
    for (uint8_t k = 0; k < RS41_NSL; k++)
        if (d->sl[k].state == ST_DATA) return true;
    return false;
}

bool decrs41_push(decrs41_t *d, uint16_t sample)
{
    int32_t xq = (int32_t)sample << 8, y, a;
    bool done = false;
    if (d->dcQ8 < 0) d->dcQ8 = xq;
    d->dcQ8 += (xq - d->dcQ8) >> DC_SH;
    y = xq - d->dcQ8;
    a = y < 0 ? -y : y;
    if (a > d->amp) d->amp += (a - d->amp) >> 2;
    else d->amp -= d->amp >> AMP_DECAY;
    for (uint8_t k = 0; k < RS41_NSL; k++) {
        rs41sl_t *s = &d->sl[k];
        int32_t t;
        uint8_t lev;
        if (s->state == ST_DONE) { done = true; continue; }
        t = (d->amp >> 3) * s->num;
        lev = s->lev;                  /* holds between the thresholds */
        if (y > t) lev = 1u;
        else if (y < -t) lev = 0;
        s->phase += STEP;
        if (lev != s->lev) {
            int32_t err = PLL_CTR - s->phase;
            if (err > 32768) err -= 65536;
            s->phase += err >> PLL_SHIFT;
            s->lev = lev;
        }
        if (s->phase < 65536) continue;
        s->phase -= 65536;
        if (slBit(s, lev)) done = true;
    }
    return done;
}

static uint16_t crc16(const uint8_t *p, uint8_t len)
{
    uint16_t crc = 0xFFFFu;
    while (len--) {
        crc ^= (uint16_t)*p++ << 8;
        for (uint8_t b = 0; b < 8u; b++) {
            bool high = (crc & 0x8000u) != 0u;
            crc = (uint16_t)(crc << 1);
            if (high) crc = (uint16_t)(crc ^ (uint16_t)0x1021u);
        }
    }
    return crc;
}

static bool blockOk(const uint8_t *f, uint16_t pos, uint8_t id, uint8_t minLen)
{
    uint8_t len;
    uint16_t end, got;
    if (pos + 4u > RS41_FRAME_LEN || f[pos] != id) return false;
    len = f[pos + 1u];
    end = (uint16_t)(pos + 2u + len);
    if (len < minLen || end + 2u > RS41_FRAME_LEN) return false;
    got = (uint16_t)(f[end] | ((uint16_t)f[end + 1u] << 8));
    return crc16(f + pos + 2u, len) == got;
}

static int32_t le32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static int32_t le16(const uint8_t *p)
{
    return (int16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t isqrt32(uint32_t n)
{
    uint32_t root = 0, bit = 1u << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) { n -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return root;
}

static uint64_t square32(int32_t v)
{
    uint32_t a = (uint32_t)v;
    if (v < 0) a = 0u - a;
    uint32_t lo = a & 0xFFFFu, hi = a >> 16;
    return (uint64_t)(lo * lo) + ((uint64_t)(lo * hi) << 17) +
           ((uint64_t)(hi * hi) << 32);
}

static uint32_t isqrt64(uint64_t n)
{
    uint64_t root = 0, bit = (uint64_t)1u << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) { n -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return (uint32_t)root;
}

/* CORDIC atan2, result in arc-seconds; arc = atan(2^-i) (from the assets). */
static int32_t atan2Arc(int32_t y, int32_t x, const uint32_t *arc)
{
    int32_t angle = 0;
    if (!x && !y) return 0;
    if (x < 0) {
        angle = y >= 0 ? 648000 : -648000;
        x = -x;
        y = -y;
    }
    for (uint8_t i = 0; i < RS41_ARC_N; i++) {
        int32_t ox = x;
        if (y > 0) {
            x += y >> i;
            y -= ox >> i;
            angle += (int32_t)arc[i];
        } else {
            x -= y >> i;
            y += ox >> i;
            angle -= (int32_t)arc[i];
        }
    }
    return angle;
}

static bool ecef(const uint8_t *p, sonde_info_t *out, const uint32_t *arc)
{
    int32_t x = le32(p), y = le32(p + 4), z = le32(p + 8);
    int32_t x100 = x / 10000, y100 = y / 10000, z100 = z / 10000;
    uint32_t ax100 = (uint32_t)x100, ay100 = (uint32_t)y100;
    uint32_t horizontal;
    uint32_t radiusCm = isqrt64(square32(x) + square32(y) + square32(z));
    uint32_t radiusM = radiusCm / 100u;
    uint32_t q, corr;
    int32_t alt;
    if (x100 < 0) ax100 = 0u - ax100;
    if (y100 < 0) ay100 = 0u - ay100;
    horizontal = isqrt32(ax100 * ax100 + ay100 * ay100);
    if (radiusM < 6300000u || radiusM > 6450000u) return false;
    q = ((uint32_t)(z100 < 0 ? -z100 : z100) << 15) / (radiusM / 100u);
    corr = (21385u * q) >> 15;
    corr = (corr * q) >> 15;
    alt = (int32_t)radiusM - (int32_t)(6378137u - corr);
    if (alt < -1000 || alt > 50000) return false;
    out->lonS = atan2Arc(y100, x100, arc);
    out->latS = atan2Arc(z100, (int32_t)((horizontal * 9933u) / 10000u), arc);
    out->alt_m = alt;
    /* ECEF velocity (cm/s) projected on the local vertical: x100.v is in
     * 100 m.cm/s, so dividing by the radius in 10 m gives dm/s. It fits an
     * int32 below 330 m/s. */
    {
        int32_t dot = x100 * le16(p + 12) + y100 * le16(p + 14) + z100 * le16(p + 16);
        out->vs_dm = (int16_t)(dot / (int32_t)(radiusM / 10u));
    }
    out->sats = p[18];
    return true;
}

/* De-whiten one frame and read it; out is valid only on success. */
static bool frameOk(const decrs41_t *d, uint8_t *f, sonde_info_t *out)
{
    const uint16_t statusPos = 0x39u, gpsPos = 0x112u;
    for (uint16_t i = 8u; i < RS41_FRAME_LEN; i++) f[i] ^= d->mask[i & 63u];
    if (!blockOk(f, statusPos, 0x79u, 40u)) return false;
    for (uint8_t i = 0; i < 8u; i++) {
        uint8_t c = f[statusPos + 4u + i];
        if (c < 0x20u || c > 0x7Eu) return false;
        out->id[i] = (char)c;
    }
    out->id[8] = 0;
    out->flags = SONDE_HAS_ID;
    if (blockOk(f, gpsPos, 0x7Bu, 21u) && ecef(f + gpsPos + 2u, out, d->arc))
        out->flags |= SONDE_HAS_POS | SONDE_HAS_ALT;
    return true;
}

bool decrs41_parse(decrs41_t *d, sonde_info_t *out)
{
    for (uint8_t k = 0; k < RS41_NSL; k++) {
        rs41sl_t *s = &d->sl[k];
        if (s->state != ST_DONE) continue;
        /* The other slicer is reading the same burst: drop it on success. */
        if (frameOk(d, s->frm, out)) { decrs41_rearm(d); return true; }
        slRearm(s);
    }
    return false;
}
