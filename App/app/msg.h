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

#ifndef APP_MSG_H
#define APP_MSG_H

#ifdef ENABLE_FEAT_KD_MSG

#include <stdbool.h>
#include <stdint.h>

#include "driver/keyboard.h"

#define MSG_MAX_SIZE       20

extern bool          gMsgActive;
extern char          gLastMessages[2][MSG_MAX_SIZE];
extern char          gCurrentUserMessage[MSG_MAX_SIZE];
extern uint8_t       gCurrentMsgWriteIndex;
extern uint8_t       gReceivedSent;

void ACTION_Msg(void);
void MSG_StorePacket(void);
void MSG_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);

#endif // ENABLE_FEAT_KD_MSG
#endif // APP_MSG_H
