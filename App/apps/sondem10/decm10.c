/* M10 soft-decision decoder derived from the Sonde M20 acquisition model.
 *
 * M10 uses differential Manchester at about 9615 chips/s. The 19.2 kHz ADC
 * rate gives just under two samples per chip; the edge DPLL tracks the small
 * rate difference. Standard 0x9F frames and their optional 0x76-byte form are
 * accepted. M10+ (0xAF) and the periodic 0x49 auxiliary frame are ignored.
 */
#include "decm10.h"

#define FS       19200u
#define SPB_Q8   ((int32_t)((FS << 8) / M10_BAUD))
#define MEAN_SH  6
#define CHIP_MSK ((uint8_t)(M10_CHIP_N - 1u))
enum { ST_SRCH = 0, ST_DATA = 1, ST_DONE = 2 };

#define RAW_HDR 0x99994C99u

static uint16_t checkM10(const uint8_t *msg, uint8_t len)
{
    uint16_t c = 0;
    for (uint8_t i = 0; i < len; i++) {
        uint8_t b = msg[i];
        b = (uint8_t)((b >> 1) | (b << 7));
        b ^= (uint8_t)((b >> 2) & 0xFFu);
        uint8_t t6 = (uint8_t)((c ^ (c >> 2) ^ (c >> 4)) & 1u);
        uint8_t t7 = (uint8_t)(((c >> 1) ^ (c >> 3) ^ (c >> 5)) & 1u);
        uint8_t t = (uint8_t)((c & 0x3Fu) | (t6 << 6) | (t7 << 7));
        uint8_t s = (uint8_t)((c >> 7) & 0xFFu);
        s ^= (uint8_t)((s >> 2) & 0xFFu);
        c = (uint16_t)(((c & 0xFFu) << 8) | (uint8_t)(b ^ t ^ s));
    }
    return c;
}

static uint32_t be(const uint8_t *p, uint8_t n)
{
    uint32_t v = 0;
    while (n--) v = (v << 8) | *p++;
    return v;
}

/* Return the high word of a*b without requiring a 64-bit multiply helper. */
static uint32_t mulHi32(uint32_t a, uint32_t b)
{
    uint32_t ah = a >> 16, al = a & 0xFFFFu;
    uint32_t bh = b >> 16, bl = b & 0xFFFFu;
    uint32_t t = al * bl;
    uint32_t carry = t >> 16;
    t = ah * bl + carry;
    uint32_t mid = t & 0xFFFFu;
    uint32_t high = t >> 16;
    t = al * bh + mid;
    return ah * bh + high + (t >> 16);
}

static int32_t binaryAngleToArcSec(const uint8_t *p)
{
    int32_t v = (int32_t)be(p, 4);
    uint32_t neg = v < 0;
    uint32_t u = (uint32_t)v;
    if (neg) u = 0u - u;
    uint32_t arc = mulHi32(u, 1296000u);
    return neg ? -(int32_t)arc : (int32_t)arc;
}

static int16_t softOf(int32_t integ)
{
    int32_t v = integ >> 8;
    if (v > 32767) v = 32767;
    if (v < -32767) v = -32767;
    return (int16_t)v;
}

static bool hdrOk(const int16_t *ch, uint8_t base)
{
    int32_t s = 0;
    uint32_t e = 0;
    for (uint8_t i = 0; i < 32u; i++) {
        int32_t c = ch[(uint8_t)(base + i) & CHIP_MSK];
        e += (uint32_t)(c >= 0 ? c : -c);
        if ((RAW_HDR >> (31u - i)) & 1u) s += c;
        else s -= c;
    }
    if (s < 0) s = -s;
    return e > 120u && (uint32_t)s * 5u > e * 3u;
}

static uint16_t softWord(const int16_t *ch, uint8_t base, uint8_t *prev_out)
{
    uint16_t w = 0;
    uint8_t prev = 0;
    for (uint8_t i = 0; i < 16u; i++) {
        int32_t s1 = ch[(uint8_t)(base + i * 2u) & CHIP_MSK];
        int32_t s2 = ch[(uint8_t)(base + i * 2u + 1u) & CHIP_MSK];
        uint8_t man = (s2 >= s1) ? 1u : 0u;
        w = (uint16_t)((w << 1) | (prev == man ? 1u : 0u));
        prev = man;
    }
    *prev_out = prev;
    return w;
}

static void putBit(decm10_t *d, uint8_t bit)
{
    if (d->nbit >= d->needBits) return;
    if (!(d->nbit & 7u)) d->frm[d->nbit >> 3] = 0;
    if (bit) d->frm[d->nbit >> 3] |= (uint8_t)(1u << (7u - (d->nbit & 7u)));
    d->nbit++;
}

static bool supportedHeader(uint16_t w)
{
    uint8_t len = (uint8_t)(w >> 8);
    return (len == 0x64u || len == 0x66u || len == 0x76u) &&
           (uint8_t)w == 0x9Fu;
}

