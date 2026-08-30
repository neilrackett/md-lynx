/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Atari Lynx emulator front-end for the SidecarTridge Multi-device
 * framebuffer template.
 *
 * The emulation core under lynx/ is Handy (Copyright (c) 1996-2004
 * K. Wilkins, zlib licence -- see lynx/license.txt), vendored via
 * xrip's pico-lynx. This file replaces pico-lynx's VGA/HDMI output,
 * USB/NES input, I2S audio and file browser with the template's
 * 320x200 framebuffer, Atari ST IKBD input, STE DMA / YM2149 audio and
 * SD-card cartridge storage.
 *
 * Display path: Mikey renders its 160x102 screen as four-bit pen indices
 * doubled horizontally, straight into the chunked framebuffer (see the
 * MD/Lynx changes in lynx/mikie.cpp); the chunky-to-planar pass fills
 * in the odd rows, so nothing here touches them. That is 320x204, of
 * which the first and last Lynx scanlines fall outside the ST screen,
 * so the remaining 320x200 fills it exactly. The menu offers a 1x mode
 * that centres the unscaled 160x102 image instead.
 * The Lynx's 16 on-screen colours go straight to the ST/STE shifter
 * palette, so no colour is quantised away.
 *
 * Cartridge images are far too big for RAM (up to 512 KB against a
 * 192 KB region that already holds two framebuffers and the emulated
 * Lynx's 64 KB of system RAM), so the selected .lnx is programmed into
 * the ROM_TEMP flash window at load time and CCart reads it in place.
 */

#include <stdint.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

extern "C" {
#include "aconfig.h"
#include "audio.h"
#include "cart_shared.h"
#include "constants.h"
#include "debug.h"
#include "fb.h"
#include "fb_blit.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "ff.h"
#include "ikbd.h"
#include "lynxemu.h"
#include "palette.h"
#include "settings/settings.h"
#include "xpad.h"
#include "xpadin.h"
}

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "lynx/handy.h"

extern "C" {
extern const struct FB_FONT font8x8; /* defined in fb.c */
}

/* ============================ Configuration ============================== */

/* Font cell metrics (font8x8): 40 columns x 25 rows over 320x200. */
#define CELL_W 8
#define CELL_H 8
#define TEXT_COLS (FB_CHUNKED_W / CELL_W)
#define TEXT_ROWS (FB_CHUNKED_H / CELL_H)

/* Menu palette indices. Index 0 also drives the ST border, so it is the
 * background rather than the text colour. */
#define PEN_BG 0
#define PEN_TEXT 1
#define PEN_TITLE 2
#define PEN_HILITE 3
#define PEN_DIM 4
#define PEN_WARN 5

static const uint16_t MenuPalette[PALETTE_ENTRIES] = {
    PALETTE_RGB(0, 0, 2), /*  0 navy      -- background + ST border */
    PALETTE_RGB(7, 7, 7), /*  1 white     -- body text              */
    PALETTE_RGB(2, 7, 7), /*  2 cyan      -- titles                 */
    PALETTE_RGB(7, 7, 0), /*  3 yellow    -- selection bar          */
    PALETTE_RGB(4, 4, 5), /*  4 grey      -- footer / dim text      */
    PALETTE_RGB(7, 3, 2), /*  5 red       -- warnings               */
    PALETTE_RGB(0, 0, 4), PALETTE_RGB(0, 0, 0), PALETTE_RGB(0, 0, 0),
    PALETTE_RGB(0, 0, 0), PALETTE_RGB(0, 0, 0), PALETTE_RGB(0, 0, 0),
    PALETTE_RGB(0, 0, 0), PALETTE_RGB(0, 0, 0), PALETTE_RGB(0, 0, 0),
    PALETTE_RGB(0, 0, 0), /* 15 black                               */
};

/* Staging chunk for the SD -> flash copy. Multiple of FLASH_SECTOR_SIZE
 * and small enough to borrow the chunked framebuffer for. */
#define LYNX_FLASH_CHUNK (32u * 1024u)

#define LYNX_MAX_GAMES 128

/* Mono sample ring between the emulator and the per-VBL audio fill.
 * One PAL VBL is 500 samples at the STE DMA rate; 2048 gives four
 * frames of slack against the 60 Hz -> 50 Hz frame pacing. */
#define AUDIO_RING_LEN 2048u

/* ST IKBD scancodes. */
#define ST_SC_ESC 0x01
#define ST_SC_P 0x19
#define ST_SC_ENTER 0x1C
#define ST_SC_CTRL 0x1D
#define ST_SC_LSHIFT 0x2A
#define ST_SC_RSHIFT 0x36
#define ST_SC_ALT 0x38
#define ST_SC_SPACE 0x39
#define ST_SC_F1 0x3B
#define ST_SC_F2 0x3C
#define ST_SC_F3 0x3D
#define ST_SC_CLRHOME 0x47
#define ST_SC_UP 0x48
#define ST_SC_LEFT 0x4B
#define ST_SC_RIGHT 0x4D
#define ST_SC_DOWN 0x50
#define ST_SC_INSERT 0x52

/* ============================== State ==================================== */

/* Mikey's render target, which is the chunked framebuffer itself -- no
 * intermediate, which is what the RP2040 has no room for.
 *
 * At 2x it writes doubled pens at a 640-byte pitch, so Lynx scanline 1
 * lands on chunked row 0: the base sits one scanline BELOW the buffer,
 * in its guard band, and scanlines 0 and 101 land in the guards and are
 * never displayed. That is the "320x204 cropped top and bottom" the
 * port is built around.
 *
 * At 1x it writes 160 bytes on consecutive rows, centred in the 320x200
 * screen, and all 102 scanlines are visible with a border around them. */
#define LYNX_FB_BASE_2X (fb_chunked_buffer - MDLYNX_DISPLAY_PITCH(2))
#define LYNX_1X_X0 ((FB_CHUNKED_W - HANDY_SCREEN_WIDTH) / 2)   /* 80 */
#define LYNX_1X_Y0 ((FB_CHUNKED_H - HANDY_SCREEN_HEIGHT) / 2)  /* 49 */
#define LYNX_FB_BASE_1X \
  (fb_chunked_buffer + LYNX_1X_Y0 * FB_CHUNKED_W + LYNX_1X_X0)

/* Stereo scratch Mikey mixes into (system.h: gAudioBuffer). Reset to
 * empty before every UpdateFrame so the front-end knows exactly how
 * many samples that frame produced. Parked in the cart-region hole --
 * main RAM is entirely spoken for by the framebuffer path and the
 * emulator's heap. */
static uint16_t s_lynx_audio[HANDY_AUDIO_BUFFER_LENGTH]
    __cart_app_free("lynxaudio");

/* Mono ring feeding lynxemu_audio_fill. */
static int8_t s_audio_ring[AUDIO_RING_LEN] __cart_app_free("audioring");
static volatile uint32_t s_audio_head; /* producer (render_frame) */
static volatile uint32_t s_audio_tail; /* consumer (audio_fill)   */
static int8_t s_audio_last;            /* held on underrun        */

struct game_entry {
  char name[40];
  uint32_t size;
};
static struct game_entry s_games[LYNX_MAX_GAMES] __cart_app_free("games");
static int s_game_count;

static char s_folder[64] = "/lynx";

static CSystem *s_lynx;

/* Settings adjustable from the menu. */
static uint32_t s_volume = 16;    /* 0..20                              */
static uint32_t s_full_speed = 1; /* 1 = true 60 Hz, 0 = 50 Hz (slower) */
static uint32_t s_show_perf;      /* overlay emulation timings in play  */
static uint32_t s_scale = 2;      /* 2 = fill the screen, 1 = centred    */

/* The two cartridge pens the overlay panel borrows while it is open;
 * picked by choose_overlay_pens() below. */
static uint8_t s_ov_bg = 0, s_ov_ink = 1;

static int s_menu_active = 1;
static int s_about_active;
static int s_selected;      /* >= 0 game index, < 0 settings item */
static int s_loaded = -1;   /* index of the cartridge in flash    */
static uint32_t s_menu_left_at_tick;
static uint32_t s_tick;

/* 60 Hz emulated frames paced against the ST's 50 Hz VBL: six emulated
 * frames per five displayed ones. */
static uint32_t s_frame_acc;

/* Lynx pad state contributed by the keyboard; OR'd with the ST joystick. */
static uint16_t s_key_buttons;

/* Rolling performance sample, published once a second. */
static uint32_t s_perf_emu_us, s_perf_disp_us, s_perf_pub_us, s_perf_fps_x10;

/* =========================== Small helpers =============================== */

/* Decimal formatter -- the template deliberately keeps newlib's printf
 * family out of the image, so text is assembled by hand. Writes into the
 * END of `buf` and returns the first character. */
static const char *fmt_uint(uint32_t n, char *buf, size_t buf_sz) {
  char *p = buf + buf_sz;
  *--p = '\0';
  if (n == 0) {
    *--p = '0';
  } else {
    while (n > 0) {
      *--p = (char)('0' + (n % 10));
      n /= 10;
    }
  }
  return p;
}

/* Append `src` to `dst` (which must be NUL-terminated), never writing
 * past `cap` bytes total. */
static void str_append(char *dst, size_t cap, const char *src) {
  size_t n = strlen(dst);
  while (n + 1 < cap && *src) dst[n++] = *src++;
  dst[n] = '\0';
}

static void text_at(int col, int row, uint8_t pen, const char *s) {
  font_set_font(&font8x8);
  font_set_color(pen);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_LEFT);
  font_move((unsigned)(col * CELL_W), (unsigned)(row * CELL_H));
  font_print(s);
}

