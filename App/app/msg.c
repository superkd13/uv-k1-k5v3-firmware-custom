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

#ifdef ENABLE_FEAT_KD_MSG

#include <assert.h>
#include <string.h>

#include "app/aircopy.h"
#include "app/msg.h"
#include "audio.h"
#include "driver/bk4819.h"
#include "driver/crc.h"
#include "driver/st7565.h"
#include "frequencies.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/main.h"
#include "ui/msg.h"
#include "ui/ui.h"

#define MSG_PACKET_MAGIC   0x2718

typedef struct {
    uint16_t magic;
    char     content[MSG_MAX_SIZE];
} MSG_Payload_t;

static const char* const msg_char_map[10] = {
    " :0",                          // KEY_0
    ".,-()@/\\+=*#<>[]1",           // KEY_1
    "abc2",                         // KEY_2
    "def3",                         // KEY_3
    "ghi4",                         // KEY_4
    "jkl5",                         // KEY_5
    "mno6",                         // KEY_6
    "pqrs7",                        // KEY_7
    "tuv8",                         // KEY_8
    "wxyz9"                         // KEY_9
};

static_assert(sizeof(MSG_Payload_t) <= 22);

bool          gMsgActive;
char          gLastMessages[4][MSG_MAX_SIZE];
char          gCurrentUserMessage[MSG_MAX_SIZE] = " ";
uint8_t       gCurrentMsgWriteIndex = 0;
uint8_t       gReceivedSent = 0;
uint8_t       gSentAck      = 0;
bool          gUnreadMessage = false;

static KEY_Code_t msg_edit_last_key = 255;
static uint8_t msg_edit_char_index = 0;

static void MSG_KeyExit(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    if (bKeyHeld)
    {
        BK4819_ResetFSK();
        RADIO_SelectVfos();
        RADIO_SetupRegisters(true);

        GUI_SelectNextDisplay(DISPLAY_MAIN);
        gMsgActive = false;
    }
    else
    {
        msg_edit_last_key = 255;
        gCurrentUserMessage[gCurrentMsgWriteIndex ? gCurrentMsgWriteIndex-- : 0] = ' ';
    }
}

static void MSG_SendPacket(void)
{
    memset(g_FSK_Buffer, 0, sizeof(g_FSK_Buffer));
    
    MSG_Payload_t* const payload = (MSG_Payload_t *)&g_FSK_Buffer[0];
    
    payload->magic = MSG_PACKET_MAGIC;
    strncpy(payload->content, gCurrentUserMessage, MSG_MAX_SIZE - 1);

    g_FSK_Buffer[11] = CRC_Calculate(&g_FSK_Buffer[0], 22);

    gReceivedSent |= (1 << MSG_SENDING);
    UI_DisplayMsg();
    ST7565_BlitFullScreen();

    RADIO_SetTxParameters();
    BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);
    BK4819_SendFSKDataMsg(g_FSK_Buffer, 12);
    BK4819_SetupPowerAmplifier(0, 0); 
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
    BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);

    //RADIO_SelectVfos(); // useless here?
    RADIO_SetupRegisters(true);
    gBeepToPlay = BEEP_880HZ_60MS_TRIPLE_BEEP;
    gUpdateDisplay = true;
    gReceivedSent = ((gReceivedSent << 1) & 14) | 1;
    gSentAck      = ((gSentAck << 1) & 14);

    strncpy(gLastMessages[0], gLastMessages[1], MSG_MAX_SIZE);
    strncpy(gLastMessages[1], gLastMessages[2], MSG_MAX_SIZE);
    strncpy(gLastMessages[2], gLastMessages[3], MSG_MAX_SIZE);
    strncpy(gLastMessages[3], gCurrentUserMessage, MSG_MAX_SIZE);
    strncpy(gCurrentUserMessage, " ", MSG_MAX_SIZE); 

}

static VfoState_t MSG_TxState(void)
{
    if (TX_freq_check(gTxVfo->pTX->Frequency) != 0 && gTxVfo->TX_LOCK)
        return VFO_STATE_TX_DISABLE;
    if (gBatteryDisplayLevel == 0)
        return VFO_STATE_BAT_LOW;
    if (gBatteryDisplayLevel > 6)
        return VFO_STATE_VOLTAGE_HIGH;
#ifndef ENABLE_TX_WHEN_AM
    if (gTxVfo->Modulation != MODULATION_FM)
        return VFO_STATE_TX_DISABLE;
#endif
    return VFO_STATE_NORMAL;
}

