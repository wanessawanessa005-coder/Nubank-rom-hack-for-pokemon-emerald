#include "global.h"
#include "nubank.h"
#include "bg.h"
#include "gpu_regs.h"
#include "main.h"
#include "menu.h"
#include "money.h"
#include "palette.h"
#include "scanline_effect.h"
#include "sound.h"
#include "sprite.h"
#include "string_util.h"
#include "task.h"
#include "text.h"
#include "window.h"
#include "constants/rgb.h"
#include "constants/songs.h"

// An original, Nubank-inspired banking app screen (purple header, greeting,
// account balance, row of buttons). It is drawn entirely with one full-screen
// text window, so it needs no extra image files.
//
// Note: the game font has no "ã"/"õ" and "$" is the end-of-text marker, so
// amounts use the Pokédollar symbol (¥) and the text avoids those letters.

#define WIN_MAIN 0

#define NUBANK_PURPLE RGB(16, 1, 26)

#define KEY_REPT(button) TEST_BUTTON(gMain.newAndRepeatedKeys, button)

// Palette indexes
enum {
    COL_BG,     // white (also the backdrop)
    COL_PURPLE, // header purple
    COL_DARK,   // dark purple
    COL_GRAY,   // light gray panels
    COL_MID,    // mid gray text
    COL_TEXT,   // near-black text
    COL_WHITE,  // white text / shapes
    COL_LIGHT,  // light purple
    COL_GREEN,  // success
    COL_SHADOW, // spare gray
    COL_RED,    // error
};

enum {
    BTN_PIX,
    BTN_DEPOSIT,
    BTN_WITHDRAW,
    BTN_EXIT,
    BTN_COUNT
};

enum {
    MODE_MAIN,
    MODE_AMOUNT
};

enum {
    MSG_LAST_PIX,
    MSG_TOTAL_PIX,
    MSG_OK,
    MSG_ERROR
};

#define BTN_W 46
#define BTN_H 30
#define BTN_Y 94
#define BTN_X(i) (7 + (i) * 60)
#define BAR_Y 142

struct NubankUi
{
    u32 shownBalance;
    u32 amount;
    u32 step;
    u8 selection;
    u8 mode;
    u8 isDeposit;
    u8 msgKind;
    u8 msg[48];
};

static EWRAM_DATA struct NubankUi sUi = {0};

static const u16 sNubankPal[16] =
{
    [COL_BG]     = RGB(31, 31, 31),
    [COL_PURPLE] = RGB(16, 1, 26),
    [COL_DARK]   = RGB(11, 0, 18),
    [COL_GRAY]   = RGB(29, 29, 30),
    [COL_MID]    = RGB(18, 18, 20),
    [COL_TEXT]   = RGB(3, 3, 5),
    [COL_WHITE]  = RGB(31, 31, 31),
    [COL_LIGHT]  = RGB(25, 16, 30),
    [COL_GREEN]  = RGB(3, 18, 10),
    [COL_SHADOW] = RGB(24, 24, 26),
    [COL_RED]    = RGB(25, 4, 4),
    [11 ... 15]  = RGB(31, 31, 31),
};

static const struct BgTemplate sBgTemplates[] =
{
    {
        .bg = 0,
        .charBaseIndex = 0,
        .mapBaseIndex = 31,
        .screenSize = 0,
        .paletteMode = 0,
        .priority = 0,
        .baseTile = 0
    }
};

static const struct WindowTemplate sWindowTemplates[] =
{
    [WIN_MAIN] = {
        .bg = 0,
        .tilemapLeft = 0,
        .tilemapTop = 0,
        .width = 30,
        .height = 20,
        .paletteNum = 0,
        .baseBlock = 1
    },
    DUMMY_WIN_TEMPLATE
};