/* Centred text. The renderer already centres to the pixel under
 * FONT_ALIGN_CENTER, so pass the alignment through rather than
 * re-deriving a column from the string length. */
static void text_centred(int row, uint8_t pen, const char *s) {
  font_set_font(&font8x8);
  font_set_color(pen);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_CENTER);
  font_move(FB_CHUNKED_W / 2u, (unsigned)(row * CELL_H));
  font_print(s);
}

/* ======================= Palette: Lynx -> ST/STE ========================= */

/* The Lynx has 4 bits per channel, the STE shifter also 4 -- but the
 * STE keeps the LSB in the top bit of each nibble so that a plain ST,
 * which only decodes the low 3 bits, still sees the correct 3-bit
 * approximation. One palette word therefore serves both machines. */
static inline uint16_t lynx_to_st(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((((r >> 1) & 7u) << 8) | ((r & 1u) << 11) |
                    (((g >> 1) & 7u) << 4) | ((g & 1u) << 7) |
                    (((b >> 1) & 7u)) | ((b & 1u) << 3));
}

static void publish_lynx_palette(void) {
  const TPALETTE *p = s_lynx->mMikie->GetPalette();
  uint16_t entries[PALETTE_ENTRIES];
  for (int i = 0; i < PALETTE_ENTRIES; i++) {
    entries[i] = lynx_to_st(p[i].Colours.Red, p[i].Colours.Green,
                            p[i].Colours.Blue);
  }
  /* While the overlay is up, the two pens it borrowed are forced to a
   * readable pair. The picture behind loses the distinction between
   * them for as long as the menu is open -- they were picked as the
   * closest pair precisely so that costs the least. */
  if (s_menu_active || s_about_active) {
    entries[s_ov_bg] = PALETTE_RGB(0, 0, 2);
    entries[s_ov_ink] = PALETTE_RGB(7, 7, 7);
  }
  palette_set(entries);
}

/* =========================== Display scaling ============================= */

/* Point Mikey at the right place in the framebuffer for the current
 * scale and clear whatever the previous one left behind (at 1x the
 * border is never written again, and at 2x the odd rows are filled
 * during the planar conversion rather than here). Call after creating
 * CSystem and whenever the setting changes. */
/* Fill the odd rows from the even ones, in chunky space.
 *
 * Only needed when something has been drawn at full vertical
 * resolution over Mikey's output -- the menu panel or the timings
 * readout. In the ordinary case the odd rows are filled far more
 * cheaply during the chunky-to-planar pass (fb_publish_row_doubled),
 * and this is skipped entirely. */
static void __not_in_flash_func(double_rows)(void) {
  uint8_t *row = fb_chunked_buffer;
  for (int y = 0; y < FB_CHUNKED_H / 2; y++) {
    memcpy(row + FB_CHUNKED_W, row, FB_CHUNKED_W);
    row += 2 * FB_CHUNKED_W;
  }
}

static void apply_scale(void) {
  if (s_lynx == NULL) return;
  s_lynx->mMikie->SetDisplayScale((UBYTE)s_scale);
  gPrimaryFrameBuffer =
      (s_scale == 1) ? LYNX_FB_BASE_1X : LYNX_FB_BASE_2X;
  fb_chunked_clear(0);
}

