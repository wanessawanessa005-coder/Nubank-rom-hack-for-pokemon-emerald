#include "global.h"
#include "nu_radar.h"
#include "battle_main.h"
#include "battle_setup.h"
#include "bg.h"
#include "data.h"
#include "gpu_regs.h"
#include "main.h"
#include "menu.h"
#include "palette.h"
#include "region_map.h"
#include "scanline_effect.h"
#include "sound.h"
#include "sprite.h"
#include "string_util.h"
#include "task.h"
#include "text.h"
#include "trig.h"
#include "window.h"
#include "constants/opponents.h"
#include "constants/region_map_sections.h"
#include "constants/rgb.h"
#include "constants/songs.h"
#include "constants/trainers.h"

// NU-RADAR: a full-screen radar that lists TRAINERS who will send a PIX if beaten.
// Centered on the player's current location, it places blips for trainers spread
// across the whole region. Everything is drawn in code (no extra art files).

struct NuRadarEntry
{
    u16 trainerId;
    u16 mapSec;
};

#include "data/nu_radar_trainers.h"

#define NUM_RADAR_TRAINERS ARRAY_COUNT(sRadarTrainers)

#define MAX_BLIPS   12
#define LIST_ROWS   7
#define ROW_HEIGHT  16

// Radar window (pixels) and geometry
#define RADAR_W   120
#define RADAR_H   112
#define RADAR_TILES_W (RADAR_W / 8)
#define CX        60
#define CY        56
#define RADIUS    52
#define BLIP_LIMIT_SQ ((RADIUS - 5) * (RADIUS - 5))
#define TRAIL     22
#define FLASH_MAX 36

#define KEY_REPT(button) TEST_BUTTON(gMain.newAndRepeatedKeys, button)

enum {
    WIN_HEADER,
    WIN_RADAR,
    WIN_LIST,
    WIN_INFO,
};

// Palette indexes (shared by every window)
enum {
    COL_BG,      // radar background
    COL_PURPLE,  // header and selected row
    COL_DARK,    // dark purple panel
    COL_RING,    // dim grid lines
    COL_RING2,   // brighter grid lines
    COL_WHITE,   // text
    COL_GRAY,    // secondary text
    COL_S0,      // sweep: leading edge
    COL_S1,
    COL_S2,
    COL_S3,      // sweep: faint tail
    COL_BLIP1,   // blip: dim
    COL_BLIP2,
    COL_BLIP3,
    COL_BLIP4,   // blip: brightest
    COL_HILITE,  // animated highlight (gold <-> white)
};

struct Blip
{
    s16 ox;       // offset from the radar center
    s16 oy;
    u16 dist2;    // ox*ox + oy*oy
    u16 trainerId;
    u16 mapSec;
    u8 flash;     // 0 = dim, FLASH_MAX = just swept
    u8 cooldown;
    u32 prize;
};

struct NuRadar
{
    struct Blip blips[MAX_BLIPS];
    u8 count;
    u8 selected;
    u8 scroll;
    u8 sweep;
    u16 frame;
    s16 pingRadius; // 0 = no ping in progress
};

struct Candidate
{
    u16 index; // into sRadarTrainers
    u16 dist;  // squared distance on the region map grid
};

static EWRAM_DATA struct NuRadar sRadar = {0};
static EWRAM_DATA struct Candidate sCandidates[NUM_RADAR_TRAINERS] = {0};
static EWRAM_DATA u8 *sPixels = NULL;