static const u8 sText_Hello[] = _("Olá, ");
static const u8 sText_Account[] = _("Conta");
static const u8 sText_Chevron[] = _(">");
static const u8 sText_Question[] = _("?");
static const u8 sText_Yen[] = _("¥");
static const u8 sText_BtnPix[] = _("Pix");
static const u8 sText_BtnDeposit[] = _("Depositar");
static const u8 sText_BtnWithdraw[] = _("Sacar");
static const u8 sText_BtnExit[] = _("Sair");
static const u8 sText_PixTotalLine[] = _("Pix recebidos: ");
static const u8 sText_LastPix[] = _("Pix mais recente: ");
static const u8 sText_TotalPix[] = _("Total em Pix: ");
static const u8 sText_NoPix[] = _("Nenhum Pix recebido ainda");
static const u8 sText_Step[] = _("Passo ");
static const u8 sText_StepKeys[] = _("  {LEFT_ARROW}{RIGHT_ARROW}");
static const u8 sText_DownArrow[] = _("{DOWN_ARROW} ");
static const u8 sText_UpArrow[] = _(" {UP_ARROW}");
static const u8 sText_AmountHint[] = _("A: ok  B: voltar  R: máximo");
static const u8 sText_DepositDone1[] = _("Depósito de ");
static const u8 sText_WithdrawDone1[] = _("Saque de ");
static const u8 sText_Done2[] = _(" feito!");
static const u8 sText_ErrWalletEmpty[] = _("Carteira vazia.");
static const u8 sText_ErrAccountFull[] = _("Conta Nubank cheia.");
static const u8 sText_ErrNoBalance[] = _("Sem saldo na conta.");
static const u8 sText_ErrWalletFull[] = _("Carteira cheia.");

static const u8 *const sButtonLabels[BTN_COUNT] =
{
    [BTN_PIX]      = sText_BtnPix,
    [BTN_DEPOSIT]  = sText_BtnDeposit,
    [BTN_WITHDRAW] = sText_BtnWithdraw,
    [BTN_EXIT]     = sText_BtnExit,
};

static void Task_NubankFadeIn(u8 taskId);
static void Task_NubankProcessInput(u8 taskId);
static void Task_NubankExit(u8 taskId);
static void DrawScreen(void);

// ---- Drawing helpers ------------------------------------------------------

static void FillRect(u8 color, s32 x, s32 y, s32 w, s32 h)
{
    FillWindowPixelRect(WIN_MAIN, color, x, y, w, h);
}

// Rectangle with the corners trimmed
static void FillRounded(u8 color, s32 x, s32 y, s32 w, s32 h)
{
    FillRect(color, x + 2, y, w - 4, h);
    FillRect(color, x, y + 2, w, h - 4);
    FillRect(color, x + 1, y + 1, w - 2, h - 2);
}

static void DrawDiamond(u8 color, s32 cx, s32 cy, s32 r)
{
    s32 i;

    for (i = -r; i <= r; i++)
    {
        s32 half = r - (i < 0 ? -i : i);
        FillRect(color, cx - half, cy + i, half * 2 + 1, 1);
    }
}

static void PrintText(u8 font, u8 x, u8 y, u8 bg, u8 fg, u8 shadow, const u8 *str)
{
    u8 colors[3];

    colors[0] = bg;
    colors[1] = fg;
    colors[2] = shadow;
    AddTextPrinterParameterized3(WIN_MAIN, font, x, y, colors, TEXT_SKIP_DRAW, str);
}

static void PrintCentered(u8 font, s32 centerX, u8 y, u8 bg, u8 fg, u8 shadow, const u8 *str)
{
    s32 width = GetStringWidth(font, str, 0);

    PrintText(font, centerX - width / 2, y, bg, fg, shadow, str);
}

// Writes "¥1234" into dst
static u8 *BufferYen(u8 *dst, u32 value)
{
    u8 *ptr = StringCopy(dst, sText_Yen);

    return ConvertIntToDecimalStringN(ptr, value, STR_CONV_MODE_LEFT_ALIGN, 8);
}