/* ============================ Audio plumbing ============================= */

/* round(-2*log2(q/32)) style hand-tuned (chA, chB) YM volume pairs from
 * the 1988 Ghostbusters demo's SAMPLE1 table -- the same LUT
 * tools/wav_to_ym4.py bakes into .YMS files, indexed by the top 6 bits
 * of an unsigned 8-bit PCM sample. Two channels summing on the YM's
 * logarithmic DAC give ~6 effective bits where one channel gives 4. */
static const uint8_t GhostLut[64][2] = {
    {0, 0},  {0, 2},  {1, 2},  {2, 2},  {2, 3},  {1, 4},  {2, 4},  {2, 5},
    {0, 6},  {2, 6},  {3, 6},  {4, 6},  {2, 7},  {4, 7},  {5, 7},  {2, 8},
    {3, 8},  {4, 8},  {5, 8},  {2, 9},  {3, 9},  {4, 9},  {5, 9},  {6, 9},
    {7, 9},  {3, 10}, {4, 10}, {5, 10}, {6, 10}, {7, 10}, {0, 11}, {1, 11},
    {2, 11}, {4, 11}, {5, 11}, {6, 11}, {7, 11}, {8, 11}, {8, 11}, {9, 11},
    {9, 11}, {0, 12}, {1, 12}, {2, 12}, {3, 12}, {4, 12}, {5, 12}, {6, 12},
    {8, 12}, {8, 12}, {9, 12}, {9, 12}, {9, 12}, {10, 12}, {0, 13}, {2, 13},
    {3, 13}, {4, 13}, {5, 13}, {6, 13}, {7, 13}, {8, 13}, {8, 13}, {9, 13},
};

static inline uint32_t audio_avail(void) {
  return (s_audio_head - s_audio_tail) & (AUDIO_RING_LEN - 1u);
}

/* Fold the stereo frame Mikey just mixed into mono and push it onto the
 * ring. gAudioBuffer holds signed 16-bit values in a UWORD array (see
 * mikie.cpp UpdateSound); the >> 7 lands full scale on +-128, which the
 * clamp brings back to the signed 8-bit range the STE DMA wants. */
static void push_audio_frame(uint32_t entries) {
  const int32_t gain = (int32_t)s_volume;
  for (uint32_t i = 0; i + 1 < entries; i += 2) {
    const int32_t l = (int16_t)s_lynx_audio[i];
    const int32_t r = (int16_t)s_lynx_audio[i + 1];
    int32_t v = (((l + r) / 2) >> 7) * gain / 20;
    if (v > 127) v = 127;
    else if (v < -127) v = -127;
    const uint32_t next = (s_audio_head + 1u) & (AUDIO_RING_LEN - 1u);
    if (next == s_audio_tail) break; /* full: drop the excess */
    s_audio_ring[s_audio_head] = (int8_t)v;
    s_audio_head = next;
  }
}

static inline int8_t audio_pop(void) {
  if (s_audio_head == s_audio_tail) return s_audio_last; /* underrun: hold */
  s_audio_last = s_audio_ring[s_audio_tail];
  s_audio_tail = (s_audio_tail + 1u) & (AUDIO_RING_LEN - 1u);
  return s_audio_last;
}

void lynxemu_audio_fill(uint8_t *buf, uint32_t bytes) {
  const audio_mode_t mode = audio_get_mode();
  /* Nothing running, or the menu is up: real silence rather than a held
   * DC level (zero is silence for the DMA and the YM alike). */
  if (mode == AUDIO_MODE_SILENT || s_lynx == NULL || s_menu_active) {
    memset(buf, 0, bytes);
    return;
  }

  const bool dma = (mode == AUDIO_MODE_DMA);
  const uint32_t nsamp = dma ? bytes : bytes / 2u;
  if (nsamp == 0) return;

  /* Resample whatever the emulator produced onto exactly the number of
   * samples the m68k will consume this VBL. The window bounds tile
   * `avail` exactly, so nothing is skipped and a sustained tone keeps
   * its phase across fills. This covers both directions: at 60 Hz the
   * emulator slightly over-produces (500.4 vs 500 samples per VBL) and
   * this decimates with a box filter; in the 50 Hz mode it produces 417
   * and this stretches, which is the right answer because the whole
   * machine -- audio pitch included -- is running 17% slow. */
  uint32_t avail = audio_avail();
  if (avail > nsamp * 8u) avail = nsamp * 8u; /* bound a backlog */
  uint32_t taken = 0;
  for (uint32_t i = 0; i < nsamp; i++) {
    const uint32_t end = (avail * (i + 1u)) / nsamp;
    int32_t sum = 0;
    uint32_t n = 0;
    for (; taken < end; taken++) {
      sum += audio_pop();
      n++;
    }
    /* n == 0 means this output slot falls between input samples (or the
     * ring ran dry): hold the last level. */
    const int32_t v = n ? (sum / (int32_t)n) : s_audio_last;
    if (dma) {
      buf[i] = (uint8_t)(int8_t)v;
    } else {
      /* The YM's DAC is logarithmic, so the sample picks a hand-tuned
       * pair of channel volumes rather than scaling linearly. */
      const uint8_t idx = (uint8_t)((v + 128) >> 2);
      buf[2u * i] = GhostLut[idx][0];
      buf[2u * i + 1u] = GhostLut[idx][1];
    }
  }
}

/* ========================= Cartridge -> flash ============================ */

/* Program `size` bytes from `f` into the ROM_TEMP flash window. Reads
 * come from SD (which needs XIP live) so the park/erase/program cycle
 * runs a chunk at a time with the framebuffer borrowed as the staging
 * buffer. Returns true if the whole image was written. */
