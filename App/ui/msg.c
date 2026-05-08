/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
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

#include <string.h>

#include "app/aircopy.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "radio.h"
#include "ui/msg.h"
#include "app/msg.h"
#include "ui/helper.h"
#include "ui/inputbox.h"

void UI_DisplayMsg(void)
{
    char *pPrintStr = { 0 };

    UI_DisplayClear();

    pPrintStr = "Bonjour de MSG";

    UI_PrintString(pPrintStr, 2, 127, 0, 8);

    UI_PrintStringSmallNormal(gLastReceivedMessage, 20, 0, 3);
    UI_PrintStringSmallNormal(gCurrentUserMessage, 20, 0, 5);

    ST7565_BlitFullScreen();
}

#endif