static void DrawIcon(u8 button, s32 cx, s32 cy, u8 color, u8 face)
{
    s32 i;

    switch (button)
    {
    case BTN_PIX: // diamond
        DrawDiamond(color, cx, cy, 9);
        DrawDiamond(face, cx, cy, 5);
        DrawDiamond(color, cx, cy, 2);
        break;
    case BTN_DEPOSIT: // arrow pointing down into a tray
        FillRect(color, cx - 1, cy - 9, 3, 9);
        for (i = 0; i < 5; i++)
            FillRect(color, cx - 4 + i, cy - 1 + i, 9 - 2 * i, 1);
        FillRect(color, cx - 8, cy + 7, 17, 2);
        break;
    case BTN_WITHDRAW: // arrow pointing up out of a tray
        for (i = 0; i < 5; i++)
            FillRect(color, cx - i, cy - 9 + i, 2 * i + 1, 1);
        FillRect(color, cx - 1, cy - 4, 3, 9);
        FillRect(color, cx - 8, cy + 7, 17, 2);
        break;
    case BTN_EXIT: // X
        for (i = 0; i < 10; i++)
        {
            FillRect(color, cx - 5 + i, cy - 5 + i, 2, 2);
            FillRect(color, cx + 4 - i, cy - 5 + i, 2, 2);
        }
        break;
    }
}

static void DrawHeader(void)
{
    FillRect(COL_PURPLE, 0, 0, DISPLAY_WIDTH, 46);

    // avatar
    FillRounded(COL_LIGHT, 10, 5, 24, 24);
    FillRounded(COL_WHITE, 18, 9, 8, 8);
    FillRounded(COL_WHITE, 14, 19, 16, 8);

    // help icon
    FillRounded(COL_LIGHT, 206, 7, 20, 20);
    PrintCentered(FONT_NORMAL, 216, 9, COL_LIGHT, COL_PURPLE, COL_LIGHT, sText_Question);

    // greeting
    StringCopy(gStringVar4, sText_Hello);
    StringAppend(gStringVar4, gSaveBlock2Ptr->playerName);
    PrintText(FONT_NORMAL, 10, 30, COL_PURPLE, COL_WHITE, COL_PURPLE, gStringVar4);
}

static void DrawAccount(void)
{
    PrintText(FONT_NORMAL, 10, 50, COL_BG, COL_TEXT, COL_BG, sText_Account);
    PrintText(FONT_NORMAL, 222, 50, COL_BG, COL_MID, COL_BG, sText_Chevron);

    BufferYen(gStringVar4, sUi.shownBalance);
    PrintText(FONT_NORMAL, 10, 66, COL_BG, COL_TEXT, COL_SHADOW, gStringVar4);

    StringCopy(gStringVar4, sText_PixTotalLine);
    BufferYen(gStringVar3, Nubank_GetTotalPix());
    StringAppend(gStringVar4, gStringVar3);
    PrintText(FONT_SMALL, 10, 83, COL_BG, COL_MID, COL_BG, gStringVar4);
}

static void DrawButtons(void)
{
    s32 i;

    for (i = 0; i < BTN_COUNT; i++)
    {
        s32 x = BTN_X(i);
        bool8 selected = (sUi.selection == i);
        u8 face = selected ? COL_LIGHT : COL_GRAY;
        u8 iconColor = selected ? COL_DARK : COL_TEXT;

        if (selected)
            FillRounded(COL_PURPLE, x - 2, BTN_Y - 2, BTN_W + 4, BTN_H + 4);
        FillRounded(face, x, BTN_Y, BTN_W, BTN_H);
        DrawIcon(i, x + BTN_W / 2, BTN_Y + BTN_H / 2, iconColor, face);
        PrintCentered(FONT_SMALL, x + BTN_W / 2, 128, COL_BG, selected ? COL_PURPLE : COL_MID, COL_BG, sButtonLabels[i]);
    }
}