static bool flash_cartridge(FIL *f, uint32_t size) {
  const uint32_t base = (uint32_t)(uintptr_t)&_rom_temp_start - XIP_BASE;
  const uint32_t erase_len =
      (size + FLASH_SECTOR_SIZE - 1u) & ~(uint32_t)(FLASH_SECTOR_SIZE - 1u);

  /* One erase for the whole image: the SDK promotes a 64 KB-aligned
   * range to block erases, which is several times faster than walking
   * it sector by sector. */
  fb_core1_park();
  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(base, erase_len);
  restore_interrupts(ints);
  fb_core1_unpark();

  uint8_t *staging = fb_chunked_buffer;
  uint32_t done = 0;
  while (done < size) {
    uint32_t want = size - done;
    if (want > LYNX_FLASH_CHUNK) want = LYNX_FLASH_CHUNK;
    UINT br = 0;
    if (f_read(f, staging, want, &br) != FR_OK || br == 0) return false;
    /* Pad the final short chunk out to a whole page; erased flash is
     * 0xFF, which is also what an absent cartridge byte reads as. */
    uint32_t prog = (br + FLASH_PAGE_SIZE - 1u) & ~(uint32_t)(FLASH_PAGE_SIZE - 1u);
    if (prog > br) memset(staging + br, 0xFF, prog - br);

    fb_core1_park();
    ints = save_and_disable_interrupts();
    flash_range_program(base + done, staging, prog);
    restore_interrupts(ints);
    fb_core1_unpark();

    done += br;
    if (br < want) break; /* short read: end of file */
  }
  return done >= size;
}

/* ============================== Games list =============================== */

/* Cartridge images: .lnx (headered), .lyx, and the raw/BS93 homebrew
 * dumps that ship as .o or .bin. */
static bool is_cartridge(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot) return false;
  return strcasecmp(dot, ".lnx") == 0 || strcasecmp(dot, ".lyx") == 0 ||
         strcasecmp(dot, ".o") == 0 || strcasecmp(dot, ".bin") == 0;
}

