/* Copyright 2026 Armel F4HWN
 * https://github.com/armel
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include "app/foxhunt.h"

#ifdef ENABLE_FEAT_F4HWN_FOXHUNT

#ifdef ENABLE_FEAT_F4HWN_K5VIEWER
#include "k5viewer.h"
#endif

// Signal window mapped onto the RSSI bar, in dBm.
// Roughly S0 (empty) to S9 + 40 dB (full), IARU VHF/UHF scale.
#define FOXHUNT_DBM_FLOOR (-141)
#define FOXHUNT_DBM_CEIL  (-53)

// Gauge geometry inside the main frame buffer (y in 0..55, status line excluded).
// 13 segments on the IARU S-meter scale: S1..S9 are 6 dB apart, the four marks
// past S9 (+10..+40) are 10 dB apart, so the axis compresses at the top.
#define FOXHUNT_BAR_X0     6    // leftmost segment x (centres the 13 segments)
#define FOXHUNT_SEG_COUNT  13   // S1..S9 + (+10..+40)
#define FOXHUNT_SEG_PITCH  9    // px between segment starts
#define FOXHUNT_SEG_W      8    // segment width
#define FOXHUNT_SEG_BOTTOM 37   // base the staircase grows up from

// Refresh period and trend sampling window.
#define FOXHUNT_TICK_MS      50
#define FOXHUNT_TREND_TICKS  20   // compare the level roughly once per second

// Geiger audio: the blip RATE tracks the signal level (faster = closer), the
// pitch rises too as a secondary cue. Below the silence floor, no blips at all.
#define FOXHUNT_AUDIO_SETTLE_MS 60  // audio amplifier warm-up after enabling the speaker
#define FOXHUNT_BLIP_MS         50  // tone duration per blip
#define FOXHUNT_RATE_SLOW_TICKS 20  // ticks between blips at the floor (~1/s)
#define FOXHUNT_RATE_FAST_TICKS 2   // ticks between blips at the ceiling (fast)
#define FOXHUNT_SILENCE_DBM     (-120) // stay silent below this level (noise floor)
#define FOXHUNT_TONE_MIN        400  // Hz at the floor of the window
#define FOXHUNT_TONE_MAX        2400 // Hz at the ceiling of the window

// Audio mode cycled by the 1 key: silent -> Geiger beep -> received station.
#define FOXHUNT_AUDIO_OFF       0
#define FOXHUNT_AUDIO_BEEP      1
#define FOXHUNT_AUDIO_STATION   2

// Attenuator via the BK4829 PGA (REG_13 bits 0..2) — the last gain stage before
// the RSSI detector, so it scales the reading linearly. Reducing the early LNA
// alone barely moved the reading on strong signals (mixer + PGA re-amplify).
// pgaTab (bk4829.c) = {-33,-27,-21,-15,-9,-6,-3,0}; index 7 (= 0 dB) matches the
// RX default (0x03DF), so step 0 leaves the gain untouched.
static const uint8_t FOXHUNT_ATT_PGA[4] = {7, 5, 3, 1};   // 0, -6, -15, -27 dB
static const uint8_t FOXHUNT_ATT_DB[4]  = {0, 6, 15, 27};

static KeyboardState kbd = {KEY_INVALID, KEY_INVALID, 0};

static bool    foxRunning;
static uint8_t foxAudioMode;
static uint8_t attStep;
static int16_t curDbm;
static int16_t peakDbm;
static int16_t trendRef;
static int16_t trendDelta;
static uint8_t trendTick;
static uint8_t audioTick;

static char str[16];

// Read the calibrated signal level of the current RX VFO, in dBm.
static int16_t FOXHUNT_ReadDbm(void)
{
    return BK4819_GetRSSI_dBm() + dBmCorrTable[gRxVfo->Band];
}

// Apply the selected attenuator step to the BK4829 PGA gain field.
static void FOXHUNT_ApplyAtt(void)
{
    uint16_t reg = BK4819_ReadRegister(BK4819_REG_13);
    reg = (reg & ~0x7u) | (uint16_t)FOXHUNT_ATT_PGA[attStep];   // PGA = REG_13 bits 0..2
    BK4819_WriteRegister(BK4819_REG_13, reg);
}

// Set the speaker/AF state for the current audio mode.
static void FOXHUNT_SetAudio(void)
{
    if (foxAudioMode == FOXHUNT_AUDIO_OFF) {
        AUDIO_AudioPathOff();
        return;
    }
    // Speaker on, let the amplifier warm up, RX audio muted for now. BEEP keeps it
    // muted (only the blips are heard); STATION un-mutes per level in the loop.
    AUDIO_AudioPathOn();
    SYSTEM_DelayMs(FOXHUNT_AUDIO_SETTLE_MS);
    BK4819_SetAF(BK4819_AF_MUTE);
}

// Map a dBm value to a Geiger tone frequency (higher signal, higher pitch).
static uint16_t FOXHUNT_DbmToTone(int16_t dbm)
{
    if (dbm <= FOXHUNT_DBM_FLOOR) return FOXHUNT_TONE_MIN;
    if (dbm >= FOXHUNT_DBM_CEIL)  return FOXHUNT_TONE_MAX;
    return FOXHUNT_TONE_MIN +
        (uint16_t)(((int32_t)(dbm - FOXHUNT_DBM_FLOOR) * (FOXHUNT_TONE_MAX - FOXHUNT_TONE_MIN)) /
                   (FOXHUNT_DBM_CEIL - FOXHUNT_DBM_FLOOR));
}

// Ticks between two blips: fewer ticks = faster clicking as the signal rises.
static uint8_t FOXHUNT_BlipPeriod(int16_t dbm)
{
    if (dbm <= FOXHUNT_DBM_FLOOR) return FOXHUNT_RATE_SLOW_TICKS;
    if (dbm >= FOXHUNT_DBM_CEIL)  return FOXHUNT_RATE_FAST_TICKS;
    return FOXHUNT_RATE_SLOW_TICKS -
        (uint8_t)(((int32_t)(dbm - FOXHUNT_DBM_FLOOR) * (FOXHUNT_RATE_SLOW_TICKS - FOXHUNT_RATE_FAST_TICKS)) /
                  (FOXHUNT_DBM_CEIL - FOXHUNT_DBM_FLOOR));
}

// Emit one short blip at the given pitch, then hand the chip back to RX.
// The speaker path is already on and settled (see the key-1 handler); between
// blips TurnsOffTones_TurnsOnRX mutes the AF, so the RX stays silent.
static void FOXHUNT_Blip(uint16_t freq)
{
    BK4819_PrepareToPlayTone(true);
    BK4819_PlayToneRaw(freq, FOXHUNT_BLIP_MS);
    BK4819_TurnsOffTones_TurnsOnRX();

    // TurnsOffTones_TurnsOnRX restores a normal RX front-end, so re-assert the
    // fixed gain and attenuator the level reading relies on.
    BK4819_SetAGC(false);
    FOXHUNT_ApplyAtt();
}

// Number of lit segments for a level, on the IARU S-meter scale:
// S1..S9 are 6 dB apart, the four marks past S9 (+10..+40) are 10 dB apart.
static uint8_t FOXHUNT_FillCount(int16_t dbm)
{
    int16_t n;
    if (dbm < -141) return 0;
    if (dbm <= -93)
        n = 1 + (dbm + 141) / 6;    // S1 (-141) .. S9 (-93)  -> 1..9
    else
        n = 9 + (dbm + 93) / 10;    // +10 (-83) .. +40 (-53) -> 10..13
    if (n > FOXHUNT_SEG_COUNT)
        n = FOXHUNT_SEG_COUNT;
    return (uint8_t)n;
}

// Build the IARU S-meter reading string (e.g. "S7" or "S9+30").
static void FOXHUNT_BuildS(char *out, int16_t dbm)
{
    if (dbm >= -93) {
        int16_t over = dbm - (-93);
        if (over > 40) over = 40;
        sprintf(out, "S9+%02d", over);
    } else if (dbm < -141) {
        sprintf(out, "S0");
    } else {
        sprintf(out, "S%d", (dbm + 147) / 6);
    }
}

// Fill a solid rectangle, column by column.
static void FOXHUNT_FillRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, bool black)
{
    for (int16_t x = x0; x <= x1; x++)
        UI_DrawLineBuffer(gFrameBuffer, x, y0, x, y1, black);
}

static void FOXHUNT_DrawBar(void)
{
    uint8_t nCur = FOXHUNT_FillCount(curDbm);
    uint8_t i;

    // Continuous S-meter staircase (no frame): each segment is 1 px taller than
    // the previous, from S1 (1 px) to S9+40 (13 px), growing up from the base.
    for (i = 0; i < nCur; i++) {
        int16_t sx = FOXHUNT_BAR_X0 + i * FOXHUNT_SEG_PITCH;
        FOXHUNT_FillRect(sx, FOXHUNT_SEG_BOTTOM - i, sx + FOXHUNT_SEG_W - 1, FOXHUNT_SEG_BOTTOM, true);
    }

    // Solid separator line between the staircase and the scale labels below,
    // with a 1 px gap above (y38) and below (y40) it.
    UI_DrawLineBuffer(gFrameBuffer, FOXHUNT_BAR_X0, FOXHUNT_SEG_BOTTOM + 2,
                      FOXHUNT_BAR_X0 + (FOXHUNT_SEG_COUNT - 1) * FOXHUNT_SEG_PITCH + FOXHUNT_SEG_W - 1,
                      FOXHUNT_SEG_BOTTOM + 2, true);
}

static void FOXHUNT_Draw(void)
{
    const uint8_t *trendIcon;
    char sMeter[8];
    char big[8];

    UI_DisplayClear();
    UI_StatusClear();

    // Title as an inverse label, in the scan-list tag style.
    GUI_DisplaySmallestInverse("FOX HUNT", 2, 0, true, true, 34);

    // Battery (icon + optional percentage/voltage) top-right, as on the main screens.
    unsigned int bx = LCD_WIDTH - sizeof(BITMAP_BatteryLevel1);
    UI_DrawBattery(gStatusLine + bx, gBatteryDisplayLevel, gLowBatteryBlink);
    if (gSetting_battery_text != 0) {
        if (gSetting_battery_text == 1) {      // voltage
            const uint16_t v = MIN(gBatteryVoltageAverage, 999);
            sprintf(str, "%u.%02u", v / 100, v % 100);
        } else {                               // percentage
            sprintf(str, "%02u%%", BATTERY_VoltsToPercent(gBatteryVoltageAverage));
        }
        bx -= 7 * strlen(str);
        UI_PrintStringSmallBufferNormal(str, gStatusLine + bx);
    }

    // Audio state icon between the label and the battery: speaker for beep,
    // headphones for station audio, nothing when silent.
    if (foxAudioMode == FOXHUNT_AUDIO_BEEP)
        memcpy(gStatusLine + 55, BITMAP_FoxHuntSignal, sizeof(BITMAP_FoxHuntSignal));
    else if (foxAudioMode == FOXHUNT_AUDIO_STATION)
        memcpy(gStatusLine + 55, BITMAP_FoxHuntSpeaker, sizeof(BITMAP_FoxHuntSpeaker));

    // Hero level, floating: the big number renders naturally (left-anchored) and
    // the "dBm" unit follows right after it, so the block flows with the width.
    sprintf(big, "%d", curDbm);
    UI_DisplayFrequency(big, 2, 0, false);
    UI_PrintStringSmallNormal("dBm", (uint8_t)(strlen(big) * 13 + 4), 0, 1);

    // Trend as an arrow on line 0 (up = nearer, down = farther, = stable),
    // right-anchored; the signed delta since ~1 s ago sits on line 1.
    trendIcon = (trendDelta > 0) ? BITMAP_FoxHuntUp
              : (trendDelta < 0) ? BITMAP_FoxHuntDown
              : BITMAP_FoxHuntFlat;
    memcpy(gFrameBuffer[0] + (126 - 11), trendIcon, 11);
    // Signed delta on line 1 — only when it actually moved (no "00 dBm" when
    // stable). "dBm" right-anchored to x126, the value 2 px to its left (a real
    // space would be 7 px), so they are drawn as two separate strings.
    if (trendDelta != 0) {
        sprintf(str, "%+03d", trendDelta);   // sign + 2 digits, e.g. "+05" / "-12"
        UI_PrintStringSmallNormal("dBm", 127 - 3 * 7, 0, 1);
        UI_PrintStringSmallNormal(str, 127 - 3 * 7 - 2 - strlen(str) * 7, 0, 1);
    }

    // Context line: peak hold and S-meter as small inverse labels (scan-list tag
    // style, 4 px/char), PK anchored left and S anchored right.
    sprintf(str, "PK %d", peakDbm);
    GUI_DisplaySmallestInverse(str, 4, 2, false, true, 4 + strlen(str) * 4);
    FOXHUNT_BuildS(sMeter, curDbm);
    GUI_DisplaySmallestInverse(sMeter, 126 - strlen(sMeter) * 4, 2, false, true, 125);

    // Segmented S-meter staircase.
    FOXHUNT_DrawBar();

    // Scale under the gauge, aligned with segments 1 / 5 / 9 / 13.
    GUI_DisplaySmallest("S1",    6, 41, false, true);
    GUI_DisplaySmallest("S5",   42, 41, false, true);
    GUI_DisplaySmallest("S9",   78, 41, false, true);
    GUI_DisplaySmallest("+40", 110, 41, false, true);

    // Attenuator as an inverse label (left), tuned frequency (right).
    sprintf(str, "ATT %ddB", FOXHUNT_ATT_DB[attStep]);
    GUI_DisplaySmallestInverse(str, 4, 6, false, true, 4 + strlen(str) * 4);
    sprintf(str, "%u.%05u", gRxVfo->pRX->Frequency / 100000, gRxVfo->pRX->Frequency % 100000);
    UI_PrintStringSmallNormal(str, 126 - strlen(str) * 7, 0, 6);
}

static void FOXHUNT_HandleKeys(void)
{
    kbd.prev    = kbd.current;
    kbd.current = KEYBOARD_GetKey();

    // Act only on the rising edge of a new key.
    if (kbd.current == KEY_INVALID || kbd.current == kbd.prev)
        return;

    switch (kbd.current) {
        case KEY_EXIT:
            foxRunning = false;
            break;
        case KEY_UP:
        case KEY_4:
            if (attStep < 3) { attStep++; FOXHUNT_ApplyAtt(); }
            break;
        case KEY_DOWN:
        case KEY_0:
            if (attStep > 0) { attStep--; FOXHUNT_ApplyAtt(); }
            break;
        case KEY_MENU:
            // Reset the peak hold and the trend reference.
            peakDbm  = curDbm;
            trendRef = curDbm;
            break;
        case KEY_1:
            // Cycle the audio: silent -> Geiger beep -> station audio -> silent.
            foxAudioMode = (uint8_t)((foxAudioMode + 1) % 3);
            FOXHUNT_SetAudio();
            if (foxAudioMode == FOXHUNT_AUDIO_BEEP)
                audioTick = FOXHUNT_RATE_SLOW_TICKS;   // blip promptly
            break;
        default:
            break;
    }
}

void APP_RunFoxHunt(void)
{
    // Finish any pending backlight fade before taking over the screen.
    BACKLIGHT_UpdateTickless();

    // Hunt on the user-selected VFO: dual-watch / cross-band may have left the
    // radio listening on the other VFO, so force RX on the selected one and retune.
    gEeprom.RX_VFO = gEeprom.TX_VFO;
    gRxVfo         = gTxVfo;
    RADIO_SetupRegisters(true);

    // Start silent; the audio (beep / station) is opt-in from inside the mode.
    AUDIO_AudioPathOff();
    foxAudioMode = FOXHUNT_AUDIO_OFF;
    audioTick    = 0;

    // Fixed front-end gain so the attenuator steps and the reading stay stable.
    BK4819_SetAGC(false);
    attStep = 0;
    FOXHUNT_ApplyAtt();

    curDbm     = FOXHUNT_ReadDbm();
    peakDbm    = curDbm;
    trendRef   = curDbm;
    trendDelta = 0;
    trendTick  = 0;

    kbd.current = KEY_INVALID;
    kbd.prev    = KEY_INVALID;

    foxRunning = true;
    while (foxRunning) {
#ifdef ENABLE_FEAT_F4HWN_K5VIEWER
        // Keep the K5Viewer link alive and pick up any remote key.
        K5VIEWER_ParseInput();
#endif
        FOXHUNT_HandleKeys();

        curDbm = FOXHUNT_ReadDbm();
        if (curDbm > peakDbm)
            peakDbm = curDbm;

        if (++trendTick >= FOXHUNT_TREND_TICKS) {
            trendDelta = curDbm - trendRef;
            trendRef   = curDbm;
            trendTick  = 0;
        }

        FOXHUNT_Draw();

        ST7565_BlitStatusLine();
        ST7565_BlitFullScreen();

#ifdef ENABLE_FEAT_F4HWN_K5VIEWER
        // Mirror the screen to K5Viewer (sends only changed chunks).
        K5VIEWER_Update(false);
#endif

        // Beep mode: Geiger blip whose rate/pitch track the level (silent below floor).
        if (foxAudioMode == FOXHUNT_AUDIO_BEEP && curDbm >= FOXHUNT_SILENCE_DBM) {
            if (++audioTick >= FOXHUNT_BlipPeriod(curDbm)) {
                audioTick = 0;
                FOXHUNT_Blip(FOXHUNT_DbmToTone(curDbm));
            }
        }
        // Station mode: route the received audio, gated by the silence floor.
        else if (foxAudioMode == FOXHUNT_AUDIO_STATION) {
            BK4819_SetAF(curDbm >= FOXHUNT_SILENCE_DBM
                         ? (gRxVfo->Modulation == MODULATION_AM ? BK4819_AF_AM : BK4819_AF_FM)
                         : BK4819_AF_MUTE);
        }

        SYSTEM_DelayMs(FOXHUNT_TICK_MS);
    }

    // Mute any pending tone and restore the normal VFO selection and RX config.
    AUDIO_AudioPathOff();
    RADIO_SelectVfos();
    RADIO_SetupRegisters(true);
}

void ACTION_FoxHunt(void)
{
    APP_RunFoxHunt();
    GUI_SelectNextDisplay(DISPLAY_MAIN);
}

#endif // ENABLE_FEAT_F4HWN_FOXHUNT