static const u16 sRadarPal[16] =
{
    [COL_BG]     = RGB(1, 0, 4),
    [COL_PURPLE] = RGB(16, 1, 26),
    [COL_DARK]   = RGB(5, 0, 9),
    [COL_RING]   = RGB(6, 3, 12),
    [COL_RING2]  = RGB(13, 9, 21),
    [COL_WHITE]  = RGB(31, 31, 31),
    [COL_GRAY]   = RGB(20, 19, 24),
    [COL_S0]     = RGB(28, 22, 31),
    [COL_S1]     = RGB(21, 13, 28),
    [COL_S2]     = RGB(14, 7, 20),
    [COL_S3]     = RGB(8, 3, 13),
    [COL_BLIP1]  = RGB(2, 9, 5),
    [COL_BLIP2]  = RGB(4, 16, 8),
    [COL_BLIP3]  = RGB(8, 25, 12),
    [COL_BLIP4]  = RGB(20, 31, 20),
    [COL_HILITE] = RGB(31, 26, 4),
};

// Colors the highlight cycles through (gold to white and back)
static const u16 sHiliteColors[8] =
{
    RGB(31, 26, 4), RGB(31, 27, 9), RGB(31, 28, 15), RGB(31, 30, 24),
    RGB(31, 31, 31), RGB(31, 30, 24), RGB(31, 28, 15), RGB(31, 27, 9),
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
    [WIN_HEADER] = {
        .bg = 0, .tilemapLeft = 0, .tilemapTop = 0, .width = 30, .height = 3,
        .paletteNum = 0, .baseBlock = 1
    },
    [WIN_RADAR] = {
        .bg = 0, .tilemapLeft = 0, .tilemapTop = 3, .width = 15, .height = 14,
        .paletteNum = 0, .baseBlock = 91
    },
    [WIN_LIST] = {
        .bg = 0, .tilemapLeft = 15, .tilemapTop = 3, .width = 15, .height = 14,
        .paletteNum = 0, .baseBlock = 301
    },
    [WIN_INFO] = {
        .bg = 0, .tilemapLeft = 0, .tilemapTop = 17, .width = 30, .height = 3,
        .paletteNum = 0, .baseBlock = 511
    },
    DUMMY_WIN_TEMPLATE
};

static const u8 sText_Title[] = _("NU-RADAR");
static const u8 sText_Hint[] = _("{A_BUTTON}PING  {B_BUTTON}EXIT");
static const u8 sText_Yen[] = _("¥");
static const u8 sText_PixLabel[] = _("PIX ");
static const u8 sText_Slash[] = _("/");
static const u8 sText_NoneLine1[] = _("No TRAINERS are offering");
static const u8 sText_NoneLine2[] = _("a PIX right now.");
static const u8 sText_NoneLine3[] = _("Check back later!");
static const u8 sText_NoneInfo1[] = _("Every TRAINER on the radar");
static const u8 sText_NoneInfo2[] = _("has already paid you.");
static const u8 sText_Space[] = _(" ");
static const u8 sText_Arrow[] = _("{RIGHT_ARROW}");

static void Task_RadarFadeIn(u8 taskId);
static void Task_RadarMain(u8 taskId);
static void Task_RadarExit(u8 taskId);
static void DrawList(void);
static void DrawInfo(void);

// ---- Pixel drawing --------------------------------------------------------

// Writes one pixel into the radar window's 4bpp tile data (bounds-checked).
static void PutPixel(s32 x, s32 y, u8 color)
{
    u8 *p;

    if ((u32)x >= RADAR_W || (u32)y >= RADAR_H)
        return;
    p = sPixels + ((((y >> 3) * RADAR_TILES_W + (x >> 3)) << 5) + ((y & 7) << 2) + ((x & 7) >> 1));
    if (x & 1)
        *p = (*p & 0x0F) | (color << 4);
    else
        *p = (*p & 0xF0) | color;
}