static void scan_games(void) {
  SettingsConfigEntry *fe =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  if (fe && fe->value[0]) {
    strncpy(s_folder, fe->value, sizeof(s_folder) - 1);
    s_folder[sizeof(s_folder) - 1] = '\0';
  }

  s_game_count = 0;
  DIR dir;
  if (f_opendir(&dir, s_folder) != FR_OK) {
    DPRINTF("No cartridge folder '%s'\n", s_folder);
    return;
  }
  FILINFO fno;
  while (s_game_count < LYNX_MAX_GAMES && f_readdir(&dir, &fno) == FR_OK &&
         fno.fname[0]) {
    if (fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;
    if (fno.fname[0] == '.') continue; /* dot files / AppleDouble ._* */
    if (!is_cartridge(fno.fname)) continue;
    if (fno.fsize == 0 || fno.fsize > rom_temp_size()) continue;
    struct game_entry *g = &s_games[s_game_count++];
    strncpy(g->name, fno.fname, sizeof(g->name) - 1);
    g->name[sizeof(g->name) - 1] = '\0';
    g->size = (uint32_t)fno.fsize;
  }
  f_closedir(&dir);
  DPRINTF("Found %d cartridge(s) in %s\n", s_game_count, s_folder);
}

/* Strip the extension for display, truncating to the caller's column
 * budget -- the overlay panel is narrower than the full-screen menu. */
static void display_name(const struct game_entry *g, char *out, size_t cap,
                         int cols) {
  if (cols > 0 && cap > (size_t)cols + 1u) cap = (size_t)cols + 1u;
  strncpy(out, g->name, cap - 1);
  out[cap - 1] = '\0';
  char *dot = strrchr(out, '.');
  if (dot) *dot = '\0';
}

/* ================================= Menu ================================== */

#define UI_ITEM_VOLUME 0
#define UI_ITEM_SCALE 1
#define UI_ITEM_SPEED 2
#define UI_ITEM_PERF 3
#define UI_ITEM_ABOUT 4
#define UI_ITEM_EXIT 5
#define UI_ITEM_COUNT 6

static const char *SpeedNames[2] = {"50HZ SMOOTH", "60HZ TRUE"};
static const char *ScaleNames[2] = {"1X CENTRED", "2X FULL"};
static const char *OnOffNames[2] = {"OFF", "ON"};

/* Menu items live ABOVE the games in the same list: index >= 0 is a
 * cartridge, index -1 .. -UI_ITEM_COUNT are the settings rows. */
static void settings_label(int item, char *buf, size_t cap) {
  char digits[12];
  buf[0] = '\0';
  switch (item) {
    case UI_ITEM_VOLUME:
      str_append(buf, cap, "VOLUME: ");
      str_append(buf, cap, fmt_uint(s_volume, digits, sizeof(digits)));
      break;
    case UI_ITEM_SCALE:
      str_append(buf, cap, "SIZE: ");
      str_append(buf, cap, ScaleNames[s_scale == 2u ? 1 : 0]);
      break;
    case UI_ITEM_SPEED:
      str_append(buf, cap, "SPEED: ");
      str_append(buf, cap, SpeedNames[s_full_speed ? 1 : 0]);
      break;
    case UI_ITEM_PERF:
      str_append(buf, cap, "TIMINGS: ");
      str_append(buf, cap, OnOffNames[s_show_perf ? 1 : 0]);
      break;
    case UI_ITEM_ABOUT: str_append(buf, cap, "ABOUT"); break;
    case UI_ITEM_EXIT: str_append(buf, cap, "EXIT TO GEM"); break;
    default: break;
  }
}

/* The menu is one flat list with the settings block on top and the
 * cartridges below, the same layout as MD/Speccy. The selection is held
 * as a signed index (>= 0 cartridge, < 0 setting) so the two kinds never
 * collide, which makes the row number very nearly a plain offset: the
 * settings occupy rows 0..UI_ITEM_COUNT-1 in reverse declaration order,
 * so EXIT heads the list and VOLUME sits at the bottom of the block.
 *
 * A blank, unselectable row separates the two groups -- but only when
 * there are cartridges to separate from, hence menu_gap(). */
static int menu_gap(void) { return s_game_count > 0 ? 1 : 0; }
static int menu_rows_total(void) {
  return UI_ITEM_COUNT + menu_gap() + s_game_count;
}
static bool row_is_gap(int row) {
  return menu_gap() && row == UI_ITEM_COUNT;
}
static int sel_to_row(int sel) {
  return (sel >= 0) ? sel + UI_ITEM_COUNT + menu_gap() : sel + UI_ITEM_COUNT;
}
static int row_to_sel(int row) {
  return (row > UI_ITEM_COUNT) ? row - UI_ITEM_COUNT - menu_gap()
                               : row - UI_ITEM_COUNT;
}

static void ui_move(int dir) {
  const int total = menu_rows_total();
  int row = sel_to_row(s_selected) + dir;
  /* Wrap, then step over the separator if we landed on it. Bounded by
   * `total` so a list that were somehow all gaps still terminates. */
  for (int guard = 0; guard <= total; guard++) {
    if (row < 0) row = total - 1;
    else if (row >= total) row = 0;
    if (!row_is_gap(row)) break;
    row += dir;
  }
  s_selected = row_to_sel(row);
}

static void load_game(int index);

static void ui_adjust(int dir) {
  if (s_selected >= 0) return;
  switch (-s_selected - 1) {
    case UI_ITEM_VOLUME:
      if (dir > 0 && s_volume < 20) s_volume++;
      else if (dir < 0 && s_volume > 0) s_volume--;
      break;
    case UI_ITEM_SCALE:
      s_scale = (dir > 0) ? 2u : 1u;
      apply_scale();
      break;
    case UI_ITEM_SPEED: s_full_speed = (dir > 0); break;
    case UI_ITEM_PERF: s_show_perf = (dir > 0); break;
    default: break;
  }
}

static void ui_select(void) {
  if (s_selected >= 0) {
    load_game(s_selected);
    return;
  }
  switch (-s_selected - 1) {
    case UI_ITEM_ABOUT: s_about_active = 1; break;
    case UI_ITEM_EXIT: ikbd_request_boot_gem(); break;
    default: break;
  }
}

/* ---------------------------------------------------------------------
 * The menu is drawn two ways from one renderer.
 *
 * With a cartridge running it is a panel down the right-hand side, the
 * game still playing beside and behind it -- MD/Speccy's arrangement,
 * wider because there is more screen here. With no cartridge loaded
 * there is nothing to overlay, so it takes the whole screen and its own
 * palette.
 *
 * The panel case cannot bring its own colours: the shifter palette
 * belongs to the running cartridge. It borrows two of the cartridge's
 * sixteen pens instead (see choose_overlay_pens), which is why a style
 * carries pen numbers rather than the fixed PEN_* constants.
 * -------------------------------------------------------------------- */

/* Panel is 26 columns of the 40, leaving 14 (112 px) of game visible. */
#define OV_COL0 14
#define OV_COLS (TEXT_COLS - OV_COL0)

struct menu_style {
  int col0;      /* leftmost text column                     */
  int cols;      /* width in columns                         */
  uint8_t bg;    /* panel fill, and the ink of a selected row */
  uint8_t text;  /* cartridge names                          */
  uint8_t title; /* headings                                 */
  uint8_t hi;    /* selection bar                            */
  uint8_t dim;   /* settings rows and footers                */
};

/* The menu is always the same panel, whether or not a cartridge is
 * running -- one geometry, so it never moves under you. Only the pens
 * differ: over a running game it borrows two of the cartridge's, and
 * with nothing loaded it has the whole palette to itself. */
static struct menu_style menu_style_now(void) {
  struct menu_style st;
  st.col0 = OV_COL0;
  st.cols = OV_COLS;
  if (s_lynx != NULL) {
    st.bg = s_ov_bg;
    st.text = s_ov_ink;
    st.title = s_ov_ink;
    st.hi = s_ov_ink;
    st.dim = s_ov_ink;
  } else {
    st.bg = PEN_BG;
    st.text = PEN_TEXT;
    st.title = PEN_TITLE;
    st.hi = PEN_HILITE;
    st.dim = PEN_DIM;
  }
  return st;
}

/* Centre `s` within the style's column span. */
static void style_centred(const struct menu_style &st, int row, uint8_t pen,
                          const char *s) {
  int col = st.col0 + (st.cols - (int)strlen(s)) / 2;
  if (col < st.col0) col = st.col0;
  text_at(col, row, pen, s);
}

#define MENU_FIRST_ROW 3
#define MENU_ROWS 20

/* Longest cartridge name that fits: one column of indent either side. */
#define NAME_COLS(st) ((st).cols - 4)

static void draw_menu(const struct menu_style &st) {
  fb_fill_rect(st.col0 * CELL_W, 0, st.cols * CELL_W, FB_CHUNKED_H, st.bg);

  /* Title hard left, version hard right, sharing the top row. */
  text_at(st.col0 + 1, 1, st.title, "MD/LYNX");
  text_at(st.col0 + st.cols - 1 - (int)strlen(RELEASE_VERSION), 1, st.dim,
          RELEASE_VERSION);

  const int total = menu_rows_total();
  int first = sel_to_row(s_selected) - MENU_ROWS / 2;
  if (first > total - MENU_ROWS) first = total - MENU_ROWS;
  if (first < 0) first = 0;

  char line[TEXT_COLS + 1];
  for (int i = 0; i < MENU_ROWS; i++) {
    const int row_idx = first + i;
    if (row_idx >= total) break;
    const int y = MENU_FIRST_ROW + i;
    if (row_is_gap(row_idx)) continue;   /* separator: nothing to draw */
    const int item = row_to_sel(row_idx);
    const bool is_game = (item >= 0);
    const bool sel = (item == s_selected);

    if (is_game) {
      display_name(&s_games[item], line, sizeof(line), NAME_COLS(st));
    } else {
      settings_label(-item - 1, line, sizeof(line));
    }

    if (sel) {
      fb_fill_rect((st.col0 + 1) * CELL_W, y * CELL_H,
                   (st.cols - 2) * CELL_W, CELL_H, st.hi);
      text_at(st.col0 + 2, y, st.bg, line);
    } else {
      /* The cartridge currently in flash reads back in the title colour
       * so ESC-ing out of a game shows at a glance what is running. */
      const uint8_t pen =
          !is_game ? st.dim : (item == s_loaded ? st.title : st.text);
      text_at(st.col0 + 2, y, pen, line);
    }
  }

  if (s_game_count == 0) {
    style_centred(st, MENU_FIRST_ROW + UI_ITEM_COUNT + 1, PEN_WARN,
                  "NO CARTRIDGES");
    style_centred(st, MENU_FIRST_ROW + UI_ITEM_COUNT + 2, PEN_WARN, s_folder);
  }

  style_centred(st, TEXT_ROWS - 2, st.dim, "CURSORS  RETURN");
  style_centred(st, TEXT_ROWS - 1, st.dim, "ESC CLOSES");
}

/* Append the rolling frame rate as "N.M". */
static void append_fps(char *buf, size_t cap) {
  char digits[12];
  str_append(buf, cap, fmt_uint(s_perf_fps_x10 / 10u, digits, sizeof(digits)));
  str_append(buf, cap, ".");
  str_append(buf, cap, fmt_uint(s_perf_fps_x10 % 10u, digits, sizeof(digits)));
}

/* Format a microsecond count as "N.M ms" into `buf`. */
static void append_ms(char *buf, size_t cap, uint32_t us) {
  char digits[12];
  str_append(buf, cap, fmt_uint(us / 1000u, digits, sizeof(digits)));
  str_append(buf, cap, ".");
  str_append(buf, cap, fmt_uint((us % 1000u) / 100u, digits, sizeof(digits)));
  str_append(buf, cap, "MS");
}

/* Kept inside 26 columns so the same text serves the overlay panel. */
static void draw_about(const struct menu_style &st) {
  fb_fill_rect(st.col0 * CELL_W, 0, st.cols * CELL_W, FB_CHUNKED_H, st.bg);

  int y = 3;
  style_centred(st, y++, st.title, "MD/LYNX");
  style_centred(st, y++, st.dim, RELEASE_VERSION);
  y++;
  style_centred(st, y++, st.text, "LYNX EMULATOR FOR THE ST");
  style_centred(st, y++, st.text, "BY NEIL RACKETT");
  y++;
  style_centred(st, y++, st.dim, "CORE: HANDY, ZLIB");
  style_centred(st, y++, st.dim, "(C) K. WILKINS");
  style_centred(st, y++, st.dim, "VIA XRIP/PICO-LYNX");
  y++;

  char buf[TEXT_COLS + 1];
  buf[0] = '\0';
  str_append(buf, sizeof(buf), "SOUND: ");
  switch (audio_get_mode()) {
    case AUDIO_MODE_DMA: str_append(buf, sizeof(buf), "STE DMA"); break;
    case AUDIO_MODE_YM: str_append(buf, sizeof(buf), "YM2149"); break;
    default: str_append(buf, sizeof(buf), "DETECTING"); break;
  }
  style_centred(st, y++, st.text, buf);

  buf[0] = '\0';
  str_append(buf, sizeof(buf), "EMU ");
  append_ms(buf, sizeof(buf), s_perf_emu_us);
  str_append(buf, sizeof(buf), " FPS ");
  append_fps(buf, sizeof(buf));
  style_centred(st, y++, st.dim, buf);

  buf[0] = '\0';
  str_append(buf, sizeof(buf), "DSP ");
  append_ms(buf, sizeof(buf), s_perf_disp_us);
  str_append(buf, sizeof(buf), " PUB ");
  append_ms(buf, sizeof(buf), s_perf_pub_us);
  style_centred(st, y++, st.dim, buf);

  style_centred(st, TEXT_ROWS - 2, st.dim, "PRESS ANY KEY");
}

/* Pens the overlay borrows: the closest pair in the cartridge's own
 * palette, because collapsing two near-identical colours is the least
 * the picture can lose. Chosen once when the menu opens, so a game
 * animating its palette doesn't make the panel flicker. */
static void choose_overlay_pens(void) {
  if (s_lynx == NULL) return;
  const TPALETTE *p = s_lynx->mMikie->GetPalette();
  int best = 1 << 30;
  s_ov_bg = 0;
  s_ov_ink = 1;
  for (int i = 0; i < PALETTE_ENTRIES; i++) {
    for (int j = i + 1; j < PALETTE_ENTRIES; j++) {
      const int dr = (int)p[i].Colours.Red - (int)p[j].Colours.Red;
      const int dg = (int)p[i].Colours.Green - (int)p[j].Colours.Green;
      const int db = (int)p[i].Colours.Blue - (int)p[j].Colours.Blue;
      const int d = dr * dr + dg * dg + db * db;
      if (d < best) {
        best = d;
        s_ov_bg = (uint8_t)i;
        s_ov_ink = (uint8_t)j;
      }
    }
  }
}

/* Draw the menu or the About page as a panel over the running game. */
static void draw_overlay(void) {
  const struct menu_style st = menu_style_now();
  if (s_about_active) draw_about(st);
  else draw_menu(st);
}

static void draw_loading(const char *name) {
  fb_chunked_clear(PEN_BG);
  text_centred(11, PEN_TITLE, "LOADING");
  text_centred(13, PEN_TEXT, name);
  text_centred(15, PEN_DIM, "WRITING CARTRIDGE TO FLASH...");
  palette_set(MenuPalette);
  fb_publish();
}

/* ============================ Game loading =============================== */

static void load_game(int index) {
  if (index < 0 || index >= s_game_count) return;
  struct game_entry *g = &s_games[index];

  char shown[TEXT_COLS + 1];
  display_name(g, shown, sizeof(shown), TEXT_COLS - 4);
  draw_loading(shown);

  char path[128];
  path[0] = '\0';
  str_append(path, sizeof(path), s_folder);
  str_append(path, sizeof(path), "/");
  str_append(path, sizeof(path), g->name);

  /* Reprogramming flash is slow and destroys whatever is in ROM_TEMP,
   * so tear the old machine down first: CCart points straight into the
   * window we are about to erase. */
  if (s_lynx) {
    s_lynx->SaveEEPROM();
    delete s_lynx;
    s_lynx = NULL;
    s_loaded = -1;
  }

  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) {
    DPRINTF("Cannot open %s\n", path);
    return;
  }
  const bool ok = flash_cartridge(&f, g->size);
  f_close(&f);
  if (!ok) {
    DPRINTF("Flashing %s failed\n", path);
    return;
  }

  gAudioEnabled = TRUE;

  s_lynx = new CSystem((UBYTE *)&_rom_temp_start, g->size,
                       MIKIE_PIXEL_FORMAT_16BPP_565, HANDY_AUDIO_SAMPLE_FREQ);

  /* Cartridge-backed saves sit next to the image, as <name>.eep. */
  char eep[128];
  eep[0] = '\0';
  str_append(eep, sizeof(eep), s_folder);
  str_append(eep, sizeof(eep), "/");
  str_append(eep, sizeof(eep), shown);
  str_append(eep, sizeof(eep), ".eep");
  s_lynx->mEEPROM->SetFilename(eep);
  s_lynx->mEEPROM->Load();
  apply_scale();

  s_loaded = index;
  s_menu_active = 0;
  s_about_active = 0;
  s_menu_left_at_tick = s_tick;
  s_key_buttons = 0;
  s_frame_acc = 0;
  s_audio_head = s_audio_tail = 0;
  s_audio_last = 0;
  publish_lynx_palette();
  DPRINTF("Loaded %s (%u bytes)\n", g->name, (unsigned)g->size);
}