static void MSG_KeyMenu(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    if (gCurrentMsgWriteIndex < 15)
    {   
        if (gCurrentUserMessage[gCurrentMsgWriteIndex] == '\0')
        {
            gCurrentUserMessage[gCurrentMsgWriteIndex] = ' ';
        }
        gCurrentMsgWriteIndex++;
        msg_edit_last_key = 255;
    }
    VfoState_t state = MSG_TxState();
    if (bKeyHeld) {
        if (state == VFO_STATE_NORMAL)
        {
            gCurrentMsgWriteIndex = 0;
            BK4819_SetupMsg();
            BK4819_ResetFSK();
            MSG_SendPacket();
        }
        else 
        {
            gReceivedSent |= (1 << MSG_NO_TX);
            UI_DisplayMsg();
            ST7565_BlitFullScreen();
            SYSTEM_DelayMs(1000);
            gReceivedSent -= (1 << MSG_NO_TX);
        }
    }
}

static void MSG_Key_0_to_9(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    if (!bKeyPressed)
        return;

    if (gCurrentMsgWriteIndex >= sizeof(gCurrentUserMessage))
        return;

    uint8_t key_idx = Key - KEY_0;

    if (Key != msg_edit_last_key)
    {
        msg_edit_last_key = Key;
        msg_edit_char_index = 0;
    }
    else
    {
        msg_edit_char_index++;
        if (msg_char_map[key_idx][msg_edit_char_index] == '\0')
        {
            msg_edit_char_index = 0;
        }
    }

    char c = msg_char_map[key_idx][msg_edit_char_index];
    gCurrentUserMessage[gCurrentMsgWriteIndex] = c;
}

void ACTION_Msg(void)
{
    gMsgActive = true;
    gUnreadMessage = false;
    GUI_SelectNextDisplay(DISPLAY_MSG);
}

void MSG_StorePacket(void)
{
    if (gFSKWriteIndex < 16) {
        return;
    }

    gFSKWriteIndex = 0;
    gUpdateDisplay = true;
    RADIO_SetupRegisters(true);
    uint16_t Crc = CRC_Calculate(&g_FSK_Buffer[0], 22);
    if (g_FSK_Buffer[11] != Crc) {
        return;
    }

    MSG_Payload_t * const payload = (MSG_Payload_t *)&g_FSK_Buffer[0];

    if (payload->magic != MSG_PACKET_MAGIC)
        return;

    gReceivedSent = ((gReceivedSent << 1) & 14);
    gSentAck      = ((gSentAck << 1) & 14);

    if(!gMsgActive) 
        gUnreadMessage = true;

    strncpy(gLastMessages[0], gLastMessages[1], MSG_MAX_SIZE);
    strncpy(gLastMessages[1], gLastMessages[2], MSG_MAX_SIZE);
    strncpy(gLastMessages[2], gLastMessages[3], MSG_MAX_SIZE);
    strncpy(gLastMessages[3], payload->content, MSG_MAX_SIZE);

    //BK4819_ResetFSK();
}

void MSG_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    if (((Key != KEY_MENU && Key != KEY_EXIT) && bKeyHeld) || !bKeyPressed)
        return;

    if (Key != KEY_PTT)
        gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;

    switch (Key) {
    case KEY_0...KEY_9:
        MSG_Key_0_to_9(Key, bKeyPressed, bKeyHeld);
        break;
    case KEY_EXIT:
        MSG_KeyExit(Key, bKeyPressed, bKeyHeld);
        return;
    case KEY_STAR:
        while (gCurrentMsgWriteIndex)
            MSG_KeyExit(Key, bKeyPressed, false);
        MSG_KeyExit(Key, bKeyPressed, false);
        break;
    case KEY_MENU:
        MSG_KeyMenu(Key, bKeyPressed, bKeyHeld);
        break;
    default:
        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
        break;
    }

    gRequestDisplayScreen = DISPLAY_MSG;
}

#endif // ENABLE_FEAT_KD_MSG