static void DrawAmountPanel(void)
{
    u8 *ptr;

    PrintText(FONT_NORMAL, 10, 92, COL_BG, COL_PURPLE, COL_BG, sUi.isDeposit ? sText_BtnDeposit : sText_BtnWithdraw);

    FillRounded(COL_PURPLE, 56, 108, 128, 20);
    FillRounded(COL_BG, 58, 110, 124, 16);
    ptr = StringCopy(gStringVar4, sText_DownArrow);
    ptr = BufferYen(ptr, sUi.amount);
    StringAppend(gStringVar4, sText_UpArrow);
    PrintCentered(FONT_NORMAL, 120, 111, COL_BG, COL_TEXT, COL_BG, gStringVar4);

    ptr = StringCopy(gStringVar4, sText_Step);
    ptr = BufferYen(ptr, sUi.step);
    StringAppend(gStringVar4, sText_StepKeys);
    PrintCentered(FONT_NORMAL, 120, 127, COL_BG, COL_MID, COL_BG, gStringVar4);
}

static void DrawBar(void)
{
    const u8 *text = gStringVar4;
    u8 color = COL_TEXT;
    u32 last;

    FillRounded(COL_GRAY, 6, BAR_Y, 228, 16);

    if (sUi.mode == MODE_AMOUNT)
    {
        text = sText_AmountHint;
        color = COL_MID;
    }
    else
    {
        switch (sUi.msgKind)
        {
        case MSG_LAST_PIX:
            last = Nubank_GetLastPix();
            if (last == 0)
            {
                text = sText_NoPix;
            }
            else
            {
                StringCopy(gStringVar4, sText_LastPix);
                BufferYen(gStringVar3, last);
                StringAppend(gStringVar4, gStringVar3);
            }
            break;
        case MSG_TOTAL_PIX:
            StringCopy(gStringVar4, sText_TotalPix);
            BufferYen(gStringVar3, Nubank_GetTotalPix());
            StringAppend(gStringVar4, gStringVar3);
            break;
        case MSG_OK:
            text = sUi.msg;
            color = COL_GREEN;
            break;
        default: // MSG_ERROR
            text = sUi.msg;
            color = COL_RED;
            break;
        }
    }
    PrintText(FONT_NORMAL, 12, BAR_Y, COL_GRAY, color, COL_GRAY, text);
}

static void DrawScreen(void)
{
    FillWindowPixelBuffer(WIN_MAIN, PIXEL_FILL(COL_BG));
    DrawHeader();
    DrawAccount();
    if (sUi.mode == MODE_AMOUNT)
        DrawAmountPanel();
    else
        DrawButtons();
    DrawBar();
    CopyWindowToVram(WIN_MAIN, COPYWIN_GFX);
}

// ---- Logic ----------------------------------------------------------------

static u32 GetMaxAmount(void)
{
    u32 wallet = GetMoney(&gSaveBlock1Ptr->money);
    u32 balance = Nubank_GetBalance();
    u32 room;

    if (sUi.isDeposit)
    {
        room = NUBANK_MAX_BALANCE - balance;
        return (wallet < room) ? wallet : room;
    }
    room = MAX_MONEY - wallet;
    return (balance < room) ? balance : room;
}

static void SetMessage(u8 kind, const u8 *text)
{
    sUi.msgKind = kind;
    StringCopy(sUi.msg, text);
}

static void EnterAmountMode(bool8 isDeposit)
{
    u32 max;

    sUi.mode = MODE_AMOUNT;
    sUi.isDeposit = isDeposit;
    sUi.step = 100;
    max = GetMaxAmount();
    sUi.amount = (sUi.step < max) ? sUi.step : max;
}