static void emitChip(decm10_t *d, int16_t soft)
{
    if (d->state == ST_SRCH) {
        uint8_t prev;
        uint16_t w;
        d->chips[d->cpos] = soft;
        d->cpos = (uint8_t)((d->cpos + 1u) & CHIP_MSK);
        if (d->nfill < 64u) { d->nfill++; return; }
        if (!hdrOk(d->chips, d->cpos)) return;
        w = softWord(d->chips, (uint8_t)(d->cpos + 32u) & CHIP_MSK, &prev);
        d->inv = (uint8_t)(w >> 15);
        w &= 0x7FFFu;
        if (!supportedHeader(w)) return;
        d->frm[0] = (uint8_t)(w >> 8);
        d->frm[1] = (uint8_t)w;
        d->nbit = 16u;
        d->needBits = (uint16_t)((d->frm[0] + 1u) * 8u);
        d->prev = (uint8_t)(prev ^ d->inv);
        d->state = ST_DATA;
        return;
    }
    if (++d->guard > (uint16_t)(d->needBits * 3u + 64u)) {
        d->state = ST_DONE;
        return;
    }
    {
        int32_t c = d->inv ? -(int32_t)soft : (int32_t)soft;
        uint8_t man;
        if (!d->half) { d->h1 = c; d->half = 1u; return; }
        d->half = 0;
        man = (c >= d->h1) ? 1u : 0u;
        putBit(d, d->prev == man ? 1u : 0u);
        d->prev = man;
        if (d->nbit >= d->needBits) d->state = ST_DONE;
    }
}

void decm10_init(decm10_t *d)
{
    uint8_t *p = (uint8_t *)d;
    for (uint16_t i = 0; i < sizeof *d; i++) p[i] = 0;
    d->meanQ8 = -1;
    d->ph = SPB_Q8 / 2;
}

void decm10_rearm(decm10_t *d)
{
    int32_t mean = d->meanQ8;
    decm10_init(d);
    d->meanQ8 = mean;
}

bool decm10_busy(const decm10_t *d) { return d->state == ST_DATA; }

bool decm10_push(decm10_t *d, uint16_t sample)
{
    int32_t xq, e;
    uint8_t s;

    if (d->state == ST_DONE) return true;
    xq = (int32_t)sample << 8;
    if (d->meanQ8 < 0) d->meanQ8 = xq;
    e = xq - d->meanQ8;
    if (d->state != ST_DATA)
        d->meanQ8 += (e + (1 << (MEAN_SH - 1))) >> MEAN_SH;
    s = e > 0 ? 1u : 0u;
    if (s != d->sign) {
        uint32_t a = (uint32_t)(d->pe < 0 ? -d->pe : d->pe);
        uint32_t b = (uint32_t)(e < 0 ? -e : e);
        int32_t err = (int32_t)((a << 8) / (a + b + 1u)) - 128 - d->ph;
        if (err < -(SPB_Q8 / 2)) err += SPB_Q8;
        d->ph += err >> 3;
        d->sign = s;
    }
    d->pe = e;
    if (d->ph < 256) {
        int32_t part = (e * (d->ph < 0 ? 0 : d->ph)) >> 8;
        int16_t chip = softOf(d->integ + part);
        d->integ = e - part;
        d->ph += SPB_Q8 - 256;
        emitChip(d, chip);
        return d->state == ST_DONE;
    }
    d->integ += e;
    d->ph -= 256;
    return false;
}

static char hexDigit(uint8_t v)
{
    if (v < 10u) return (char)('0' + (int)v);
    return (char)('A' + (int)v - 10);
}

static char *digits(char *o, uint32_t v, uint32_t div)
{
    for (; div; div /= 10u) *o++ = (char)('0' + (v / div) % 10u);
    return o;
}

bool decm10_parse(decm10_t *d, sonde_info_t *out)
{
    const uint8_t *f = d->frm;
    uint8_t pc = (uint8_t)(f[0] - 1u);
    int32_t alt;

    if ((f[0] != 0x64u && f[0] != 0x66u && f[0] != 0x76u) ||
        f[1] != 0x9Fu || pc >= M10_FRAME_LEN - 1u ||
        be(f + pc, 2) != checkM10(f, pc))
        return false;

    {
        uint8_t b = f[0x5Fu];
        uint16_t n = (uint16_t)(f[0x60] | ((uint16_t)f[0x61] << 8));
        char *o = out->id;
        *o++ = hexDigit((uint8_t)(b >> 4));
        o = digits(o, b & 0x0Fu, 10u); *o++ = '-';
        *o++ = hexDigit(f[0x5Du] & 0x0Fu); *o++ = '-';
        *o++ = (char)('0' + ((n >> 13) & 7u));
        o = digits(o, n & 0x1FFFu, 1000u); *o = 0;
    }
    out->flags = SONDE_HAS_ID;
    {
        uint8_t sc = f[0x3Eu];
        uint16_t adc = (uint16_t)(f[0x3Fu] | ((uint16_t)f[0x40u] << 8));
        if (sc < 3u && adc > 0xA000u && adc <= 0xAFFFu)
            out->tRaw = (uint16_t)((adc - 0xA000u) | ((uint16_t)sc << 12));
        else
            out->tRaw = 0;
    }
    if (f[2] != 0x20u || (f[0x1Du] & 1u)) return true;

    out->latS = binaryAngleToArcSec(f + 0x0Eu);
    out->lonS = binaryAngleToArcSec(f + 0x12u);
    alt = (int32_t)be(f + 0x16u, 4);
    out->alt_m = alt / 1000;
    out->flags |= SONDE_HAS_ALT;
    if (out->latS || out->lonS) out->flags |= SONDE_HAS_POS;
    return true;
}
