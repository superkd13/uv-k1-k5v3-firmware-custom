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

    pPrintStr = "Messages";

    UI_PrintStringSmallBold(pPrintStr, 2, 127, 0);

    UI_PrintStringSmallNormal(gReceivedSent & 8 ? "< " : "> ", 2, 0, 1);
    UI_PrintStringSmallNormal(gLastMessages[0], 16, 0, 1);
    UI_PrintStringSmallNormal(gReceivedSent & 4 ? "< " : "> ", 2, 0, 2);
    UI_PrintStringSmallNormal(gLastMessages[1], 16, 0, 2);
    UI_PrintStringSmallNormal(gReceivedSent & 2 ? "< " : "> ", 2, 0, 3);
    UI_PrintStringSmallNormal(gLastMessages[2], 16, 0, 3);
    UI_PrintStringSmallNormal(gReceivedSent & 1 ? "< " : "> ", 2, 0, 4);
    UI_PrintStringSmallNormal(gLastMessages[3], 16, 0, 4);
    UI_PrintStringSmallNormal(":", 3, 0, 5);

    if (gReceivedSent & (1 << 4))
        UI_PrintStringSmallBold(" SENDING ...", 12, 0, 5);
    else
        UI_PrintStringSmallNormal(gCurrentUserMessage, 12, 0, 5);

    UI_PrintStringSmallNormal("^", 12 + gCurrentMsgWriteIndex * 7, 0, 6);

    ST7565_BlitFullScreen();
}

#endif