static void ConfirmTransfer(void)
{
    u32 amount = sUi.amount;
    u8 *ptr;

    if (amount == 0 || amount > GetMaxAmount())
    {
        if (sUi.isDeposit)
            SetMessage(MSG_ERROR, GetMoney(&gSaveBlock1Ptr->money) == 0 ? sText_ErrWalletEmpty : sText_ErrAccountFull);
        else
            SetMessage(MSG_ERROR, Nubank_GetBalance() == 0 ? sText_ErrNoBalance : sText_ErrWalletFull);
        PlaySE(SE_FAILURE);
        sUi.mode = MODE_MAIN;
        return;
    }

    if (sUi.isDeposit)
    {
        RemoveMoney(&gSaveBlock1Ptr->money, amount);
        Nubank_AddBalance(amount);
        ptr = StringCopy(sUi.msg, sText_DepositDone1);
    }
    else
    {
        Nubank_RemoveBalance(amount);
        AddMoney(&gSaveBlock1Ptr->money, amount);
        ptr = StringCopy(sUi.msg, sText_WithdrawDone1);
    }
    ptr = BufferYen(ptr, amount);
    StringAppend(sUi.msg, sText_Done2);
    sUi.msgKind = MSG_OK;
    sUi.mode = MODE_MAIN;
    PlaySE(SE_SHOP);
}

static void StartExit(u8 taskId)
{
    PlaySE(SE_PC_OFF);
    BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
    gTasks[taskId].func = Task_NubankExit;
}

// ---- Tasks and setup ------------------------------------------------------

static void Task_NubankFadeIn(u8 taskId)
{
    if (!gPaletteFade.active)
        gTasks[taskId].func = Task_NubankProcessInput;
}

static void Task_NubankProcessInput(u8 taskId)
{
    bool8 redraw = FALSE;
    u32 target = Nubank_GetBalance();

    // The balance counts up (or down) toward its real value
    if (sUi.shownBalance != target)
    {
        u32 diff = (sUi.shownBalance < target) ? target - sUi.shownBalance : sUi.shownBalance - target;
        u32 delta = diff / 6 + 1;

        if (delta > diff)
            delta = diff;
        if (sUi.shownBalance < target)
            sUi.shownBalance += delta;
        else
            sUi.shownBalance -= delta;
        redraw = TRUE;
    }

    if (sUi.mode == MODE_MAIN)
    {
        if (JOY_NEW(DPAD_RIGHT))
        {
            sUi.selection = (sUi.selection + 1) % BTN_COUNT;
            PlaySE(SE_SELECT);
            redraw = TRUE;
        }
        else if (JOY_NEW(DPAD_LEFT))
        {
            sUi.selection = (sUi.selection + BTN_COUNT - 1) % BTN_COUNT;
            PlaySE(SE_SELECT);
            redraw = TRUE;
        }
        else if (JOY_NEW(A_BUTTON))
        {
            switch (sUi.selection)
            {
            case BTN_PIX:
                sUi.msgKind = (sUi.msgKind == MSG_LAST_PIX) ? MSG_TOTAL_PIX : MSG_LAST_PIX;
                PlaySE(SE_PIN);
                break;
            case BTN_DEPOSIT:
                EnterAmountMode(TRUE);
                PlaySE(SE_SELECT);
                break;
            case BTN_WITHDRAW:
                EnterAmountMode(FALSE);
                PlaySE(SE_SELECT);
                break;
            default: // BTN_EXIT
                StartExit(taskId);
                return;
            }
            redraw = TRUE;
        }
        else if (JOY_NEW(B_BUTTON))
        {
            StartExit(taskId);
            return;
        }
    }
    else // MODE_AMOUNT
    {
        u32 max = GetMaxAmount();

        if (KEY_REPT(DPAD_UP))
        {
            if (sUi.amount < max)
            {
                sUi.amount = (max - sUi.amount < sUi.step) ? max : sUi.amount + sUi.step;
                PlaySE(SE_SELECT);
                redraw = TRUE;
            }
        }
        else if (KEY_REPT(DPAD_DOWN))
        {
            if (sUi.amount > 0)
            {
                sUi.amount = (sUi.amount < sUi.step) ? 0 : sUi.amount - sUi.step;
                PlaySE(SE_SELECT);
                redraw = TRUE;
            }
        }
        else if (JOY_NEW(DPAD_RIGHT))
        {
            if (sUi.step < 100000)
                sUi.step *= 10;
            PlaySE(SE_SELECT);
            redraw = TRUE;
        }
        else if (JOY_NEW(DPAD_LEFT))
        {
            if (sUi.step > 1)
                sUi.step /= 10;
            PlaySE(SE_SELECT);
            redraw = TRUE;
        }
        else if (JOY_NEW(R_BUTTON))
        {
            sUi.amount = max;
            PlaySE(SE_SELECT);
            redraw = TRUE;
        }
        else if (JOY_NEW(A_BUTTON))
        {
            ConfirmTransfer();
            redraw = TRUE;
        }
        else if (JOY_NEW(B_BUTTON))
        {
            sUi.mode = MODE_MAIN;
            PlaySE(SE_SELECT);
            redraw = TRUE;
        }
    }

    if (redraw)
        DrawScreen();
}