/* ================================ Input ================================== */

/* The Lynx pad is four directions plus A, B, Option 1, Option 2 and
 * Pause. The ST cursor cluster (and the port-1 joystick) drive the
 * d-pad; the two fire buttons sit under the keys nearest the cursors on
 * an ST keyboard, and the rest map to function keys. Return is left
 * alone -- it selects in the menu.
 *
 * Pause is on F3 as well as P. Several Lynx titles want Pause held
 * together with an Option button (screen flip for left-handed play is
 * the familiar one), and F1+P is a two-handed stretch on an ST keyboard
 * whereas F1/F2/F3 fall under one hand. P stays because it is the
 * obvious key to reach for. */
static uint16_t key_to_button(uint8_t sc) {
  switch (sc) {
    case ST_SC_UP: return BUTTON_UP;
    case ST_SC_DOWN: return BUTTON_DOWN;
    case ST_SC_LEFT: return BUTTON_LEFT;
    case ST_SC_RIGHT: return BUTTON_RIGHT;
    case ST_SC_ALT:
    case ST_SC_SPACE:
    case ST_SC_INSERT: return BUTTON_A;
    case ST_SC_CTRL:
    case ST_SC_LSHIFT:
    case ST_SC_RSHIFT:
    case ST_SC_CLRHOME: return BUTTON_B;
    case ST_SC_F1: return BUTTON_OPT1;
    case ST_SC_F2: return BUTTON_OPT2;
    case ST_SC_F3:
    case ST_SC_P: return BUTTON_PAUSE;
    default: return 0;
  }
}