static void DrawCircle(s32 cx, s32 cy, s32 r, u8 color)
{
    s32 x = r;
    s32 y = 0;
    s32 err = 1 - r;

    while (x >= y)
    {
        PutPixel(cx + x, cy + y, color);
        PutPixel(cx + y, cy + x, color);
        PutPixel(cx - y, cy + x, color);
        PutPixel(cx - x, cy + y, color);
        PutPixel(cx - x, cy - y, color);
        PutPixel(cx - y, cy - x, color);
        PutPixel(cx + y, cy - x, color);
        PutPixel(cx + x, cy - y, color);
        y++;
        if (err < 0)
        {
            err += 2 * y + 1;
        }
        else
        {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

static void DrawRadialLine(u8 angle, s32 r0, s32 r1, u8 color)
{
    s32 c = Cos(angle, 256);
    s32 s = Sin(angle, 256);
    s32 r;

    for (r = r0; r <= r1; r++)
        PutPixel(CX + ((c * r) >> 8), CY - ((s * r) >> 8), color);
}

static u8 GetBlipColor(u8 flash)
{
    if (flash >= 24)
        return COL_BLIP4;
    if (flash >= 14)
        return COL_BLIP3;
    if (flash >= 6)
        return COL_BLIP2;
    return COL_BLIP1;
}

static void DrawPlus(s32 x, s32 y, u8 color)
{
    PutPixel(x, y, color);
    PutPixel(x - 1, y, color);
    PutPixel(x + 1, y, color);
    PutPixel(x, y - 1, color);
    PutPixel(x, y + 1, color);
}

static void DrawRadar(void)
{
    s32 i, k, r;

    sPixels = gWindows[WIN_RADAR].tileData;
    FillWindowPixelBuffer(WIN_RADAR, PIXEL_FILL(COL_BG));

    // range rings, dotted crosshair and tick marks
    DrawCircle(CX, CY, 13, COL_RING);
    DrawCircle(CX, CY, 26, COL_RING);
    DrawCircle(CX, CY, 39, COL_RING);
    DrawCircle(CX, CY, RADIUS, COL_RING2);
    for (r = -RADIUS; r <= RADIUS; r += 2)
    {
        PutPixel(CX + r, CY, COL_RING);
        PutPixel(CX, CY + r, COL_RING);
    }
    for (r = RADIUS - 4; r < RADIUS; r++)
    {
        PutPixel(CX + r, CY, COL_RING2);
        PutPixel(CX - r, CY, COL_RING2);
        PutPixel(CX, CY + r, COL_RING2);
        PutPixel(CX, CY - r, COL_RING2);
    }

    // slow ambient ring that expands from the player
    r = (sRadar.frame >> 1) % (RADIUS + 20);
    if (r < RADIUS)
        DrawCircle(CX, CY, r, COL_RING2);

    // sweep wedge: faint tail first, bright leading edge last
    for (k = TRAIL - 1; k >= 0; k--)
    {
        u8 color = (k < 3) ? COL_S0 : (k < 8) ? COL_S1 : (k < 15) ? COL_S2 : COL_S3;

        DrawRadialLine((u8)(sRadar.sweep - k), 2, RADIUS - 1, color);
    }

    // blips
    for (i = 0; i < sRadar.count; i++)
    {
        struct Blip *blip = &sRadar.blips[i];
        s32 bx = CX + blip->ox;
        s32 by = CY + blip->oy;

        if (i == sRadar.selected)
        {
            DrawCircle(bx, by, 4 + ((sRadar.frame >> 3) & 3), COL_HILITE);
            DrawPlus(bx, by, COL_BLIP4);
        }
        else if (blip->flash > 0)
        {
            DrawPlus(bx, by, GetBlipColor(blip->flash));
            if (blip->flash >= 24)
            {
                PutPixel(bx - 1, by - 1, COL_BLIP3);
                PutPixel(bx + 1, by - 1, COL_BLIP3);
                PutPixel(bx - 1, by + 1, COL_BLIP3);
                PutPixel(bx + 1, by + 1, COL_BLIP3);
            }
        }
        else
        {
            PutPixel(bx, by, COL_BLIP1);
            PutPixel(bx + 1, by, COL_BLIP1);
        }
    }

    // ping wave
    if (sRadar.pingRadius > 0)
    {
        DrawCircle(CX, CY, sRadar.pingRadius, COL_BLIP4);
        if (sRadar.pingRadius > 2)
            DrawCircle(CX, CY, sRadar.pingRadius - 2, COL_BLIP3);
        if (sRadar.pingRadius > 4)
            DrawCircle(CX, CY, sRadar.pingRadius - 4, COL_BLIP1);
    }

    // the player
    DrawPlus(CX, CY, COL_WHITE);
    DrawCircle(CX, CY, 3, COL_RING2);

    CopyWindowToVram(WIN_RADAR, COPYWIN_GFX);
}

// ---- Text helpers ---------------------------------------------------------

static void PrintText(u8 win, u8 font, s32 x, s32 y, u8 bg, u8 fg, u8 shadow, const u8 *str)
{
    u8 colors[3];

    colors[0] = bg;
    colors[1] = fg;
    colors[2] = shadow;
    AddTextPrinterParameterized3(win, font, x, y, colors, TEXT_SKIP_DRAW, str);
}

// Writes "¥1234" into dst and returns the end pointer
static u8 *BufferYen(u8 *dst, u32 value)
{
    u8 *ptr = StringCopy(dst, sText_Yen);

    return ConvertIntToDecimalStringN(ptr, value, STR_CONV_MODE_LEFT_ALIGN, 6);
}

static void PrintRightAligned(u8 win, u8 font, s32 right, s32 y, u8 bg, u8 fg, u8 shadow, const u8 *str)
{
    PrintText(win, font, right - GetStringWidth(font, str, 0), y, bg, fg, shadow, str);
}

static void DrawHeader(void)
{
    FillWindowPixelBuffer(WIN_HEADER, PIXEL_FILL(COL_PURPLE));
    PrintText(WIN_HEADER, FONT_NORMAL, 8, 4, COL_PURPLE, COL_WHITE, COL_DARK, sText_Title);
    PrintText(WIN_HEADER, FONT_SMALL, 138, 8, COL_PURPLE, COL_WHITE, COL_DARK, sText_Hint);
    CopyWindowToVram(WIN_HEADER, COPYWIN_GFX);
}

static void DrawList(void)
{
    s32 row;

    FillWindowPixelBuffer(WIN_LIST, PIXEL_FILL(COL_BG));

    if (sRadar.count == 0)
    {
        PrintText(WIN_LIST, FONT_SMALL, 8, 36, COL_BG, COL_GRAY, COL_BG, sText_NoneLine1);
        PrintText(WIN_LIST, FONT_SMALL, 8, 50, COL_BG, COL_GRAY, COL_BG, sText_NoneLine2);
        PrintText(WIN_LIST, FONT_SMALL, 8, 70, COL_BG, COL_WHITE, COL_BG, sText_NoneLine3);
    }

    for (row = 0; row < LIST_ROWS; row++)
    {
        s32 index = sRadar.scroll + row;
        struct Blip *blip;
        bool8 selected;
        s32 y = row * ROW_HEIGHT;
        u8 bg;

        if (index >= sRadar.count)
            break;

        blip = &sRadar.blips[index];
        selected = (index == sRadar.selected);
        bg = selected ? COL_PURPLE : COL_BG;

        if (selected)
            FillWindowPixelRect(WIN_LIST, COL_PURPLE, 0, y, RADAR_W, ROW_HEIGHT);

        StringCopy(gStringVar4, gTrainers[blip->trainerId].trainerName);
        PrintText(WIN_LIST, FONT_SMALL, selected ? 10 : 6, y + 2, bg, selected ? COL_WHITE : COL_GRAY, bg, gStringVar4);
        if (selected)
            PrintText(WIN_LIST, FONT_SMALL, 1, y + 2, bg, COL_HILITE, bg, sText_Arrow);

        BufferYen(gStringVar4, blip->prize);
        PrintRightAligned(WIN_LIST, FONT_SMALL, RADAR_W - 3, y + 2, bg, selected ? COL_HILITE : COL_BLIP3, bg, gStringVar4);
    }

    CopyWindowToVram(WIN_LIST, COPYWIN_GFX);
}

static void DrawInfo(void)
{
    u8 *ptr;

    FillWindowPixelBuffer(WIN_INFO, PIXEL_FILL(COL_DARK));

    if (sRadar.count == 0)
    {
        PrintText(WIN_INFO, FONT_SMALL, 8, 1, COL_DARK, COL_WHITE, COL_DARK, sText_NoneInfo1);
        PrintText(WIN_INFO, FONT_SMALL, 8, 12, COL_DARK, COL_GRAY, COL_DARK, sText_NoneInfo2);
    }
    else
    {
        struct Blip *blip = &sRadar.blips[sRadar.selected];

        // line 1: class and name, selection counter on the right
        ptr = StringCopy(gStringVar4, gTrainerClassNames[gTrainers[blip->trainerId].trainerClass]);
        ptr = StringCopy(ptr, sText_Space);
        StringCopy(ptr, gTrainers[blip->trainerId].trainerName);
        PrintText(WIN_INFO, FONT_SMALL, 8, 1, COL_DARK, COL_WHITE, COL_DARK, gStringVar4);

        ptr = ConvertIntToDecimalStringN(gStringVar1, sRadar.selected + 1, STR_CONV_MODE_LEFT_ALIGN, 2);
        ptr = StringCopy(ptr, sText_Slash);
        ConvertIntToDecimalStringN(ptr, sRadar.count, STR_CONV_MODE_LEFT_ALIGN, 2);
        PrintRightAligned(WIN_INFO, FONT_SMALL, 232, 1, COL_DARK, COL_GRAY, COL_DARK, gStringVar1);

        // line 2: location on the left, prize on the right
        GetMapName(gStringVar4, blip->mapSec, 0);
        PrintText(WIN_INFO, FONT_SMALL, 8, 12, COL_DARK, COL_GRAY, COL_DARK, gStringVar4);

        ptr = StringCopy(gStringVar2, sText_PixLabel);
        BufferYen(ptr, blip->prize);
        PrintRightAligned(WIN_INFO, FONT_SMALL, 232, 12, COL_DARK, COL_HILITE, COL_DARK, gStringVar2);
    }

    CopyWindowToVram(WIN_INFO, COPYWIN_GFX);
}

// ---- Data -----------------------------------------------------------------

// Base prize for beating a trainer (same formula the battle uses, without bonuses)
static u32 GetTrainerPrize(u16 trainerId)
{
    const struct Trainer *trainer = &gTrainers[trainerId];
    u32 level = 0;
    u32 i = 0;

    if (trainer->partySize != 0)
    {
        u32 last = trainer->partySize - 1;

        switch (trainer->partyFlags)
        {
        case 0:
            level = trainer->party.NoItemDefaultMoves[last].lvl;
            break;
        case F_TRAINER_PARTY_CUSTOM_MOVESET:
            level = trainer->party.NoItemCustomMoves[last].lvl;
            break;
        case F_TRAINER_PARTY_HELD_ITEM:
            level = trainer->party.ItemDefaultMoves[last].lvl;
            break;
        case F_TRAINER_PARTY_CUSTOM_MOVESET | F_TRAINER_PARTY_HELD_ITEM:
            level = trainer->party.ItemCustomMoves[last].lvl;
            break;
        }
    }

    for (; gTrainerMoneyTable[i].classId != 0xFF; i++)
    {
        if (gTrainerMoneyTable[i].classId == trainer->trainerClass)
            break;
    }
    return 4 * level * gTrainerMoneyTable[i].value;
}

static s32 GetMapCenterX(u16 mapSec)
{
    return gRegionMapEntries[mapSec].x + gRegionMapEntries[mapSec].width / 2;
}

static s32 GetMapCenterY(u16 mapSec)
{
    return gRegionMapEntries[mapSec].y + gRegionMapEntries[mapSec].height / 2;
}

// Finds trainers that have not paid yet, sorts them by distance from the player,
// then picks up to MAX_BLIPS spread from the nearest to the farthest.
static void BuildBlips(void)
{
    u16 playerSec = gMapHeader.regionMapSectionId;
    s32 playerX, playerY;
    s32 total = 0;
    s32 i, j, count;

    if (playerSec >= MAPSEC_NONE)
        playerSec = MAPSEC_LITTLEROOT_TOWN;
    playerX = GetMapCenterX(playerSec);
    playerY = GetMapCenterY(playerSec);

    for (i = 0; i < NUM_RADAR_TRAINERS; i++)
    {
        s32 dx, dy;
        struct Candidate cand;

        if (HasTrainerBeenFought(sRadarTrainers[i].trainerId))
            continue;
        dx = GetMapCenterX(sRadarTrainers[i].mapSec) - playerX;
        dy = GetMapCenterY(sRadarTrainers[i].mapSec) - playerY;
        cand.index = i;
        cand.dist = dx * dx + dy * dy;

        // insertion sort (nearest first)
        for (j = total; j > 0 && sCandidates[j - 1].dist > cand.dist; j--)
            sCandidates[j] = sCandidates[j - 1];
        sCandidates[j] = cand;
        total++;
    }

    count = (total < MAX_BLIPS) ? total : MAX_BLIPS;
    sRadar.count = count;

    for (i = 0; i < count; i++)
    {
        const struct NuRadarEntry *entry = &sRadarTrainers[sCandidates[(i * total) / count].index];
        struct Blip *blip = &sRadar.blips[i];
        s32 ox = ((GetMapCenterX(entry->mapSec) - playerX) * 7) / 4 + ((entry->trainerId * 5) % 7) - 3;
        s32 oy = ((GetMapCenterY(entry->mapSec) - playerY) * 7) / 4 + ((entry->trainerId * 3) % 7) - 3;

        // blips too close to the player's marker are moved onto a small ring around it
        if (ox * ox + oy * oy < 100)
        {
            u8 angle = (entry->trainerId * 53) & 0xFF;
            s32 ring = 9 + (entry->trainerId % 4) * 2;

            ox = (Cos(angle, 256) * ring) >> 8;
            oy = -((Sin(angle, 256) * ring) >> 8);
        }

        // keep the blip inside the circle
        while (ox * ox + oy * oy > BLIP_LIMIT_SQ)
        {
            ox = (ox * 7) / 8;
            oy = (oy * 7) / 8;
        }

        blip->ox = ox;
        blip->oy = oy;
        blip->dist2 = ox * ox + oy * oy;
        blip->trainerId = entry->trainerId;
        blip->mapSec = entry->mapSec;
        blip->flash = 0;
        blip->cooldown = 0;
        blip->prize = GetTrainerPrize(entry->trainerId);
    }
}

// ---- Tasks ----------------------------------------------------------------

static void UpdateSweepAndPing(void)
{
    s32 dirX = Cos(sRadar.sweep, 256);
    s32 dirY = -Sin(sRadar.sweep, 256);
    s32 i;

    sRadar.sweep++;
    sRadar.frame++;

    for (i = 0; i < sRadar.count; i++)
    {
        struct Blip *blip = &sRadar.blips[i];
        s32 perp = blip->ox * dirY - blip->oy * dirX;
        s32 dot = blip->ox * dirX + blip->oy * dirY;

        if (blip->flash > 0)
            blip->flash--;
        if (blip->cooldown > 0)
            blip->cooldown--;
        if (perp < 0)
            perp = -perp;

        // the beam just passed over this blip
        if (dot > 0 && perp < 512 && blip->cooldown == 0)
        {
            blip->flash = FLASH_MAX;
            blip->cooldown = 90;
            if (i == sRadar.selected)
                PlaySE(SE_PIN);
        }
    }

    if (sRadar.pingRadius > 0)
    {
        s32 prev = sRadar.pingRadius;

        sRadar.pingRadius += 3;
        for (i = 0; i < sRadar.count; i++)
        {
            struct Blip *blip = &sRadar.blips[i];

            if (prev * prev < blip->dist2 && blip->dist2 <= sRadar.pingRadius * sRadar.pingRadius)
            {
                blip->flash = FLASH_MAX;
                if (i == sRadar.selected)
                    PlaySE(SE_PIN);
            }
        }
        if (sRadar.pingRadius > RADIUS + 6)
            sRadar.pingRadius = 0;
    }

    // animated highlight color (only once the fade-in is finished)
    if (!gPaletteFade.active && (sRadar.frame & 3) == 0)
        LoadPalette(&sHiliteColors[(sRadar.frame >> 2) & 7], BG_PLTT_ID(0) + COL_HILITE, sizeof(u16));
}

static void ChangeSelection(s32 newSelection)
{
    sRadar.selected = newSelection;
    if (sRadar.selected < sRadar.scroll)
        sRadar.scroll = sRadar.selected;
    else if (sRadar.selected >= sRadar.scroll + LIST_ROWS)
        sRadar.scroll = sRadar.selected - LIST_ROWS + 1;
    PlaySE(SE_DEX_SCROLL);
    DrawList();
    DrawInfo();
}

static void Task_RadarFadeIn(u8 taskId)
{
    DrawRadar();
    UpdateSweepAndPing();
    if (!gPaletteFade.active)
        gTasks[taskId].func = Task_RadarMain;
}

static void Task_RadarMain(u8 taskId)
{
    UpdateSweepAndPing();

    if (JOY_NEW(B_BUTTON))
    {
        PlaySE(SE_PC_OFF);
        BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
        gTasks[taskId].func = Task_RadarExit;
        return;
    }

    if (sRadar.count != 0)
    {
        if (KEY_REPT(DPAD_UP))
            ChangeSelection(sRadar.selected == 0 ? sRadar.count - 1 : sRadar.selected - 1);
        else if (KEY_REPT(DPAD_DOWN))
            ChangeSelection(sRadar.selected + 1 >= sRadar.count ? 0 : sRadar.selected + 1);
        else if (JOY_NEW(A_BUTTON) && sRadar.pingRadius == 0)
        {
            sRadar.pingRadius = 1;
            sRadar.blips[sRadar.selected].flash = FLASH_MAX;
            PlaySE(SE_DEX_SEARCH);
        }
    }

    DrawRadar();
}

static void Task_RadarExit(u8 taskId)
{
    DrawRadar();
    UpdateSweepAndPing();
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

void CB2_InitNuRadar(void)
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
        LoadPalette(sRadarPal, BG_PLTT_ID(0), sizeof(sRadarPal));
        sRadar = (struct NuRadar){0};
        BuildBlips();
        PutWindowTilemap(WIN_HEADER);
        PutWindowTilemap(WIN_RADAR);
        PutWindowTilemap(WIN_LIST);
        PutWindowTilemap(WIN_INFO);
        DrawHeader();
        DrawList();
        DrawInfo();
        DrawRadar();
        CopyWindowToVram(WIN_HEADER, COPYWIN_FULL);
        CopyWindowToVram(WIN_RADAR, COPYWIN_FULL);
        CopyWindowToVram(WIN_LIST, COPYWIN_FULL);
        CopyWindowToVram(WIN_INFO, COPYWIN_FULL);
        gMain.state++;
        break;
    case 4:
        CreateTask(Task_RadarFadeIn, 0);
        BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, RGB(16, 1, 26)); // purple fade-in
        SetVBlankCallback(VBlankCB);
        SetMainCallback2(MainCB2);
        PlaySE(SE_DEX_SEARCH);
        return;
    }
}