static void Task_NubankExit(u8 taskId)
{
    if (!gPaletteFade.active)
    {
        DestroyTask(taskId);
        FreeAllWindowBuffers();
        SetMainCallback2(gMain.savedCallback);
    }
}

static void MainCB2(void)
{
    RunTasks();
    AnimateSprites();
    BuildOamBuffer();
    UpdatePaletteFade();
}

static void VBlankCB(void)
{
    LoadOam();
    ProcessSpriteCopyRequests();
    TransferPlttBuffer();
}

void CB2_InitNubankApp(void)
{
    switch (gMain.state)
    {
    default:
    case 0:
        SetVBlankCallback(NULL);
        gMain.state++;
        break;
    case 1:
        DmaClearLarge16(3, (void *)(VRAM), VRAM_SIZE, 0x1000);
        DmaClear32(3, OAM, OAM_SIZE);
        DmaClear16(3, PLTT, PLTT_SIZE);
        SetGpuReg(REG_OFFSET_DISPCNT, 0);
        ResetBgsAndClearDma3BusyFlags(0);
        InitBgsFromTemplates(0, sBgTemplates, ARRAY_COUNT(sBgTemplates));
        ChangeBgX(0, 0, BG_COORD_SET);
        ChangeBgY(0, 0, BG_COORD_SET);
        InitWindows(sWindowTemplates);
        DeactivateAllTextPrinters();
        SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_OBJ_ON | DISPCNT_OBJ_1D_MAP);
        ShowBg(0);
        gMain.state++;
        break;
    case 2:
        ResetPaletteFade();
        ScanlineEffect_Stop();
        ResetTasks();
        ResetSpriteData();
        gMain.state++;
        break;
    case 3:
        LoadPalette(sNubankPal, BG_PLTT_ID(0), sizeof(sNubankPal));
        sUi.shownBalance = Nubank_GetBalance();
        sUi.amount = 0;
        sUi.step = 100;
        sUi.selection = BTN_DEPOSIT;
        sUi.mode = MODE_MAIN;
        sUi.isDeposit = TRUE;
        sUi.msgKind = MSG_LAST_PIX;
        sUi.msg[0] = EOS;
        PutWindowTilemap(WIN_MAIN);
        DrawScreen();
        CopyWindowToVram(WIN_MAIN, COPYWIN_FULL);
        gMain.state++;
        break;
    case 4:
        CreateTask(Task_NubankFadeIn, 0);
        BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, NUBANK_PURPLE); // purple "splash" fade-in
        SetVBlankCallback(VBlankCB);
        SetMainCallback2(MainCB2);
        PlaySE(SE_PC_LOGIN);
        return;
    }
}