/* Xpad button mask -> Lynx pad. The Lynx's Option 1 and Option 2 are
 * what a one-button ST joystick can never reach, so they get both a
 * face button and a shoulder each. Select and Guide are handled
 * separately: they toggle the menu, the pad's equivalent of ESC. */
static uint16_t xpad_to_lynx(uint16_t b) {
  uint16_t out = 0;
  if (b & XPAD_UP) out |= BUTTON_UP;
  if (b & XPAD_DOWN) out |= BUTTON_DOWN;
  if (b & XPAD_LEFT) out |= BUTTON_LEFT;
  if (b & XPAD_RIGHT) out |= BUTTON_RIGHT;
  if (b & XPAD_SOUTH) out |= BUTTON_A;
  if (b & XPAD_EAST) out |= BUTTON_B;
  if (b & (XPAD_WEST | XPAD_TL)) out |= BUTTON_OPT1;
  if (b & (XPAD_NORTH | XPAD_TR)) out |= BUTTON_OPT2;
  if (b & XPAD_START) out |= BUTTON_PAUSE;
  return out;
}

/* Open or close the menu. Shared by the ESC key and the pad's Select
 * button so the two can't drift apart. */
static void toggle_menu(void) {
  if (s_lynx == NULL) return; /* nothing to go back to yet */
  s_menu_active = !s_menu_active;
  if (s_menu_active) choose_overlay_pens();
  if (!s_menu_active) {
    s_menu_left_at_tick = s_tick;
    s_key_buttons = 0;
    publish_lynx_palette();
  } else {
    /* Drop any queued audio so play resumes from live samples rather
     * than replaying what was in flight when the menu came up. */
    s_audio_tail = s_audio_head;
    s_audio_last = 0;
  }
}

void lynxemu_handle_key(const ikbd_key_event_t *k) {
  const uint8_t sc = k->scancode;
  const bool press = k->is_press;

  /* The About page is modal: any press dismisses it and is swallowed. */
  if (s_about_active) {
    if (press) s_about_active = 0;
    return;
  }

  if (sc == ST_SC_ESC) {
    /* We own ESC (auto-exit is off), so it toggles the menu; leaving to
     * GEM is the menu's own Exit item. With no cartridge loaded there is
     * nothing to go back to, so the menu stays up. */
    if (press) toggle_menu();
    return;
  }

  if (s_menu_active) {
    if (!press) return;
    switch (sc) {
      case ST_SC_UP: ui_move(-1); break;
      case ST_SC_DOWN: ui_move(1); break;
      case ST_SC_LEFT: ui_adjust(-1); break;
      case ST_SC_RIGHT: ui_adjust(1); break;
      case ST_SC_ENTER:
      case ST_SC_SPACE:
      case ST_SC_INSERT:
      case ST_SC_CLRHOME: ui_select(); break;
      default: break;
    }
    return;
  }

  const uint16_t bit = key_to_button(sc);
  if (bit == 0) return;
  if (press) s_key_buttons |= bit;
  else s_key_buttons &= (uint16_t)~bit;
}

/* ============================== Main loop ================================ */

/* Advance the emulator by one 60 Hz frame, capturing its audio. */
static void run_one_frame(bool draw) {
  gAudioBufferPointer = 0;
  s_lynx->UpdateFrame(draw);
  push_audio_frame(gAudioBufferPointer);
}

static uint16_t pad_state(void);

/* The subset of the Lynx pad the menu reacts to. Masking keeps B and
 * the Option buttons from dismissing the About page. */
#define MENU_NAV                                                          \
  (BUTTON_UP | BUTTON_DOWN | BUTTON_LEFT | BUTTON_RIGHT | BUTTON_A |      \
   BUTTON_PAUSE)

static void drive_menu_from_pad(void) {
  static uint16_t prev;
  const uint16_t nav = (uint16_t)(pad_state() & MENU_NAV);
  const uint16_t edge = (uint16_t)(nav & ~prev);
  prev = nav;

  if (s_about_active) {
    if (edge) s_about_active = 0;
    return;
  }
  if (edge & BUTTON_UP) ui_move(-1);
  if (edge & BUTTON_DOWN) ui_move(1);
  if (edge & BUTTON_LEFT) ui_adjust(-1);
  if (edge & BUTTON_RIGHT) ui_adjust(1);
  /* Fire on the stick, South on a pad, or Start -- all "select". */
  if (edge & (BUTTON_A | BUTTON_PAUSE)) ui_select();
}

/* Select / Guide on an Xpad toggles the menu -- without it a pad player
 * would have to reach for the keyboard to change cartridge. Edge
 * triggered, and polled in both states rather than only in play. */
static void poll_xpad_menu_toggle(void) {
  static bool prev;
  uint16_t xb = 0;
  const bool now =
      xpadin_get(&xb) && (xb & (XPAD_SELECT | XPAD_MODE)) != 0u;
  if (now && !prev && !s_about_active) toggle_menu();
  prev = now;
}

/* The ST joystick and any Xpad, folded into one Lynx pad state. The
 * plain stick can only reach the d-pad and Button A; an Xpad reaches
 * everything (see xpad_to_lynx). The menu drives off this same mapping,
 * so there is one place where a physical control becomes a Lynx button. */
static uint16_t pad_state(void) {
  const uint8_t joy = ikbd_get_joystick();
  uint16_t b = 0;
  if (joy & 0x01) b |= BUTTON_UP;
  if (joy & 0x02) b |= BUTTON_DOWN;
  if (joy & 0x04) b |= BUTTON_LEFT;
  if (joy & 0x08) b |= BUTTON_RIGHT;
  if (joy & 0x80) b |= BUTTON_A;

  uint16_t xb;
  if (xpadin_get(&xb)) b |= xpad_to_lynx(xb);
  return b;
}

static uint16_t pad_buttons(void) {
  /* Ignore the pad for a few frames after the menu closes so the fire
   * that picked a cartridge doesn't leak into play. */
  if (s_tick < s_menu_left_at_tick + 10u) return 0;
  return pad_state();
}

static void draw_perf_overlay(void) {
  char buf[TEXT_COLS + 1];
  buf[0] = '\0';
  str_append(buf, sizeof(buf), "EMU ");
  append_ms(buf, sizeof(buf), s_perf_emu_us);
  str_append(buf, sizeof(buf), " FPS ");
  append_fps(buf, sizeof(buf));
  /* Pen 1 is whatever the cartridge put there, so the readout borrows a
   * colour rather than owning one -- it is a diagnostic, not chrome. */
  text_at(0, 0, 1, buf);
}

/* Sample emulation / display / publish costs over a one-second window
 * and republish the means for the About page and the overlay. */
static void account(uint32_t emu_us, uint32_t disp_us, uint32_t pub_us) {
  static uint32_t win_start, acc_emu, acc_disp, acc_pub, acc_frames;
  static bool started;
  if (!started) { /* first window starts here, not at time zero */
    started = true;
    win_start = time_us_32();
  }
  acc_emu += emu_us;
  acc_disp += disp_us;
  acc_pub += pub_us;
  acc_frames++;
  const uint32_t now = time_us_32();
  const uint32_t span = now - win_start;
  if (span >= 1000000u) {
    s_perf_emu_us = acc_emu / acc_frames;
    s_perf_disp_us = acc_disp / acc_frames;
    s_perf_pub_us = acc_pub / acc_frames;
    s_perf_fps_x10 = (uint32_t)(((uint64_t)acc_frames * 10000000u) / span);
    win_start = now;
    acc_emu = acc_disp = acc_pub = acc_frames = 0;
  }
}

void lynxemu_render_frame(void) {
  const uint32_t t0 = time_us_32();

  poll_xpad_menu_toggle();
  const bool overlay = (s_menu_active || s_about_active);
  if (overlay || s_lynx == NULL) {
    drive_menu_from_pad();
  }

  /* No cartridge yet: there is nothing to overlay onto, so the menu
   * takes the whole screen with its own palette. (drive_menu_from_pad
   * may have just loaded one, so this is re-tested rather than folded
   * into the branch above -- a cartridge picked this frame drops
   * straight into play.) */
  if (s_lynx == NULL) {
    /* Same panel, on an empty screen rather than over a game. */
    fb_chunked_clear(PEN_BG);
    draw_overlay();
    palette_set(MenuPalette);
    const uint32_t t1 = time_us_32();
    fb_publish();
    account(0, t1 - t0, time_us_32() - t1);
    s_tick++;
    return;
  }

  /* Play, or the menu overlaid on it. The game keeps running behind the
   * panel -- that is the point of an overlay -- but its controls are
   * held so navigating the menu doesn't also drive the game. */
  s_lynx->SetButtonData(overlay ? 0u : (s_key_buttons | pad_buttons()));

  /* Six emulated 60 Hz frames per five ST VBLs keeps game speed and
   * audio pitch correct; the intermediate frame is emulated but not
   * rendered. "50HZ SMOOTH" trades that for one frame per VBL, which
   * runs everything ~17% slow but never doubles up. */
  const uint32_t te0 = time_us_32();
  if (s_full_speed) {
    s_frame_acc += 6u;
    while (s_frame_acc >= 10u) { /* two frames due this VBL */
      run_one_frame(false);
      s_frame_acc -= 5u;
    }
    run_one_frame(true);
    s_frame_acc -= 5u;
  } else {
    run_one_frame(true);
  }
  const uint32_t emu_us = time_us_32() - te0;

  const uint32_t td0 = time_us_32();
  publish_lynx_palette();

  /* Two renders, in this order, and the order is the whole point.
   *
   * Mikey has drawn the game into the even rows only. Anything on top
   * of it -- the menu panel, the timings readout -- is drawn at full
   * vertical resolution, so the odd rows have to be filled BEFORE that,
   * not after: doubling afterwards would overwrite every odd row of
   * every glyph with a copy of the row above and halve the text. And
   * the row-doubling publish path can't carry an overlay at all, since
   * it only ever converts the even rows.
   *
   * So: complete the game image, draw over it, then transpose the lot.
   * The full transpose costs back the ~290 us the row-doubled path
   * saves, but only while something is being drawn over the game --
   * exactly when the frame budget doesn't matter. */
  const bool full_res = overlay || s_show_perf;
  if (full_res) {
    if (s_scale == 2) double_rows();   /* finish the game first */
    if (overlay) draw_overlay();       /* then draw on top of it */
    if (s_show_perf) draw_perf_overlay();
  }

  const uint32_t td1 = time_us_32();
  if (s_scale == 2 && !full_res) {
    fb_publish_row_doubled();
  } else {
    fb_publish();
  }
  account(emu_us, td1 - td0, time_us_32() - td1);
  s_tick++;
}

/* ================================ Init =================================== */

void lynxemu_init(void) {
  /* ESC is ours: it toggles the menu. Exit to GEM is the menu item,
   * which posts the same cart-bus command the auto-exit would have. */
  ikbd_set_esc_auto_exit(false);

  gPrimaryFrameBuffer = LYNX_FB_BASE_2X;
  gAudioBuffer = s_lynx_audio;
  gAudioEnabled = FALSE;

  s_audio_head = s_audio_tail = 0;

  scan_games();
  /* Open on the first cartridge, with the settings scrolled above it --
   * or on the bottom settings row when the card has nothing to run. */
  s_selected = (s_game_count > 0) ? 0 : -1;
  s_menu_active = 1;
  palette_set(MenuPalette);
}
