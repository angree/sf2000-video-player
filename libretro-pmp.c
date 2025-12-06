/*
 * PMP072 - A ZERO Player
 * - FAST video player with idx1 index support
 * - Auto-detect offset format
 * - Supports 3+ hour videos at 30fps (360000 frames)
 * - Amiga-style menu UI with Instructions & About
 * - Lock/unlock icons (key shape)
 * - LUT optimization for YCbCr->RGB (no multiplications)
 * - Color modes: Unchanged, Lifted, Gamma, Dithered, Warm, Night, Legacy
 * - Scrollable color mode submenu
 * - Dither2/Night+Dither2 modes (dither black for smooth transitions)
 * - Enhanced warm tint (more visible red shift)
 * - Built-in file browser (load videos from SD card)
 * by Grzegorz Korycki
 */

#include "libretro.h"
#include "tjpgd.h"
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>

/* SF2000 filesystem functions - DO NOT use standard fopen/fread! */
#define FS_O_RDONLY     0x0000
#define FS_O_WRONLY     0x0001
#define FS_O_RDWR       0x0002
#define FS_O_CREAT      0x0100
#define FS_O_TRUNC      0x0200

extern int fs_open(const char *path, int oflag, int perms);
extern int fs_close(int fd);
extern int64_t fs_lseek(int fd, int64_t offset, int whence);
extern ssize_t fs_read(int fd, void *buf, size_t nbyte);
extern ssize_t fs_write(int fd, const void *buf, size_t nbyte);
extern int fs_mkdir(const char *path, int mode);
extern int fs_opendir(const char *path);
extern int fs_closedir(int fd);
extern ssize_t fs_readdir(int fd, void *buffer);

/* S_ISREG and S_ISDIR macros for fs_readdir type field */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240
#define FRAME_PIXELS  (SCREEN_WIDTH * SCREEN_HEIGHT)
#define MAX_JPEG_SIZE (64 * 1024)
#define TJPGD_WORKSPACE_SIZE 4096

/* Audio settings */
#define AUDIO_SAMPLE_RATE 22050
#define MAX_AUDIO_BUFFER 2048

/* Audio ring buffer: ~1 second (like 042) */
#define AUDIO_RING_SIZE (22050 * 2)
#define AUDIO_REFILL_THRESHOLD (AUDIO_RING_SIZE / 2)

typedef uint16_t pixel_t;

/* Display framebuffer - decode directly here */
static pixel_t framebuffer[FRAME_PIXELS];

static uint8_t jpeg_buffer[MAX_JPEG_SIZE + 2];
static uint8_t tjpgd_work[TJPGD_WORKSPACE_SIZE];

/* Audio output buffer */
static int16_t audio_out_buffer[MAX_AUDIO_BUFFER * 2];

/* Audio ring buffer */
static uint8_t audio_ring[AUDIO_RING_SIZE];
static int aring_read = 0;
static int aring_write = 0;
static int aring_count = 0;

static retro_video_refresh_t video_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_audio_sample_batch_t audio_batch_cb = NULL;

static FILE *video_file = NULL;
static int is_playing = 0;
static uint32_t clip_fps = 30;
static uint32_t us_per_frame = 33333;

/* 360000 frames = 6.6 hours at 15fps, 3.3 hours at 30fps */
#define MAX_FRAMES 360000
static uint32_t frame_offsets[MAX_FRAMES];
static uint32_t frame_sizes[MAX_FRAMES];
static int total_frames = 0;

#define MAX_AUDIO_CHUNKS 360000
static uint32_t audio_offsets[MAX_AUDIO_CHUNKS];
static uint32_t audio_sizes[MAX_AUDIO_CHUNKS];
static int total_audio_chunks = 0;

/* Frame index - single index, no buffering */
static int current_frame_idx = 0;

/* Audio stream state */
static int audio_chunk_idx = 0;
static uint32_t audio_chunk_pos = 0;
static uint64_t audio_samples_sent = 0;
static uint32_t total_audio_bytes = 0;

/* Audio format */
static int audio_channels = 0;
static int audio_sample_rate = 0;
static int audio_bits = 0;
static int audio_bytes_per_sample = 0;
static int has_audio = 0;

/* Repeat timing (15fps content on 30fps display) */
static int repeat_count = 1;
static int repeat_counter = 0;

/* Playback control */
static int is_paused = 0;
static int prev_a = 0;
static int prev_b = 0;
static int prev_left = 0;
static int prev_right = 0;
static int prev_l = 0;
static int prev_r = 0;

/* Key lock: L+R held for 2 seconds toggles lock */
#define LOCK_HOLD_FRAMES (30 * 2)  /* 2 seconds at 30fps */
#define LOCK_INDICATOR_FRAMES (30 * 3)  /* show indicator for 3 seconds */
static int is_locked = 0;
static int lock_hold_counter = 0;  /* frames L+R held */
static int lock_indicator_timer = 0;  /* countdown for indicator display */

/* Color processing modes */
#define COLOR_MODE_UNCHANGED   0
#define COLOR_MODE_LIFTED16    1
#define COLOR_MODE_LIFTED32    2
#define COLOR_MODE_GAMMA_1_2   3
#define COLOR_MODE_GAMMA_1_5   4
#define COLOR_MODE_GAMMA_1_8   5
#define COLOR_MODE_DITHERED    6
#define COLOR_MODE_DITHER2     7
#define COLOR_MODE_WARM        8
#define COLOR_MODE_WARM_PLUS   9
#define COLOR_MODE_NIGHT       10
#define COLOR_MODE_NIGHT_PLUS  11
#define COLOR_MODE_NIGHT_DITHER 12
#define COLOR_MODE_NIGHT_DITHER2 13
#define COLOR_MODE_LEGACY      14
#define COLOR_MODE_COUNT       15
int color_mode = COLOR_MODE_UNCHANGED;  /* non-static for tjpgd.c access */
static const char *color_mode_names[COLOR_MODE_COUNT] = {
    "Unchanged", "Lift 16", "Lift 32",
    "Gamma 1.2", "Gamma 1.5", "Gamma 1.8",
    "Dithered", "Dither2",
    "Warm", "Warm+", "Night", "Night+",
    "Night+Dith", "Night+Dith2", "Legacy"
};

/* Color mode submenu */
static int color_submenu_active = 0;
static int color_submenu_scroll = 0;  /* scroll offset for modes list */

/* 4x4 Bayer dithering matrix (scaled for RGB565) */
static const int8_t bayer4x4[4][4] = {
    { -8,  0, -6,  2 },
    {  4, -4,  6, -2 },
    { -5,  3, -7,  1 },
    {  7, -1,  5, -3 }
};

/* Gamma lookup tables for 5-bit (R/B) and 6-bit (G) values */
/* For warm/night modes: separate R, G, B tables needed */
static uint8_t gamma_r5[COLOR_MODE_COUNT][32];
static uint8_t gamma_g6[COLOR_MODE_COUNT][64];
static uint8_t gamma_b5[COLOR_MODE_COUNT][32];  /* separate B for warm modes */

/* Menu overlay */
#define MENU_ITEMS 10
static int menu_active = 0;
static int menu_selection = 0;
static int prev_start = 0;
static int prev_up = 0;
static int prev_down = 0;
static int show_time = 1;     /* time display visible (ON by default) */
static int show_debug = 0;    /* debug panel visible (OFF by default) */
static int seek_position = 0;  /* 0-20 (0%, 5%, 10%, ... 100%) */
static int was_paused_before_menu = 0;  /* remember pause state */
static int submenu_active = 0;  /* 0=none, 1=instructions, 2=greetings, 3=file browser */
static int save_feedback_timer = 0;  /* frames to show "Settings Saved" message */
#define SAVE_FEEDBACK_FRAMES 60  /* 2 seconds at 30fps */
static const char *menu_labels[MENU_ITEMS] = {
    "Load File",       /* 0 */
    "Go to Position",  /* 1 */
    "Color Mode",      /* 2 */
    "Resume",          /* 3 */
    "Show Time",       /* 4 */
    "Debug Info",      /* 5 */
    "Restart",         /* 6 */
    "Save Settings",   /* 7 - NEW */
    "Instructions",    /* 8 */
    "About"            /* 9 */
};

/* File browser */
#define FB_MAX_FILES 64
#define FB_MAX_PATH 256
#define FB_MAX_NAME 64
#define FB_VISIBLE_ITEMS 15
#define FB_START_PATH "/mnt/sda1/VIDEOS"
#define SETTINGS_FILE "/mnt/sda1/VIDEOS/a0player.cfg"

static int file_browser_active = 0;
static char fb_current_path[FB_MAX_PATH] = FB_START_PATH;
static char system_directory[FB_MAX_PATH] = "";
static char fb_files[FB_MAX_FILES][FB_MAX_NAME];
static int fb_is_dir[FB_MAX_FILES];  /* 1 = directory, 0 = file */
static int fb_file_count = 0;
static int fb_selection = 0;
static int fb_scroll = 0;
static int no_file_loaded = 0;  /* 1 if started without file */
static char loaded_file_path[FB_MAX_PATH] = "";

/* Visual feedback icons */
#define ICON_NONE 0
#define ICON_SKIP_LEFT 1
#define ICON_SKIP_RIGHT 2
#define ICON_PAUSE 3
#define ICON_PLAY 4
#define ICON_LOCK 5
#define ICON_UNLOCK 6
#define ICON_SKIP_BACK_1M 7
#define ICON_SKIP_FWD_1M 8
#define ICON_FRAMES 30  /* show icon for 1 second */
static int icon_type = ICON_NONE;
static int icon_timer = 0;

/* Debug stats */
static int run_counter = 0;
static int decode_counter = 0;
static int runs_per_sec = 0;
static int decodes_per_sec = 0;
static int sec_counter = 0;

/* Video scaling (for small videos) */
static int video_width = 320;   /* detected from first frame */
static int video_height = 240;
static int scale_factor = 1;    /* 1, 2, or 3 */
static int offset_x = 0;        /* centering offset */
static int offset_y = 0;

typedef struct { uint8_t *data; uint32_t size; uint32_t pos; } jpeg_io_t;
static jpeg_io_t jpeg_io;

/* Font 5x7 */
static const unsigned char font[96][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},
    {0x3E,0x41,0x41,0x51,0x32},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},{0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},{0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
    {0x00,0x7F,0x10,0x28,0x44},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},{0x08,0x08,0x2A,0x1C,0x08},{0x08,0x1C,0x2A,0x08,0x08},
};

/* Initialize color transformation lookup tables */
static void init_color_tables(void) {
    /* 5-bit tables (R and B) */
    for (int i = 0; i < 32; i++) {
        float norm = i / 31.0f;
        int boosted;
        /* Unchanged - identity */
        gamma_r5[COLOR_MODE_UNCHANGED][i] = i;
        gamma_b5[COLOR_MODE_UNCHANGED][i] = i;
        /* Lifted 16 */
        gamma_r5[COLOR_MODE_LIFTED16][i] = 4 + (i * 27) / 31;
        gamma_b5[COLOR_MODE_LIFTED16][i] = 4 + (i * 27) / 31;
        /* Lifted 32 */
        gamma_r5[COLOR_MODE_LIFTED32][i] = 8 + (i * 23) / 31;
        gamma_b5[COLOR_MODE_LIFTED32][i] = 8 + (i * 23) / 31;
        /* Gamma modes */
        gamma_r5[COLOR_MODE_GAMMA_1_2][i] = (int)(31.0f * powf(norm, 0.833f) + 0.5f);
        gamma_b5[COLOR_MODE_GAMMA_1_2][i] = (int)(31.0f * powf(norm, 0.833f) + 0.5f);
        gamma_r5[COLOR_MODE_GAMMA_1_5][i] = (int)(31.0f * powf(norm, 0.667f) + 0.5f);
        gamma_b5[COLOR_MODE_GAMMA_1_5][i] = (int)(31.0f * powf(norm, 0.667f) + 0.5f);
        gamma_r5[COLOR_MODE_GAMMA_1_8][i] = (int)(31.0f * powf(norm, 0.556f) + 0.5f);
        gamma_b5[COLOR_MODE_GAMMA_1_8][i] = (int)(31.0f * powf(norm, 0.556f) + 0.5f);
        /* Dithered - identity */
        gamma_r5[COLOR_MODE_DITHERED][i] = i;
        gamma_b5[COLOR_MODE_DITHERED][i] = i;
        /* Dither2 - identity (dithers black too) */
        gamma_r5[COLOR_MODE_DITHER2][i] = i;
        gamma_b5[COLOR_MODE_DITHER2][i] = i;
        /* Warm - warm tint: R boost 15%, B reduced 40% */
        boosted = (i * 115) / 100; if (boosted > 31) boosted = 31;
        gamma_r5[COLOR_MODE_WARM][i] = boosted;
        gamma_b5[COLOR_MODE_WARM][i] = (i * 60) / 100;
        /* Warm+ - more warm: R boost 30%, B reduced 65% */
        boosted = (i * 130) / 100; if (boosted > 31) boosted = 31;
        gamma_r5[COLOR_MODE_WARM_PLUS][i] = boosted;
        gamma_b5[COLOR_MODE_WARM_PLUS][i] = (i * 35) / 100;
        /* Night - warm + dimmed to 63% brightness */
        boosted = (i * 73) / 100;  /* 63% * 115% */
        if (boosted > 31) boosted = 31;
        gamma_r5[COLOR_MODE_NIGHT][i] = boosted;
        gamma_b5[COLOR_MODE_NIGHT][i] = (i * 38) / 100;  /* 63% * 60% */
        /* Night+ - warm + dimmed to 27% brightness */
        boosted = (i * 31) / 100;  /* 27% * 115% */
        if (boosted > 31) boosted = 31;
        gamma_r5[COLOR_MODE_NIGHT_PLUS][i] = boosted;
        gamma_b5[COLOR_MODE_NIGHT_PLUS][i] = (i * 16) / 100;  /* 27% * 60% */
        /* Night+Dither - same as Night+ but with dithering applied */
        gamma_r5[COLOR_MODE_NIGHT_DITHER][i] = boosted;
        gamma_b5[COLOR_MODE_NIGHT_DITHER][i] = (i * 16) / 100;
        /* Night+Dither2 - same as Night+ but dithers black too */
        gamma_r5[COLOR_MODE_NIGHT_DITHER2][i] = boosted;
        gamma_b5[COLOR_MODE_NIGHT_DITHER2][i] = (i * 16) / 100;
        /* Legacy - identity (LUT not used, but fill anyway) */
        gamma_r5[COLOR_MODE_LEGACY][i] = i;
        gamma_b5[COLOR_MODE_LEGACY][i] = i;
    }
    /* 6-bit table (G) */
    for (int i = 0; i < 64; i++) {
        float norm = i / 63.0f;
        gamma_g6[COLOR_MODE_UNCHANGED][i] = i;
        gamma_g6[COLOR_MODE_LIFTED16][i] = 8 + (i * 55) / 63;
        gamma_g6[COLOR_MODE_LIFTED32][i] = 16 + (i * 47) / 63;
        gamma_g6[COLOR_MODE_GAMMA_1_2][i] = (int)(63.0f * powf(norm, 0.833f) + 0.5f);
        gamma_g6[COLOR_MODE_GAMMA_1_5][i] = (int)(63.0f * powf(norm, 0.667f) + 0.5f);
        gamma_g6[COLOR_MODE_GAMMA_1_8][i] = (int)(63.0f * powf(norm, 0.556f) + 0.5f);
        gamma_g6[COLOR_MODE_DITHERED][i] = i;
        gamma_g6[COLOR_MODE_DITHER2][i] = i;
        /* Warm - G reduced 20% */
        gamma_g6[COLOR_MODE_WARM][i] = (i * 80) / 100;
        /* Warm+ - G reduced 40% */
        gamma_g6[COLOR_MODE_WARM_PLUS][i] = (i * 60) / 100;
        /* Night - dimmed to 63% then warm G */
        gamma_g6[COLOR_MODE_NIGHT][i] = (i * 50) / 100;  /* 63% * 80% */
        /* Night+ - dimmed to 27% then warm G */
        gamma_g6[COLOR_MODE_NIGHT_PLUS][i] = (i * 19) / 100;  /* 27% * 70% */
        /* Night+Dither - same as Night+ */
        gamma_g6[COLOR_MODE_NIGHT_DITHER][i] = (i * 19) / 100;
        /* Night+Dither2 - same as Night+ */
        gamma_g6[COLOR_MODE_NIGHT_DITHER2][i] = (i * 19) / 100;
        /* Legacy - identity (LUT not used, but fill anyway) */
        gamma_g6[COLOR_MODE_LEGACY][i] = i;
    }
}

static void draw_char(int x, int y, char c, pixel_t col) {
    if (c < 32 || c > 127) c = '?';
    const unsigned char *g = font[c - 32];
    for (int cx = 0; cx < 5; cx++)
        for (int cy = 0; cy < 7; cy++)
            if (g[cx] & (1 << cy))
                if (x+cx < SCREEN_WIDTH && y+cy < SCREEN_HEIGHT)
                    framebuffer[(y+cy) * SCREEN_WIDTH + x+cx] = col;
}

/* Check if position (cx,cy) is a font pixel in character glyph g */
static int is_font_pixel(const unsigned char *g, int cx, int cy) {
    if (cx < 0 || cx >= 5 || cy < 0 || cy >= 7) return 0;
    return (g[cx] & (1 << cy)) ? 1 : 0;
}

/* Draw character with black outline for better visibility */
static void draw_char_outline(int x, int y, char c, pixel_t col) {
    if (c < 32 || c > 127) c = '?';
    const unsigned char *g = font[c - 32];
    const pixel_t outline_col = 0x0000;  /* black */

    /* First pass: draw black outline around font pixels */
    for (int cx = 0; cx < 5; cx++) {
        for (int cy = 0; cy < 7; cy++) {
            if (g[cx] & (1 << cy)) {
                /* This is a font pixel - draw outline in 8 directions */
                static const int dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
                static const int dy[] = {-1, -1, -1, 0, 0, 1, 1, 1};
                for (int d = 0; d < 8; d++) {
                    int ox = cx + dx[d];
                    int oy = cy + dy[d];
                    /* Only draw outline if not a font pixel */
                    if (!is_font_pixel(g, ox, oy)) {
                        int px = x + ox;
                        int py = y + oy;
                        /* Skip if outside screen bounds */
                        if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                            framebuffer[py * SCREEN_WIDTH + px] = outline_col;
                        }
                    }
                }
            }
        }
    }

    /* Second pass: draw actual font pixels (overwrites outline where needed) */
    for (int cx = 0; cx < 5; cx++)
        for (int cy = 0; cy < 7; cy++)
            if (g[cx] & (1 << cy))
                if (x+cx >= 0 && x+cx < SCREEN_WIDTH && y+cy >= 0 && y+cy < SCREEN_HEIGHT)
                    framebuffer[(y+cy) * SCREEN_WIDTH + x+cx] = col;
}

static void draw_str(int x, int y, const char *s, pixel_t col) {
    while (*s) { draw_char_outline(x, y, *s++, col); x += 6; }
}

static void draw_num(int x, int y, int num, pixel_t col) {
    char buf[12]; int i = 0, neg = 0;
    if (num < 0) { neg = 1; num = -num; }
    if (num == 0) buf[i++] = '0';
    else while (num > 0) { buf[i++] = '0' + (num % 10); num /= 10; }
    if (neg) buf[i++] = '-';
    while (i > 0) { draw_char_outline(x, y, buf[--i], col); x += 6; }
}

/* Get pixel width of a number (6 pixels per digit) */
static int num_width(int num) {
    if (num == 0) return 6;
    int digits = 0;
    if (num < 0) { digits++; num = -num; }
    while (num > 0) { digits++; num /= 10; }
    return digits * 6;
}

/* Darken a pixel (for semi-transparent overlay) */
static pixel_t darken_pixel(pixel_t p) {
    /* RGB565: reduce each component by 75% (keep 25%) */
    uint16_t r = (p >> 11) & 0x1F;
    uint16_t g = (p >> 5) & 0x3F;
    uint16_t b = p & 0x1F;
    r = r >> 2;  /* divide by 4 */
    g = g >> 2;
    b = b >> 2;
    return (r << 11) | (g << 5) | b;
}

/* Draw a darkened rectangle (semi-transparent effect) */
static void draw_dark_rect(int x1, int y1, int x2, int y2) {
    for (int y = y1; y <= y2 && y < SCREEN_HEIGHT; y++) {
        for (int x = x1; x <= x2 && x < SCREEN_WIDTH; x++) {
            if (x >= 0 && y >= 0) {
                int idx = y * SCREEN_WIDTH + x;
                framebuffer[idx] = darken_pixel(framebuffer[idx]);
            }
        }
    }
}

/* Draw filled rectangle */
static void draw_fill_rect(int x1, int y1, int x2, int y2, pixel_t col) {
    for (int y = y1; y <= y2 && y < SCREEN_HEIGHT; y++) {
        for (int x = x1; x <= x2 && x < SCREEN_WIDTH; x++) {
            if (x >= 0 && y >= 0) {
                framebuffer[y * SCREEN_WIDTH + x] = col;
            }
        }
    }
}

/* Draw rectangle outline (border only) */
static void draw_rect(int x1, int y1, int x2, int y2, pixel_t col) {
    /* Top and bottom edges */
    for (int x = x1; x <= x2 && x < SCREEN_WIDTH; x++) {
        if (x >= 0) {
            if (y1 >= 0 && y1 < SCREEN_HEIGHT) framebuffer[y1 * SCREEN_WIDTH + x] = col;
            if (y2 >= 0 && y2 < SCREEN_HEIGHT) framebuffer[y2 * SCREEN_WIDTH + x] = col;
        }
    }
    /* Left and right edges */
    for (int y = y1; y <= y2 && y < SCREEN_HEIGHT; y++) {
        if (y >= 0) {
            if (x1 >= 0 && x1 < SCREEN_WIDTH) framebuffer[y * SCREEN_WIDTH + x1] = col;
            if (x2 >= 0 && x2 < SCREEN_WIDTH) framebuffer[y * SCREEN_WIDTH + x2] = col;
        }
    }
}

/* Draw circle outline */
static void draw_circle(int cx, int cy, int r, pixel_t col) {
    for (int angle = 0; angle < 360; angle += 5) {
        /* Simple circle using 8-way symmetry */
        int x = 0, y = r, d = 3 - 2 * r;
        while (x <= y) {
            int px, py;
            px = cx + x; py = cy + y; if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) framebuffer[py * SCREEN_WIDTH + px] = col;
            px = cx - x; py = cy + y; if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) framebuffer[py * SCREEN_WIDTH + px] = col;
            px = cx + x; py = cy - y; if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) framebuffer[py * SCREEN_WIDTH + px] = col;
            px = cx - x; py = cy - y; if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) framebuffer[py * SCREEN_WIDTH + px] = col;
            px = cx + y; py = cy + x; if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) framebuffer[py * SCREEN_WIDTH + px] = col;
            px = cx - y; py = cy + x; if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) framebuffer[py * SCREEN_WIDTH + px] = col;
            px = cx + y; py = cy - x; if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) framebuffer[py * SCREEN_WIDTH + px] = col;
            px = cx - y; py = cy - x; if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) framebuffer[py * SCREEN_WIDTH + px] = col;
            if (d < 0) { d += 4 * x + 6; } else { d += 4 * (x - y) + 10; y--; }
            x++;
        }
        break; /* only need one pass */
    }
}

/* Draw filled circle */
static void draw_filled_circle(int cx, int cy, int r, pixel_t col) {
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x*x + y*y <= r*r) {
                int px = cx + x, py = cy + y;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                    framebuffer[py * SCREEN_WIDTH + px] = col;
                }
            }
        }
    }
}

/* Draw visual feedback icon */
static void draw_icon(int type) {
    int cx, cy;
    pixel_t bg_col = 0x4208;  /* dark gray */
    pixel_t fg_col = 0xFFFF;  /* white */

    if (type == ICON_SKIP_LEFT) {
        cx = 60; cy = 120;  /* left side */
        draw_filled_circle(cx, cy, 25, bg_col);
        draw_circle(cx, cy, 25, fg_col);
        /* Draw << double arrow pointing LEFT - centered in circle */
        for (int i = 0; i < 10; i++) {
            int px = cx + 5 - i, py1 = cy - (9-i), py2 = cy + (9-i);
            if (px >= 0 && px < SCREEN_WIDTH) {
                if (py1 >= 0 && py1 < SCREEN_HEIGHT) framebuffer[py1 * SCREEN_WIDTH + px] = fg_col;
                if (py2 >= 0 && py2 < SCREEN_HEIGHT) framebuffer[py2 * SCREEN_WIDTH + px] = fg_col;
            }
            px = cx - 5 - i;
            if (px >= 0 && px < SCREEN_WIDTH) {
                if (py1 >= 0 && py1 < SCREEN_HEIGHT) framebuffer[py1 * SCREEN_WIDTH + px] = fg_col;
                if (py2 >= 0 && py2 < SCREEN_HEIGHT) framebuffer[py2 * SCREEN_WIDTH + px] = fg_col;
            }
        }
        draw_str(cx - 9, cy + 30, "15s", fg_col);
    }
    else if (type == ICON_SKIP_RIGHT) {
        cx = 260; cy = 120;  /* right side */
        draw_filled_circle(cx, cy, 25, bg_col);
        draw_circle(cx, cy, 25, fg_col);
        /* Draw >> double arrow pointing RIGHT - centered in circle */
        for (int i = 0; i < 10; i++) {
            int px = cx - 5 + i, py1 = cy - (9-i), py2 = cy + (9-i);
            if (px >= 0 && px < SCREEN_WIDTH) {
                if (py1 >= 0 && py1 < SCREEN_HEIGHT) framebuffer[py1 * SCREEN_WIDTH + px] = fg_col;
                if (py2 >= 0 && py2 < SCREEN_HEIGHT) framebuffer[py2 * SCREEN_WIDTH + px] = fg_col;
            }
            px = cx + 5 + i;
            if (px >= 0 && px < SCREEN_WIDTH) {
                if (py1 >= 0 && py1 < SCREEN_HEIGHT) framebuffer[py1 * SCREEN_WIDTH + px] = fg_col;
                if (py2 >= 0 && py2 < SCREEN_HEIGHT) framebuffer[py2 * SCREEN_WIDTH + px] = fg_col;
            }
        }
        draw_str(cx - 9, cy + 30, "15s", fg_col);
    }
    else if (type == ICON_PAUSE) {
        cx = 160; cy = 120;  /* center */
        draw_filled_circle(cx, cy, 25, bg_col);
        draw_circle(cx, cy, 25, fg_col);
        /* Draw || pause bars */
        draw_fill_rect(cx - 8, cy - 10, cx - 4, cy + 10, fg_col);
        draw_fill_rect(cx + 4, cy - 10, cx + 8, cy + 10, fg_col);
    }
    else if (type == ICON_PLAY) {
        cx = 160; cy = 120;  /* center */
        draw_filled_circle(cx, cy, 25, bg_col);
        draw_circle(cx, cy, 25, fg_col);
        /* Draw > play triangle pointing RIGHT */
        for (int i = 0; i < 14; i++) {
            int px = cx - 5 + i;
            int h = (14 - i) * 10 / 14;  /* widest at left, point at right */
            for (int dy = -h; dy <= h; dy++) {
                int py = cy + dy;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                    framebuffer[py * SCREEN_WIDTH + px] = fg_col;
                }
            }
        }
    }
    else if (type == ICON_LOCK || type == ICON_UNLOCK) {
        cx = 160; cy = 120;  /* center */
        draw_filled_circle(cx, cy, 25, bg_col);
        draw_circle(cx, cy, 25, fg_col);

        /* Draw key shape */
        /* Key head (circle at top) */
        draw_circle(cx, cy - 8, 7, fg_col);
        draw_circle(cx, cy - 8, 6, fg_col);
        /* Key hole */
        draw_filled_circle(cx, cy - 8, 3, bg_col);

        /* Key shaft (vertical bar) */
        draw_fill_rect(cx - 2, cy - 1, cx + 2, cy + 14, fg_col);

        /* Key teeth (horizontal bars on shaft) */
        draw_fill_rect(cx + 2, cy + 4, cx + 6, cy + 6, fg_col);
        draw_fill_rect(cx + 2, cy + 9, cx + 8, cy + 11, fg_col);

        /* If unlocking, draw X over the key */
        if (type == ICON_UNLOCK) {
            pixel_t x_col = 0xF800;  /* red */
            /* Draw X from top-left to bottom-right */
            for (int i = -10; i <= 10; i++) {
                int px = cx + i;
                int py = cy + i;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                    framebuffer[py * SCREEN_WIDTH + px] = x_col;
                    if (py+1 < SCREEN_HEIGHT) framebuffer[(py+1) * SCREEN_WIDTH + px] = x_col;
                }
            }
            /* Draw X from top-right to bottom-left */
            for (int i = -10; i <= 10; i++) {
                int px = cx + i;
                int py = cy - i;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                    framebuffer[py * SCREEN_WIDTH + px] = x_col;
                    if (py+1 < SCREEN_HEIGHT) framebuffer[(py+1) * SCREEN_WIDTH + px] = x_col;
                }
            }
        }
    }
    else if (type == ICON_SKIP_BACK_1M) {
        cx = 60; cy = 120;  /* left side */
        draw_filled_circle(cx, cy, 25, bg_col);
        draw_circle(cx, cy, 25, fg_col);
        /* Draw << double arrow pointing LEFT */
        for (int i = 0; i < 10; i++) {
            int px = cx + 5 - i, py1 = cy - (9-i), py2 = cy + (9-i);
            if (px >= 0 && px < SCREEN_WIDTH) {
                if (py1 >= 0 && py1 < SCREEN_HEIGHT) framebuffer[py1 * SCREEN_WIDTH + px] = fg_col;
                if (py2 >= 0 && py2 < SCREEN_HEIGHT) framebuffer[py2 * SCREEN_WIDTH + px] = fg_col;
            }
            px = cx - 5 - i;
            if (px >= 0 && px < SCREEN_WIDTH) {
                if (py1 >= 0 && py1 < SCREEN_HEIGHT) framebuffer[py1 * SCREEN_WIDTH + px] = fg_col;
                if (py2 >= 0 && py2 < SCREEN_HEIGHT) framebuffer[py2 * SCREEN_WIDTH + px] = fg_col;
            }
        }
        draw_str(cx - 6, cy + 30, "1m", fg_col);
    }
    else if (type == ICON_SKIP_FWD_1M) {
        cx = 260; cy = 120;  /* right side */
        draw_filled_circle(cx, cy, 25, bg_col);
        draw_circle(cx, cy, 25, fg_col);
        /* Draw >> double arrow pointing RIGHT */
        for (int i = 0; i < 10; i++) {
            int px = cx - 5 + i, py1 = cy - (9-i), py2 = cy + (9-i);
            if (px >= 0 && px < SCREEN_WIDTH) {
                if (py1 >= 0 && py1 < SCREEN_HEIGHT) framebuffer[py1 * SCREEN_WIDTH + px] = fg_col;
                if (py2 >= 0 && py2 < SCREEN_HEIGHT) framebuffer[py2 * SCREEN_WIDTH + px] = fg_col;
            }
            px = cx + 5 + i;
            if (px >= 0 && px < SCREEN_WIDTH) {
                if (py1 >= 0 && py1 < SCREEN_HEIGHT) framebuffer[py1 * SCREEN_WIDTH + px] = fg_col;
                if (py2 >= 0 && py2 < SCREEN_HEIGHT) framebuffer[py2 * SCREEN_WIDTH + px] = fg_col;
            }
        }
        draw_str(cx - 6, cy + 30, "1m", fg_col);
    }
}

/* Forward declaration for seek preview */
static int decode_single_frame(int idx);
static int load_avi_file(const char *path);

/* ========== Settings save/load ========== */
static void save_settings(void) {
    /* Ensure VIDEOS directory exists first */
    int fd = fs_open(FB_START_PATH, FS_O_RDONLY, 0);
    if (fd >= 0) {
        fs_close(fd);
    } else {
        fs_mkdir(FB_START_PATH, 0777);
    }

    /* Write settings to temp file first (atomic write) */
    char tmp_path[FB_MAX_PATH];
    snprintf(tmp_path, FB_MAX_PATH, "%s.tmp", SETTINGS_FILE);

    fd = fs_open(tmp_path, FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC, 0666);
    if (fd < 0) return;  /* Can't write - SD might be read-only */

    /* Simple key=value format */
    char buf[512];
    int len = snprintf(buf, 512,
        "# A ZERO Player settings\n"
        "color_mode=%d\n"
        "show_time=%d\n"
        "show_debug=%d\n"
        "last_dir=%s\n",
        color_mode, show_time, show_debug, fb_current_path);

    fs_write(fd, buf, len);
    fs_close(fd);

    /* TODO: rename tmp to final (atomic) - for now just write directly */
    /* Since rename might not work reliably, write directly to final */
    fd = fs_open(SETTINGS_FILE, FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC, 0666);
    if (fd >= 0) {
        fs_write(fd, buf, len);
        fs_close(fd);
    }
}

static void load_settings(void) {
    int fd = fs_open(SETTINGS_FILE, FS_O_RDONLY, 0);
    if (fd < 0) return;  /* No settings file - use defaults */

    char buf[512];
    ssize_t bytes = fs_read(fd, buf, 511);
    fs_close(fd);

    if (bytes <= 0) return;
    buf[bytes] = '\0';

    /* Parse key=value lines */
    char *line = buf;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';

        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\0') {
            line = next;
            continue;
        }

        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            char *key = line;
            char *val = eq + 1;

            if (strcmp(key, "color_mode") == 0) {
                int v = 0;
                while (*val >= '0' && *val <= '9') v = v * 10 + (*val++ - '0');
                if (v >= 0 && v < COLOR_MODE_COUNT) color_mode = v;
            }
            else if (strcmp(key, "show_time") == 0) {
                show_time = (val[0] == '1') ? 1 : 0;
            }
            else if (strcmp(key, "show_debug") == 0) {
                show_debug = (val[0] == '1') ? 1 : 0;
            }
            else if (strcmp(key, "last_dir") == 0) {
                strncpy(fb_current_path, val, FB_MAX_PATH - 1);
                fb_current_path[FB_MAX_PATH - 1] = '\0';
            }
        }
        line = next;
    }
}

/* File browser functions */
static void fb_ensure_videos_dir(void) {
    /* Try to create VIDEOS directory if it doesn't exist */
    int fd = fs_open(FB_START_PATH, FS_O_RDONLY, 0);
    if (fd >= 0) {
        fs_close(fd);
    } else {
        fs_mkdir(FB_START_PATH, 0777);
    }
}

static int str_ends_with(const char *str, const char *suffix) {
    int str_len = strlen(str);
    int suffix_len = strlen(suffix);
    if (suffix_len > str_len) return 0;
    return strcasecmp(str + str_len - suffix_len, suffix) == 0;
}

/* Convert Polish UTF-8 characters to Latin equivalents for display */
static void polish_to_latin(const char *src, char *dst, int max_len) {
    int i = 0, j = 0;
    while (src[i] && j < max_len - 1) {
        unsigned char c = (unsigned char)src[i];
        /* UTF-8 Polish characters (2-byte sequences starting with 0xC4 or 0xC5) */
        if (c == 0xC4 && src[i+1]) {
            unsigned char c2 = (unsigned char)src[i+1];
            switch (c2) {
                case 0x84: dst[j++] = 'A'; break;  /* Ą */
                case 0x85: dst[j++] = 'a'; break;  /* ą */
                case 0x86: dst[j++] = 'C'; break;  /* Ć */
                case 0x87: dst[j++] = 'c'; break;  /* ć */
                case 0x98: dst[j++] = 'E'; break;  /* Ę */
                case 0x99: dst[j++] = 'e'; break;  /* ę */
                default: dst[j++] = '?'; break;
            }
            i += 2;
        } else if (c == 0xC5 && src[i+1]) {
            unsigned char c2 = (unsigned char)src[i+1];
            switch (c2) {
                case 0x81: dst[j++] = 'L'; break;  /* Ł */
                case 0x82: dst[j++] = 'l'; break;  /* ł */
                case 0x83: dst[j++] = 'N'; break;  /* Ń */
                case 0x84: dst[j++] = 'n'; break;  /* ń */
                case 0x9A: dst[j++] = 'S'; break;  /* Ś */
                case 0x9B: dst[j++] = 's'; break;  /* ś */
                case 0xB9: dst[j++] = 'Z'; break;  /* Ź */
                case 0xBA: dst[j++] = 'z'; break;  /* ź */
                case 0xBB: dst[j++] = 'Z'; break;  /* Ż */
                case 0xBC: dst[j++] = 'z'; break;  /* ż */
                case 0xB3: dst[j++] = 'O'; break;  /* Ó */
                case 0xB4: dst[j++] = 'o'; break;  /* ó - note: Ó is actually 0xC3 0x93 */
                default: dst[j++] = '?'; break;
            }
            i += 2;
        } else if (c == 0xC3 && src[i+1]) {
            /* Ó/ó are in Latin-1 supplement range */
            unsigned char c2 = (unsigned char)src[i+1];
            switch (c2) {
                case 0x93: dst[j++] = 'O'; break;  /* Ó */
                case 0xB3: dst[j++] = 'o'; break;  /* ó */
                default: dst[j++] = '?'; break;
            }
            i += 2;
        } else if (c >= 0x80) {
            /* Other multi-byte UTF-8 - skip */
            if ((c & 0xE0) == 0xC0) i += 2;
            else if ((c & 0xF0) == 0xE0) i += 3;
            else if ((c & 0xF8) == 0xF0) i += 4;
            else i++;
            dst[j++] = '?';
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

static void fb_scan_directory(void) {
    /* fs_readdir buffer structure */
    union {
        struct {
            uint8_t _1[0x10];
            uint32_t type;
        };
        struct {
            uint8_t _2[0x22];
            char d_name[0x225];
        };
        uint8_t __[0x428];
    } buffer;

    fb_file_count = 0;
    fb_selection = 0;
    fb_scroll = 0;

    int dir_fd = fs_opendir(fb_current_path);
    if (dir_fd < 0) {
        /* Directory doesn't exist, try to go to root */
        strcpy(fb_current_path, "/mnt/sda1");
        dir_fd = fs_opendir(fb_current_path);
        if (dir_fd < 0) return;
    }

    /* First add ".." entry if not at root */
    if (strcmp(fb_current_path, "/mnt/sda1") != 0) {
        strcpy(fb_files[fb_file_count], "..");
        fb_is_dir[fb_file_count] = 1;
        fb_file_count++;
    }

    /* Read directory entries */
    while (fb_file_count < FB_MAX_FILES) {
        memset(&buffer, 0, sizeof(buffer));
        if (fs_readdir(dir_fd, &buffer) < 0) break;

        /* Skip . and .. */
        if (buffer.d_name[0] == '.' &&
            (buffer.d_name[1] == '\0' ||
             (buffer.d_name[1] == '.' && buffer.d_name[2] == '\0'))) {
            continue;
        }

        int is_dir = S_ISDIR(buffer.type);
        int is_avi = str_ends_with(buffer.d_name, ".avi");

        /* Only show directories and .avi files */
        if (!is_dir && !is_avi) continue;

        /* Copy filename (truncate if needed) */
        strncpy(fb_files[fb_file_count], buffer.d_name, FB_MAX_NAME - 1);
        fb_files[fb_file_count][FB_MAX_NAME - 1] = '\0';
        fb_is_dir[fb_file_count] = is_dir;
        fb_file_count++;
    }

    fs_closedir(dir_fd);
}

static void fb_enter_selected(void) {
    if (fb_file_count == 0) return;

    if (fb_is_dir[fb_selection]) {
        /* Enter directory */
        if (strcmp(fb_files[fb_selection], "..") == 0) {
            /* Go up - find last / */
            char *last_slash = strrchr(fb_current_path, '/');
            if (last_slash && last_slash != fb_current_path) {
                *last_slash = '\0';
            }
        } else {
            /* Enter subdirectory */
            int len = strlen(fb_current_path);
            if (len + 1 + strlen(fb_files[fb_selection]) < FB_MAX_PATH) {
                strcat(fb_current_path, "/");
                strcat(fb_current_path, fb_files[fb_selection]);
            }
        }
        fb_scan_directory();
    } else {
        /* Load file */
        char full_path[FB_MAX_PATH];
        snprintf(full_path, FB_MAX_PATH, "%s/%s", fb_current_path, fb_files[fb_selection]);

        if (load_avi_file(full_path) == 0) {
            /* Success - close file browser and menu */
            strcpy(loaded_file_path, full_path);
            file_browser_active = 0;
            menu_active = 0;
            no_file_loaded = 0;
            is_paused = 0;
        }
    }
}

static void draw_file_browser(void) {
    int fb_x = 30;
    int fb_y = 15;
    int fb_w = 260;
    int fb_h = 210;

    pixel_t col_bg = 0x0000;
    pixel_t col_border = 0xFFFF;
    pixel_t col_title = 0xFFE0;
    pixel_t col_file = 0xFFFF;
    pixel_t col_dir = 0x07FF;
    pixel_t col_sel = 0x001F;

    /* Background */
    draw_fill_rect(fb_x, fb_y, fb_x + fb_w, fb_y + fb_h, col_bg);
    /* Border */
    draw_rect(fb_x, fb_y, fb_x + fb_w, fb_y + fb_h, col_border);
    draw_rect(fb_x + 1, fb_y + 1, fb_x + fb_w - 1, fb_y + fb_h - 1, col_border);

    /* Title */
    draw_str(fb_x + 8, fb_y + 5, "Load Video File", col_title);

    /* Current path (truncated if too long, convert Polish chars) */
    char path_display[40];
    char path_latin[40];
    int path_len = strlen(fb_current_path);
    if (path_len > 38) {
        strcpy(path_display, "...");
        strcat(path_display, fb_current_path + path_len - 35);
    } else {
        strcpy(path_display, fb_current_path);
    }
    polish_to_latin(path_display, path_latin, 40);
    draw_str(fb_x + 8, fb_y + 17, path_latin, 0x7BEF);

    /* Separator */
    draw_fill_rect(fb_x + 4, fb_y + 28, fb_x + fb_w - 4, fb_y + 29, col_border);

    /* File list */
    int list_y = fb_y + 34;
    int item_height = 10;

    for (int i = 0; i < FB_VISIBLE_ITEMS && (fb_scroll + i) < fb_file_count; i++) {
        int idx = fb_scroll + i;
        int y = list_y + i * item_height;

        /* Selection highlight */
        if (idx == fb_selection) {
            draw_fill_rect(fb_x + 4, y - 1, fb_x + fb_w - 4, y + 8, col_sel);
        }

        /* Icon and filename (convert Polish chars for display) */
        pixel_t col = fb_is_dir[idx] ? col_dir : col_file;
        char display_name[45];
        char display_latin[45];

        if (fb_is_dir[idx]) {
            snprintf(display_name, 44, "[%s]", fb_files[idx]);
        } else {
            strncpy(display_name, fb_files[idx], 44);
            display_name[44] = '\0';
        }
        polish_to_latin(display_name, display_latin, 45);
        draw_str(fb_x + 8, y, display_latin, col);
    }

    /* Scroll indicators */
    if (fb_scroll > 0) {
        draw_str(fb_x + fb_w - 20, list_y, "^", col_border);
    }
    if (fb_scroll + FB_VISIBLE_ITEMS < fb_file_count) {
        draw_str(fb_x + fb_w - 20, list_y + (FB_VISIBLE_ITEMS - 1) * item_height, "v", col_border);
    }

    /* Instructions */
    draw_str(fb_x + 8, fb_y + fb_h - 20, "A:Select B:Back", 0x7BEF);

    /* File count */
    char count_str[20];
    snprintf(count_str, 20, "%d files", fb_file_count);
    draw_str(fb_x + fb_w - 60, fb_y + fb_h - 20, count_str, 0x7BEF);
}

/* Draw menu overlay - Amiga style */
static void draw_menu(void) {
    int menu_x = 50;
    int menu_y = 5;    /* expanded up to fit 10 menu items */
    int menu_w = 220;
    int menu_h = 232;  /* +10px for Save Settings option */

    /* Colors - Amiga Workbench inspired blueish palette */
    pixel_t col_bg = 0x0010;      /* dark blue background */
    pixel_t col_border = 0x001F;  /* bright blue border */
    pixel_t col_title = 0xFFFF;   /* white title text */
    pixel_t col_titlebar = 0x52AA; /* medium blue title bar */
    pixel_t col_text = 0xFFFF;    /* white text */
    pixel_t col_sel = 0x07E0;     /* green selected */
    pixel_t col_value = 0x07FF;   /* cyan values */
    pixel_t col_hint = 0xFBE0;    /* orange hint */
    pixel_t col_corner = 0x6B5D;  /* light blue corners */

    /* Darken background */
    draw_dark_rect(menu_x - 8, menu_y - 8, menu_x + menu_w + 8, menu_y + menu_h + 8);

    /* Main window fill */
    draw_fill_rect(menu_x, menu_y, menu_x + menu_w, menu_y + menu_h, col_bg);

    /* Amiga-style border with corner cuts */
    /* Top border */
    draw_fill_rect(menu_x + 6, menu_y - 2, menu_x + menu_w - 6, menu_y, col_border);
    /* Bottom border */
    draw_fill_rect(menu_x + 6, menu_y + menu_h, menu_x + menu_w - 6, menu_y + menu_h + 2, col_border);
    /* Left border */
    draw_fill_rect(menu_x - 2, menu_y + 6, menu_x, menu_y + menu_h - 6, col_border);
    /* Right border */
    draw_fill_rect(menu_x + menu_w, menu_y + 6, menu_x + menu_w + 2, menu_y + menu_h - 6, col_border);

    /* Corner angles (Amiga style) */
    /* Top-left corner */
    draw_fill_rect(menu_x, menu_y, menu_x + 6, menu_y + 2, col_corner);
    draw_fill_rect(menu_x, menu_y, menu_x + 2, menu_y + 6, col_corner);
    /* Top-right corner */
    draw_fill_rect(menu_x + menu_w - 6, menu_y, menu_x + menu_w, menu_y + 2, col_corner);
    draw_fill_rect(menu_x + menu_w - 2, menu_y, menu_x + menu_w, menu_y + 6, col_corner);
    /* Bottom-left corner */
    draw_fill_rect(menu_x, menu_y + menu_h - 2, menu_x + 6, menu_y + menu_h, col_corner);
    draw_fill_rect(menu_x, menu_y + menu_h - 6, menu_x + 2, menu_y + menu_h, col_corner);
    /* Bottom-right corner */
    draw_fill_rect(menu_x + menu_w - 6, menu_y + menu_h - 2, menu_x + menu_w, menu_y + menu_h, col_corner);
    draw_fill_rect(menu_x + menu_w - 2, menu_y + menu_h - 6, menu_x + menu_w, menu_y + menu_h, col_corner);

    /* Title bar */
    draw_fill_rect(menu_x + 4, menu_y + 4, menu_x + menu_w - 4, menu_y + 26, col_titlebar);

    /* Title text - shifted right by 2 chars to align with author */
    draw_str(menu_x + 52, menu_y + 7, "A ZERO Player v0.75", col_title);
    draw_str(menu_x + 50, menu_y + 17, "by Grzegorz Korycki", col_value);

    /* Item 0: Load File */
    int load_y = menu_y + 34;
    pixel_t load_col = (menu_selection == 0) ? col_sel : col_text;
    if (menu_selection == 0) {
        draw_fill_rect(menu_x + 6, load_y - 1, menu_x + menu_w - 6, load_y + 9, 0x0015);
        draw_str(menu_x + 8, load_y, ">", col_sel);
    }
    draw_str(menu_x + 20, load_y, menu_labels[0], load_col);
    draw_str(menu_x + 130, load_y, "[...]", col_value);

    /* Item 1: Go to Position (with slider) */
    int go_y = menu_y + 48;
    pixel_t go_col = (menu_selection == 1) ? col_sel : col_text;
    if (menu_selection == 1) {
        draw_fill_rect(menu_x + 6, go_y - 1, menu_x + menu_w - 6, go_y + 9, 0x0015);
        draw_str(menu_x + 8, go_y, ">", col_sel);
    }
    draw_str(menu_x + 20, go_y, menu_labels[1], go_col);

    /* Seek slider */
    int slider_y = go_y + 14;
    int slider_x = menu_x + 15;
    int slider_w = menu_w - 30;

    /* Slider background (groove) */
    draw_fill_rect(slider_x, slider_y, slider_x + slider_w, slider_y + 8, 0x0008);
    draw_fill_rect(slider_x + 1, slider_y + 1, slider_x + slider_w - 1, slider_y + 7, 0x2104);

    /* Slider position markers */
    for (int p = 0; p <= 20; p += 5) {
        int mark_x = slider_x + (p * slider_w / 20);
        draw_fill_rect(mark_x, slider_y - 2, mark_x + 1, slider_y + 10, col_border);
    }

    /* Current position handle */
    int pos_x = slider_x + (seek_position * slider_w / 20);
    draw_fill_rect(pos_x - 4, slider_y - 3, pos_x + 4, slider_y + 11, col_sel);
    draw_fill_rect(pos_x - 2, slider_y - 1, pos_x + 2, slider_y + 9, col_title);

    /* Position info */
    int pct = seek_position * 5;
    int target_frame = (total_frames > 0) ? (seek_position * total_frames / 20) : 0;
    draw_num(slider_x, slider_y + 14, pct, col_hint);
    draw_str(slider_x + 18, slider_y + 14, "%", col_hint);
    draw_str(slider_x + 50, slider_y + 14, "Fr:", col_text);
    draw_num(slider_x + 70, slider_y + 14, target_frame, col_value);
    draw_str(slider_x + 110, slider_y + 14, "/", col_text);
    draw_num(slider_x + 118, slider_y + 14, total_frames, col_value);

    /* Hint when slider selected */
    if (menu_selection == 1) {
        draw_str(menu_x + 52, slider_y + 24, "L/R: Seek", col_hint);
    }

    /* Separator line after Go to Position */
    draw_fill_rect(menu_x + 10, menu_y + 97, menu_x + menu_w - 10, menu_y + 98, col_border);

    /* Menu items 2-8 */
    for (int i = 2; i < MENU_ITEMS; i++) {
        int item_y = menu_y + 103 + (i - 2) * 14;
        pixel_t col = (i == menu_selection) ? col_sel : col_text;

        if (i == menu_selection) {
            draw_fill_rect(menu_x + 6, item_y - 1, menu_x + menu_w - 6, item_y + 9, 0x0015);
            draw_str(menu_x + 8, item_y, ">", col_sel);
        }
        draw_str(menu_x + 20, item_y, menu_labels[i], col);

        /* Show current state for items */
        if (i == 2) {  /* Color Mode */
            draw_str(menu_x + 120, item_y, color_mode_names[color_mode], col_value);
        } else if (i == 4) {  /* Show Time */
            draw_str(menu_x + 150, item_y, show_time ? "[ON]" : "[OFF]", col_value);
        } else if (i == 5) {  /* Debug Info */
            draw_str(menu_x + 150, item_y, show_debug ? "[ON]" : "[OFF]", col_value);
        } else if (i == 7) {  /* Save Settings - disk icon */
            draw_str(menu_x + 150, item_y, "[!]", col_value);
        } else if (i == 8) {  /* Instructions - show arrow */
            draw_str(menu_x + 150, item_y, "[>]", col_value);
        } else if (i == 9) {  /* About - yellow slash on white */
            draw_str(menu_x + 155, item_y, "/", 0xFFE0);  /* yellow slash */
        }
    }

    /* Instructions at bottom */
    draw_str(menu_x + 30, menu_y + menu_h - 12, "UP/DOWN:Sel  START:Close", 0x6B5D);

    /* Draw submenu overlay if active */
    if (submenu_active > 0) {
        int sub_x = menu_x + 20;
        int sub_y = menu_y + 40;
        int sub_w = menu_w - 40;
        int sub_h = (submenu_active == 1) ? 124 : 116;

        /* Submenu background */
        draw_fill_rect(sub_x, sub_y, sub_x + sub_w, sub_y + sub_h, 0x0008);
        draw_fill_rect(sub_x + 2, sub_y + 2, sub_x + sub_w - 2, sub_y + sub_h - 2, col_bg);

        /* Border */
        draw_fill_rect(sub_x, sub_y, sub_x + sub_w, sub_y + 2, col_border);
        draw_fill_rect(sub_x, sub_y + sub_h - 2, sub_x + sub_w, sub_y + sub_h, col_border);
        draw_fill_rect(sub_x, sub_y, sub_x + 2, sub_y + sub_h, col_border);
        draw_fill_rect(sub_x + sub_w - 2, sub_y, sub_x + sub_w, sub_y + sub_h, col_border);

        if (submenu_active == 1) {
            /* Instructions submenu */
            draw_str(sub_x + 40, sub_y + 8, "INSTRUCTIONS", col_title);
            draw_str(sub_x + 10, sub_y + 26, "A: Play/Pause", col_text);
            draw_str(sub_x + 10, sub_y + 38, "L/R: Skip 15 sec", col_text);
            draw_str(sub_x + 10, sub_y + 50, "Up/Down: Skip 1 min", col_text);
            draw_str(sub_x + 10, sub_y + 62, "START: Menu", col_text);
            draw_str(sub_x + 10, sub_y + 74, "L+R Shoulder 2s:", col_text);
            draw_str(sub_x + 20, sub_y + 86, "Lock all keys", col_text);
            draw_str(sub_x + 40, sub_y + 106, "A: Back", col_hint);
        } else if (submenu_active == 2) {
            /* About submenu */
            draw_str(sub_x + 60, sub_y + 8, "ABOUT", col_title);
            draw_str(sub_x + 47, sub_y + 26, "Contact:", col_text);
            draw_str(sub_x + 10, sub_y + 38, "@the_q_dev on Telegram", col_value);
            draw_str(sub_x + 37, sub_y + 56, "Greetings to:", col_text);
            draw_str(sub_x + 10, sub_y + 68, "Maciek, Madzia, Tomek,", col_value);
            draw_str(sub_x + 32, sub_y + 80, "Eliasz, Eliza", col_value);
            draw_str(sub_x + 40, sub_y + 100, "A: Back", col_hint);
        }
    }

    /* Color mode submenu (scrollable) */
    if (color_submenu_active) {
        int csub_x = menu_x + 15;
        int csub_y = menu_y + 35;
        int csub_w = menu_w - 30;
        int csub_h = 130;
        int visible_items = 8;  /* number of modes visible at once */

        /* Submenu background */
        draw_fill_rect(csub_x, csub_y, csub_x + csub_w, csub_y + csub_h, 0x0008);
        draw_fill_rect(csub_x + 2, csub_y + 2, csub_x + csub_w - 2, csub_y + csub_h - 2, col_bg);

        /* Border */
        draw_fill_rect(csub_x, csub_y, csub_x + csub_w, csub_y + 2, col_border);
        draw_fill_rect(csub_x, csub_y + csub_h - 2, csub_x + csub_w, csub_y + csub_h, col_border);
        draw_fill_rect(csub_x, csub_y, csub_x + 2, csub_y + csub_h, col_border);
        draw_fill_rect(csub_x + csub_w - 2, csub_y, csub_x + csub_w, csub_y + csub_h, col_border);

        /* Title */
        draw_str(csub_x + 35, csub_y + 6, "COLOR MODE", col_title);

        /* Scroll indicators */
        if (color_submenu_scroll > 0) {
            draw_str(csub_x + csub_w - 18, csub_y + 6, "^", col_hint);
        }
        if (color_submenu_scroll + visible_items < COLOR_MODE_COUNT) {
            draw_str(csub_x + csub_w - 18, csub_y + csub_h - 16, "v", col_hint);
        }

        /* Draw visible color modes */
        for (int i = 0; i < visible_items && (color_submenu_scroll + i) < COLOR_MODE_COUNT; i++) {
            int mode_idx = color_submenu_scroll + i;
            int item_y = csub_y + 20 + i * 12;
            pixel_t item_col = (mode_idx == color_mode) ? col_sel : col_text;

            if (mode_idx == color_mode) {
                draw_fill_rect(csub_x + 6, item_y - 1, csub_x + csub_w - 6, item_y + 9, 0x0015);
                draw_str(csub_x + 8, item_y, ">", col_sel);
            }
            draw_str(csub_x + 20, item_y, color_mode_names[mode_idx], item_col);
        }

        /* Hint at bottom */
        draw_str(csub_x + 15, csub_y + csub_h - 12, "A:Select B:Back", col_hint);
    }
}

/* AVI parsing helpers */
static uint32_t read_u32_le(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t read_u16_le(const uint8_t *p) {
    return p[0] | (p[1] << 8);
}
static int read32(FILE *f, uint32_t *out) {
    uint8_t buf[4];
    if (fread(buf, 1, 4, f) != 4) return -1;
    *out = read_u32_le(buf);
    return 0;
}
static int check4(FILE *f, const char *tag) {
    uint8_t buf[4];
    if (fread(buf, 1, 4, f) != 4) return 0;
    return (buf[0]==tag[0] && buf[1]==tag[1] && buf[2]==tag[2] && buf[3]==tag[3]);
}

/* Check if data at offset starts with JPEG magic */
static int check_jpeg_magic(long offset) {
    uint8_t magic[2];
    long saved = ftell(video_file);
    fseek(video_file, offset, SEEK_SET);
    int ok = (fread(magic, 1, 2, video_file) == 2 && magic[0] == 0xFF && magic[1] == 0xD8);
    fseek(video_file, saved, SEEK_SET);
    return ok;
}

/* Parse idx1 index chunk - returns 1 if successful, 0 if not found */
static int parse_idx1(long movi_data_start) {
    uint8_t tag[4];
    uint32_t chunk_size;

    /* Look for idx1 chunk after current position */
    while (fread(tag, 1, 4, video_file) == 4) {
        if (read32(video_file, &chunk_size) != 0) break;

        if (tag[0]=='i' && tag[1]=='d' && tag[2]=='x' && tag[3]=='1') {
            /* Found idx1! */
            int num_entries = chunk_size / 16;
            int entries_per_block = MAX_JPEG_SIZE / 16;

            long idx_start = ftell(video_file);

            /* Find first video entry to detect offset format */
            uint8_t entry[16];
            uint32_t first_video_offset = 0;
            uint32_t first_video_size = 0;
            int found_video = 0;

            for (int i = 0; i < num_entries && i < 100; i++) {
                if (fread(entry, 1, 16, video_file) != 16) break;
                if ((entry[2]=='d' || entry[2]=='D') && (entry[3]=='c' || entry[3]=='C')) {
                    first_video_offset = read_u32_le(entry + 8);
                    first_video_size = read_u32_le(entry + 12);
                    found_video = 1;
                    break;
                }
            }

            if (!found_video) {
                fseek(video_file, idx_start, SEEK_SET);
                return 0;  /* No video in idx1 */
            }

            /* Auto-detect offset format by checking JPEG magic */
            /* Try different offset calculations */
            long offset_base = 0;
            int add_header = 8;  /* +8 if offset points to chunk header */

            /* Variant 1: relative to movi + header offset */
            if (check_jpeg_magic(movi_data_start + first_video_offset + 8)) {
                offset_base = movi_data_start;
                add_header = 8;
            }
            /* Variant 2: relative to movi, no header offset */
            else if (check_jpeg_magic(movi_data_start + first_video_offset)) {
                offset_base = movi_data_start;
                add_header = 0;
            }
            /* Variant 3: absolute + header offset */
            else if (check_jpeg_magic(first_video_offset + 8)) {
                offset_base = 0;
                add_header = 8;
            }
            /* Variant 4: absolute, no header offset */
            else if (check_jpeg_magic(first_video_offset)) {
                offset_base = 0;
                add_header = 0;
            }
            /* Variant 5: relative to movi-4 (some encoders) + header */
            else if (check_jpeg_magic(movi_data_start - 4 + first_video_offset + 8)) {
                offset_base = movi_data_start - 4;
                add_header = 8;
            }
            else {
                /* None worked - fall back to scan */
                fseek(video_file, idx_start, SEEK_SET);
                return 0;
            }

            /* Go back and parse all entries with detected format */
            fseek(video_file, idx_start, SEEK_SET);
            int entries_done = 0;

            while (entries_done < num_entries) {
                int to_read = num_entries - entries_done;
                if (to_read > entries_per_block) to_read = entries_per_block;

                size_t got = fread(jpeg_buffer, 16, to_read, video_file);
                if (got == 0) break;

                for (size_t i = 0; i < got; i++) {
                    uint8_t *e = jpeg_buffer + i * 16;
                    uint32_t offset = read_u32_le(e + 8);
                    uint32_t size = read_u32_le(e + 12);
                    uint32_t abs_data_offset = offset_base + offset + add_header;

                    if ((e[2]=='d' || e[2]=='D') && (e[3]=='c' || e[3]=='C')) {
                        if (total_frames < MAX_FRAMES) {
                            frame_offsets[total_frames] = abs_data_offset;
                            frame_sizes[total_frames] = size;
                            total_frames++;
                        }
                    }
                    else if ((e[2]=='w' || e[2]=='W') && (e[3]=='b' || e[3]=='B')) {
                        if (total_audio_chunks < MAX_AUDIO_CHUNKS) {
                            audio_offsets[total_audio_chunks] = abs_data_offset;
                            audio_sizes[total_audio_chunks] = size;
                            total_audio_bytes += size;
                            total_audio_chunks++;
                        }
                    }
                }
                entries_done += got;
            }
            return 1;
        }

        fseek(video_file, chunk_size + (chunk_size & 1), SEEK_CUR);
    }

    return 0;
}

/* Fallback: scan movi chunk with buffered I/O (faster than byte-by-byte) */
static void scan_movi_buffered(long movi_start, long movi_end) {
    fseek(video_file, movi_start, SEEK_SET);

    while (ftell(video_file) < movi_end && total_frames < MAX_FRAMES) {
        uint8_t header[8];
        if (fread(header, 1, 8, video_file) != 8) break;

        uint32_t fsize = read_u32_le(header + 4);
        long data_pos = ftell(video_file);

        if ((header[2]=='d' || header[2]=='D') && (header[3]=='c' || header[3]=='C')) {
            frame_offsets[total_frames] = data_pos;
            frame_sizes[total_frames] = fsize;
            total_frames++;
        }
        else if ((header[2]=='w' || header[2]=='W') && (header[3]=='b' || header[3]=='B')) {
            if (total_audio_chunks < MAX_AUDIO_CHUNKS) {
                audio_offsets[total_audio_chunks] = data_pos;
                audio_sizes[total_audio_chunks] = fsize;
                total_audio_bytes += fsize;
                total_audio_chunks++;
            }
        }

        fseek(video_file, fsize + (fsize & 1), SEEK_CUR);
    }
}

static int parse_avi(void) {
    uint32_t riff_size, chunk_size, hsize;
    char tag[4], list_type[4], htag[4];
    long hdrl_end, strl_end;
    long movi_start = 0, movi_end = 0;
    uint8_t buf[64];
    int in_audio_strl = 0;
    int found_idx1 = 0;

    if (!check4(video_file, "RIFF")) return 0;
    if (read32(video_file, &riff_size) != 0) return 0;
    if (!check4(video_file, "AVI ")) return 0;

    total_frames = 0;
    total_audio_chunks = 0;
    total_audio_bytes = 0;
    clip_fps = 30;
    us_per_frame = 33333;
    repeat_count = 1;
    has_audio = 0;

    while (fread(tag, 1, 4, video_file) == 4) {
        if (read32(video_file, &chunk_size) != 0) break;

        if (tag[0]=='L' && tag[1]=='I' && tag[2]=='S' && tag[3]=='T') {
            if (fread(list_type, 1, 4, video_file) != 4) break;

            if (list_type[0]=='h' && list_type[1]=='d' && list_type[2]=='r' && list_type[3]=='l') {
                /* Parse header list for fps and audio info */
                hdrl_end = ftell(video_file) + chunk_size - 4;
                while (ftell(video_file) < hdrl_end) {
                    if (fread(htag, 1, 4, video_file) != 4) break;
                    if (read32(video_file, &hsize) != 0) break;

                    if (htag[0]=='a' && htag[1]=='v' && htag[2]=='i' && htag[3]=='h') {
                        if (hsize >= 4 && fread(buf, 1, (hsize < 56 ? hsize : 56), video_file) >= 4) {
                            us_per_frame = read_u32_le(buf);
                            if (us_per_frame > 0) {
                                clip_fps = 1000000 / us_per_frame;
                                if (clip_fps == 0) clip_fps = 1;
                            }
                            if (clip_fps >= 25) repeat_count = 1;
                            else if (clip_fps >= 12) repeat_count = 2;
                            else repeat_count = 3;
                            if (hsize > 56) fseek(video_file, hsize - 56, SEEK_CUR);
                        } else fseek(video_file, hsize, SEEK_CUR);
                    }
                    else if (htag[0]=='L' && htag[1]=='I' && htag[2]=='S' && htag[3]=='T') {
                        if (fread(buf, 1, 4, video_file) != 4) break;
                        if (buf[0]=='s' && buf[1]=='t' && buf[2]=='r' && buf[3]=='l') {
                            strl_end = ftell(video_file) + hsize - 4;
                            in_audio_strl = 0;
                            while (ftell(video_file) < strl_end) {
                                if (fread(htag, 1, 4, video_file) != 4) break;
                                uint32_t shsize;
                                if (read32(video_file, &shsize) != 0) break;

                                if (htag[0]=='s' && htag[1]=='t' && htag[2]=='r' && htag[3]=='h') {
                                    if (shsize >= 8 && fread(buf, 1, (shsize < 64 ? shsize : 64), video_file) >= 8) {
                                        if (buf[0]=='a' && buf[1]=='u' && buf[2]=='d' && buf[3]=='s') {
                                            in_audio_strl = 1;
                                        }
                                        if (shsize > 64) fseek(video_file, shsize - 64, SEEK_CUR);
                                    } else fseek(video_file, shsize, SEEK_CUR);
                                }
                                else if (htag[0]=='s' && htag[1]=='t' && htag[2]=='r' && htag[3]=='f') {
                                    if (in_audio_strl && shsize >= 16) {
                                        if (fread(buf, 1, 16, video_file) == 16) {
                                            uint16_t fmt = read_u16_le(buf);
                                            audio_channels = read_u16_le(buf + 2);
                                            audio_sample_rate = read_u32_le(buf + 4);
                                            audio_bits = read_u16_le(buf + 14);
                                            if (fmt == 1 && audio_channels > 0 && audio_sample_rate > 0) {
                                                has_audio = 1;
                                                audio_bytes_per_sample = (audio_bits / 8) * audio_channels;
                                            }
                                            if (shsize > 16) fseek(video_file, shsize - 16, SEEK_CUR);
                                        }
                                    } else fseek(video_file, shsize, SEEK_CUR);
                                }
                                else fseek(video_file, shsize + (shsize & 1), SEEK_CUR);
                            }
                        } else fseek(video_file, hsize - 4, SEEK_CUR);
                    }
                    else fseek(video_file, hsize + (hsize & 1), SEEK_CUR);
                }
            }
            else if (list_type[0]=='m' && list_type[1]=='o' && list_type[2]=='v' && list_type[3]=='i') {
                /* Found movi - save position but DON'T scan it yet */
                movi_start = ftell(video_file);
                movi_end = movi_start + chunk_size - 4;

                /* Skip to end of movi to look for idx1 */
                fseek(video_file, movi_end, SEEK_SET);

                /* Try to parse idx1 (instant loading!) */
                found_idx1 = parse_idx1(movi_start);

                if (!found_idx1) {
                    /* No idx1 - fall back to scanning movi */
                    scan_movi_buffered(movi_start, movi_end);
                }
                break;
            }
            else fseek(video_file, chunk_size - 4, SEEK_CUR);
        }
        else fseek(video_file, chunk_size + (chunk_size & 1), SEEK_CUR);
    }
    return (total_frames > 0);
}

/* JPEG decode into framebuffer */
static size_t tjpgd_input(JDEC *jd, uint8_t *buff, size_t nbyte) {
    jpeg_io_t *io = (jpeg_io_t *)jd->device;
    size_t remain = io->size - io->pos;
    if (nbyte > remain) nbyte = remain;
    if (buff) memcpy(buff, io->data + io->pos, nbyte);
    io->pos += nbyte;
    return nbyte;
}

static int tjpgd_output(JDEC *jd, void *bitmap, JRECT *rect) {
    (void)jd;
    uint16_t *src = (uint16_t *)bitmap;
    int w = rect->right - rect->left + 1, h = rect->bottom - rect->top + 1;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint16_t pixel = src[y * w + x];
            int src_x = rect->left + x;
            int src_y = rect->top + y;

            /* Apply color transformation if not unchanged */
            if (color_mode != COLOR_MODE_UNCHANGED) {
                int r5 = (pixel >> 11) & 0x1F;
                int g6 = (pixel >> 5) & 0x3F;
                int b5 = pixel & 0x1F;

                if (color_mode == COLOR_MODE_DITHERED) {
                    /* Skip dithering for pure black - keep it solid */
                    if (pixel != 0) {
                        /* Ordered dithering with 4x4 Bayer matrix */
                        int dither = bayer4x4[src_y & 3][src_x & 3];
                        r5 = r5 + (dither >> 2);
                        g6 = g6 + (dither >> 1);
                        b5 = b5 + (dither >> 2);
                        if (r5 < 0) r5 = 0; if (r5 > 31) r5 = 31;
                        if (g6 < 0) g6 = 0; if (g6 > 63) g6 = 63;
                        if (b5 < 0) b5 = 0; if (b5 > 31) b5 = 31;
                    }
                } else if (color_mode == COLOR_MODE_DITHER2) {
                    /* Dither2: dither everything including black */
                    int dither = bayer4x4[src_y & 3][src_x & 3];
                    r5 = r5 + (dither >> 2);
                    g6 = g6 + (dither >> 1);
                    b5 = b5 + (dither >> 2);
                    if (r5 < 0) r5 = 0; if (r5 > 31) r5 = 31;
                    if (g6 < 0) g6 = 0; if (g6 > 63) g6 = 63;
                    if (b5 < 0) b5 = 0; if (b5 > 31) b5 = 31;
                } else if (color_mode == COLOR_MODE_NIGHT_DITHER) {
                    /* Night+Dither: apply gamma first, then dither */
                    r5 = gamma_r5[color_mode][r5];
                    g6 = gamma_g6[color_mode][g6];
                    b5 = gamma_b5[color_mode][b5];
                    /* Then apply dithering (skip for pure black) */
                    if (r5 != 0 || g6 != 0 || b5 != 0) {
                        int dither = bayer4x4[src_y & 3][src_x & 3];
                        r5 = r5 + (dither >> 2);
                        g6 = g6 + (dither >> 1);
                        b5 = b5 + (dither >> 2);
                        if (r5 < 0) r5 = 0; if (r5 > 31) r5 = 31;
                        if (g6 < 0) g6 = 0; if (g6 > 63) g6 = 63;
                        if (b5 < 0) b5 = 0; if (b5 > 31) b5 = 31;
                    }
                } else if (color_mode == COLOR_MODE_NIGHT_DITHER2) {
                    /* Night+Dither2: apply gamma first, then dither everything */
                    r5 = gamma_r5[color_mode][r5];
                    g6 = gamma_g6[color_mode][g6];
                    b5 = gamma_b5[color_mode][b5];
                    /* Dither including black for smoother transitions */
                    int dither = bayer4x4[src_y & 3][src_x & 3];
                    r5 = r5 + (dither >> 2);
                    g6 = g6 + (dither >> 1);
                    b5 = b5 + (dither >> 2);
                    if (r5 < 0) r5 = 0; if (r5 > 31) r5 = 31;
                    if (g6 < 0) g6 = 0; if (g6 > 63) g6 = 63;
                    if (b5 < 0) b5 = 0; if (b5 > 31) b5 = 31;
                } else {
                    /* Apply gamma/lifted blacks via lookup table */
                    r5 = gamma_r5[color_mode][r5];
                    g6 = gamma_g6[color_mode][g6];
                    b5 = gamma_b5[color_mode][b5];
                }
                pixel = (r5 << 11) | (g6 << 5) | b5;
            }

            /* Apply scaling and offset */
            for (int sy = 0; sy < scale_factor; sy++) {
                for (int sx = 0; sx < scale_factor; sx++) {
                    int dst_x = offset_x + src_x * scale_factor + sx;
                    int dst_y = offset_y + src_y * scale_factor + sy;
                    if (dst_x >= 0 && dst_x < SCREEN_WIDTH &&
                        dst_y >= 0 && dst_y < SCREEN_HEIGHT) {
                        framebuffer[dst_y * SCREEN_WIDTH + dst_x] = pixel;
                    }
                }
            }
        }
    }
    return 1;
}

/* Calculate scaling parameters based on video dimensions */
static void calculate_scaling(int width, int height) {
    video_width = width;
    video_height = height;

    /* Determine scale factor:
     * - If video fits in 106x80, scale 3x (318x240 max)
     * - If video fits in 160x120, scale 2x (320x240 max)
     * - Otherwise scale 1x (no scaling, just center)
     */
    if (width <= 106 && height <= 80) {
        scale_factor = 3;
    } else if (width <= 160 && height <= 120) {
        scale_factor = 2;
    } else {
        scale_factor = 1;
    }

    /* Calculate centering offset */
    int scaled_w = width * scale_factor;
    int scaled_h = height * scale_factor;
    offset_x = (SCREEN_WIDTH - scaled_w) / 2;
    offset_y = (SCREEN_HEIGHT - scaled_h) / 2;
    if (offset_x < 0) offset_x = 0;
    if (offset_y < 0) offset_y = 0;
}

/* Decode frame at index directly into framebuffer, return success */
static int decode_single_frame(int idx) {
    if (!video_file || idx >= total_frames) return 0;

    uint32_t offset = frame_offsets[idx];
    uint32_t size = frame_sizes[idx];

    if (size > MAX_JPEG_SIZE) size = MAX_JPEG_SIZE;
    if (size == 0) return 0;

    if (fseek(video_file, offset, SEEK_SET) != 0) return 0;
    if (fread(jpeg_buffer, 1, size, video_file) != size) return 0;

    if (jpeg_buffer[0] != 0xFF || jpeg_buffer[1] != 0xD8) return 0;

    /* Find/add EOI marker */
    int eoi_pos = -1;
    for (int i = size - 2; i >= 0; i--) {
        if (jpeg_buffer[i] == 0xFF && jpeg_buffer[i+1] == 0xD9) {
            eoi_pos = i;
            break;
        }
    }
    if (eoi_pos >= 0) size = eoi_pos + 2;
    else { jpeg_buffer[size] = 0xFF; jpeg_buffer[size+1] = 0xD9; size += 2; }

    jpeg_io.data = jpeg_buffer;
    jpeg_io.size = size;
    jpeg_io.pos = 0;

    JDEC jdec;
    if (jd_prepare(&jdec, tjpgd_input, tjpgd_work, TJPGD_WORKSPACE_SIZE, &jpeg_io) != JDR_OK)
        return 0;

    /* Detect video dimensions on first frame decode */
    if (idx == 0 || (video_width == 320 && video_height == 240)) {
        calculate_scaling(jdec.width, jdec.height);
        /* Clear framebuffer for proper centering */
        memset(framebuffer, 0, sizeof(framebuffer));
    }

    if (jd_decomp(&jdec, tjpgd_output, 0) != JDR_OK)
        return 0;

    decode_counter++;
    return 1;
}

/* Forward declaration */
static void refill_audio_ring(void);

/* Seek to specific frame */
static void seek_to_frame(int target_frame) {
    if (target_frame < 0) target_frame = 0;
    if (target_frame >= total_frames) target_frame = total_frames - 1;

    /* Set frame position */
    current_frame_idx = target_frame;
    repeat_counter = 0;

    /* Estimate audio position based on time */
    if (has_audio && audio_bytes_per_sample > 0) {
        uint64_t time_samples = (uint64_t)target_frame * audio_sample_rate / clip_fps;
        uint64_t target_bytes = time_samples * audio_bytes_per_sample;

        /* Find which audio chunk contains this position */
        uint64_t bytes_so_far = 0;
        audio_chunk_idx = 0;
        audio_chunk_pos = 0;

        while (audio_chunk_idx < total_audio_chunks) {
            if (bytes_so_far + audio_sizes[audio_chunk_idx] > target_bytes) {
                audio_chunk_pos = target_bytes - bytes_so_far;
                break;
            }
            bytes_so_far += audio_sizes[audio_chunk_idx];
            audio_chunk_idx++;
        }

        audio_samples_sent = time_samples;
        aring_read = 0;
        aring_write = 0;
        aring_count = 0;
        refill_audio_ring();
    }

    /* Decode the target frame immediately */
    decode_single_frame(target_frame);
}

/* Audio: read from disk into ring buffer */
static int read_audio_disk(uint8_t *buf, int bytes_needed) {
    int bytes_read = 0;
    while (bytes_read < bytes_needed && audio_chunk_idx < total_audio_chunks) {
        uint32_t chunk_size = audio_sizes[audio_chunk_idx];
        uint32_t remaining = chunk_size - audio_chunk_pos;
        uint32_t to_read = bytes_needed - bytes_read;
        if (to_read > remaining) to_read = remaining;

        uint32_t file_pos = audio_offsets[audio_chunk_idx] + audio_chunk_pos;
        if (fseek(video_file, file_pos, SEEK_SET) != 0) break;

        size_t got = fread(buf + bytes_read, 1, to_read, video_file);
        bytes_read += got;
        audio_chunk_pos += got;

        if (audio_chunk_pos >= chunk_size) {
            audio_chunk_idx++;
            audio_chunk_pos = 0;
        }
        if (got < to_read) break;
    }
    return bytes_read;
}

static void refill_audio_ring(void) {
    if (!has_audio || audio_chunk_idx >= total_audio_chunks) return;

    int free_space = AUDIO_RING_SIZE - aring_count;
    while (free_space > 0 && audio_chunk_idx < total_audio_chunks) {
        int before_wrap = AUDIO_RING_SIZE - aring_write;
        int to_read = (free_space < before_wrap) ? free_space : before_wrap;
        if (to_read > 4096) to_read = 4096;

        int got = read_audio_disk(audio_ring + aring_write, to_read);
        if (got <= 0) break;

        aring_write = (aring_write + got) % AUDIO_RING_SIZE;
        aring_count += got;
        free_space -= got;
    }
}

static int read_audio_ring(uint8_t *buf, int bytes_needed) {
    int bytes_read = 0;
    while (bytes_read < bytes_needed && aring_count > 0) {
        int before_wrap = AUDIO_RING_SIZE - aring_read;
        int avail = (aring_count < before_wrap) ? aring_count : before_wrap;
        int to_read = bytes_needed - bytes_read;
        if (to_read > avail) to_read = avail;

        memcpy(buf + bytes_read, audio_ring + aring_read, to_read);
        aring_read = (aring_read + to_read) % AUDIO_RING_SIZE;
        aring_count -= to_read;
        bytes_read += to_read;
    }
    return bytes_read;
}

static void play_audio_for_frame(void) {
    if (!has_audio || !audio_batch_cb || audio_bytes_per_sample == 0) return;

    if (aring_count < AUDIO_REFILL_THRESHOLD) {
        refill_audio_ring();
    }

    /* Sync based on current_frame_idx (frames actually shown) */
    /* Add ~0.1s audio lead to compensate for video-ahead-of-audio sync issue */
    /* At 22050Hz, 0.1s = 2205 samples. Using 2000 for round number. */
    #define AUDIO_SYNC_OFFSET 2000
    uint64_t expected = (uint64_t)current_frame_idx * audio_sample_rate / clip_fps + AUDIO_SYNC_OFFSET;
    int64_t to_send = expected - audio_samples_sent;

    if (to_send <= 0) return;
    if (to_send > MAX_AUDIO_BUFFER) to_send = MAX_AUDIO_BUFFER;

    int bytes_needed = to_send * audio_bytes_per_sample;
    uint8_t temp[MAX_AUDIO_BUFFER * 4];
    if (bytes_needed > (int)sizeof(temp)) {
        bytes_needed = sizeof(temp);
        to_send = bytes_needed / audio_bytes_per_sample;
    }

    int got_bytes = read_audio_ring(temp, bytes_needed);
    int got_samples = got_bytes / audio_bytes_per_sample;
    if (got_samples <= 0) return;

    int out = 0;
    if (audio_channels == 1 && audio_bits == 16) {
        int16_t *src = (int16_t *)temp;
        for (int i = 0; i < got_samples && out < MAX_AUDIO_BUFFER; i++) {
            audio_out_buffer[out * 2] = src[i];
            audio_out_buffer[out * 2 + 1] = src[i];
            out++;
        }
    } else if (audio_channels == 2 && audio_bits == 16) {
        int16_t *src = (int16_t *)temp;
        for (int i = 0; i < got_samples && out < MAX_AUDIO_BUFFER; i++) {
            audio_out_buffer[out * 2] = src[i * 2];
            audio_out_buffer[out * 2 + 1] = src[i * 2 + 1];
            out++;
        }
    } else if (audio_bits == 8) {
        for (int i = 0; i < got_samples && out < MAX_AUDIO_BUFFER; i++) {
            int16_t s = ((int16_t)temp[i * audio_channels] - 128) << 8;
            audio_out_buffer[out * 2] = s;
            audio_out_buffer[out * 2 + 1] = s;
            out++;
        }
    }

    if (out > 0) {
        audio_batch_cb(audio_out_buffer, out);
        audio_samples_sent += out;
    }
}

static int open_video(const char *path) {
    if (video_file) fclose(video_file);
    video_file = fopen(path, "rb");
    if (!video_file) return 0;
    if (!parse_avi()) { fclose(video_file); video_file = NULL; return 0; }

    /* Reset all state */
    current_frame_idx = 0;

    audio_chunk_idx = 0;
    audio_chunk_pos = 0;
    audio_samples_sent = 0;
    aring_read = 0;
    aring_write = 0;
    aring_count = 0;

    repeat_counter = 0;
    run_counter = 0;
    decode_counter = 0;
    sec_counter = 0;

    /* Pre-fill audio buffer only */
    refill_audio_ring();

    /* Decode first frame */
    decode_single_frame(0);

    is_playing = 1;
    return 1;
}

/* Load AVI file from path - used by file browser */
static int load_avi_file(const char *path) {
    return open_video(path) ? 0 : -1;  /* return 0 on success, -1 on failure */
}

/* Libretro API */
void retro_init(void) {
    memset(framebuffer, 0, sizeof(framebuffer));
    init_color_tables();
    load_settings();  /* Load saved settings (color mode, show_time, etc.) */
}
void retro_deinit(void) { if (video_file) fclose(video_file); }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_set_controller_port_device(unsigned p, unsigned d) { (void)p; (void)d; }

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "A ZERO Player";
    info->library_version = "0.72";
    info->need_fullpath = 1;
    info->valid_extensions = "avi";
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    info->timing.fps = 30;
    info->timing.sample_rate = AUDIO_SAMPLE_RATE;
    info->geometry.base_width = SCREEN_WIDTH;
    info->geometry.base_height = SCREEN_HEIGHT;
    info->geometry.max_width = SCREEN_WIDTH;
    info->geometry.max_height = SCREEN_HEIGHT;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
}

void retro_set_environment(retro_environment_t cb) { environ_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }

void retro_reset(void) {
    current_frame_idx = 0;
    audio_chunk_idx = 0;
    audio_chunk_pos = 0;
    audio_samples_sent = 0;
    aring_read = 0;
    aring_write = 0;
    aring_count = 0;
    repeat_counter = 0;
}

void retro_run(void) {
    input_poll_cb();

    /* Input handling */
    int cur_a = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
    int cur_b = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
    int cur_left = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
    int cur_right = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
    int cur_l = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
    int cur_r = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
    int cur_start = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
    int cur_up = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
    int cur_down = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);

    /* Key lock: L+R held for 2 seconds toggles lock */
    if (cur_l && cur_r) {
        lock_hold_counter++;
        if (lock_hold_counter >= LOCK_HOLD_FRAMES) {
            is_locked = !is_locked;
            lock_hold_counter = 0;
            lock_indicator_timer = LOCK_INDICATOR_FRAMES;  /* show indicator */
            /* Show lock/unlock icon */
            icon_type = is_locked ? ICON_LOCK : ICON_UNLOCK;
            icon_timer = ICON_FRAMES;
        }
    } else {
        lock_hold_counter = 0;
    }

    /* Decrease indicator timer */
    if (lock_indicator_timer > 0) {
        lock_indicator_timer--;
    }

    /* All other controls only work when NOT locked */
    if (!is_locked) {
        /* START: toggle menu (on press) */
        if (cur_start && !prev_start) {
            if (menu_active) {
                /* Closing menu - refresh frame and restore play state */
                menu_active = 0;
                color_submenu_active = 0;  /* close any submenus */
                submenu_active = 0;
                file_browser_active = 0;  /* close file browser */
                decode_single_frame(current_frame_idx);  /* refresh frame */
                is_paused = was_paused_before_menu;  /* restore pause state */
                if (!is_paused) {
                    icon_type = ICON_PLAY;
                    icon_timer = ICON_FRAMES;
                }
            } else {
                /* Opening menu */
                menu_active = 1;
                was_paused_before_menu = is_paused;  /* remember pause state */
                is_paused = 1;  /* pause when menu opens */
                /* Initialize slider to current position */
                if (total_frames > 0) {
                    seek_position = (current_frame_idx * 20) / total_frames;
                    if (seek_position > 20) seek_position = 20;
                }
            }
        }

        if (menu_active) {
            /* Handle color submenu (scrollable mode picker) */
            if (color_submenu_active) {
                /* Up/Down to navigate modes - wraps around */
                if (cur_up && !prev_up) {
                    if (color_mode > 0) {
                        color_mode--;
                    } else {
                        color_mode = COLOR_MODE_COUNT - 1;  /* wrap to last */
                    }
                    decode_single_frame(current_frame_idx);
                    /* Adjust scroll to keep selection visible */
                    if (color_mode < color_submenu_scroll) {
                        color_submenu_scroll = color_mode;
                    }
                    if (color_mode >= color_submenu_scroll + 8) {
                        color_submenu_scroll = color_mode - 7;
                    }
                }
                if (cur_down && !prev_down) {
                    if (color_mode < COLOR_MODE_COUNT - 1) {
                        color_mode++;
                    } else {
                        color_mode = 0;  /* wrap to first */
                    }
                    decode_single_frame(current_frame_idx);
                    /* Adjust scroll to keep selection visible */
                    if (color_mode >= color_submenu_scroll + 8) {
                        color_submenu_scroll = color_mode - 7;
                    }
                    if (color_mode < color_submenu_scroll) {
                        color_submenu_scroll = color_mode;
                    }
                }
                /* A or B to close */
                if ((cur_a && !prev_a) || (cur_b && !prev_b)) {
                    color_submenu_active = 0;
                }
                /* L/R shoulders do nothing in color submenu */
            }
            /* Handle file browser navigation */
            else if (file_browser_active) {
                /* Up/Down to navigate files */
                if (cur_up && !prev_up) {
                    if (fb_selection > 0) {
                        fb_selection--;
                        if (fb_selection < fb_scroll) {
                            fb_scroll = fb_selection;
                        }
                    }
                }
                if (cur_down && !prev_down) {
                    if (fb_selection < fb_file_count - 1) {
                        fb_selection++;
                        if (fb_selection >= fb_scroll + FB_VISIBLE_ITEMS) {
                            fb_scroll = fb_selection - FB_VISIBLE_ITEMS + 1;
                        }
                    }
                }
                /* A to enter directory or load file */
                if (cur_a && !prev_a) {
                    fb_enter_selected();
                }
                /* B to go back (parent directory or close browser) */
                if (cur_b && !prev_b) {
                    /* Try to go to parent directory */
                    char *last_slash = strrchr(fb_current_path, '/');
                    if (last_slash && last_slash != fb_current_path) {
                        *last_slash = '\0';
                        fb_selection = 0;
                        fb_scroll = 0;
                        fb_scan_directory();
                    } else {
                        /* At root or /mnt - close file browser */
                        file_browser_active = 0;
                    }
                }
            }
            /* Handle other submenus */
            else if (submenu_active > 0) {
                if (cur_a && !prev_a) {
                    submenu_active = 0;
                }
            }
            /* Main menu controls */
            else {
                /* Menu navigation with Up/Down */
                if (cur_up && !prev_up) {
                    menu_selection = (menu_selection - 1 + MENU_ITEMS) % MENU_ITEMS;
                    save_feedback_timer = 0;  /* hide popup on navigation */
                }
                if (cur_down && !prev_down) {
                    menu_selection = (menu_selection + 1) % MENU_ITEMS;
                    save_feedback_timer = 0;  /* hide popup on navigation */
                }

                /* L/R shoulders for cycling options (d-pad removed - too easy to trigger accidentally) */
                int cycle_prev = (cur_l && !prev_l);
                int cycle_next = (cur_r && !prev_r);

                /* Handle option cycling based on menu selection */
                if (cycle_prev || cycle_next) {
                    switch (menu_selection) {
                        case 2:  /* Color Mode */
                            if (cycle_next) {
                                color_mode = (color_mode + 1) % COLOR_MODE_COUNT;
                            } else {
                                color_mode = (color_mode - 1 + COLOR_MODE_COUNT) % COLOR_MODE_COUNT;
                            }
                            decode_single_frame(current_frame_idx);
                            break;
                        case 4:  /* Show Time */
                            show_time = !show_time;
                            break;
                        case 5:  /* Debug Info */
                            show_debug = !show_debug;
                            break;
                    }
                }

                /* Slider control for Go to Position (item 1) - only Left/Right */
                if (menu_selection == 1) {
                    if (cur_left && !prev_left) {
                        if (seek_position > 0) {
                            seek_position--;
                            int target_frame = (total_frames > 0) ? (seek_position * total_frames / 20) : 0;
                            seek_to_frame(target_frame);
                            decode_single_frame(current_frame_idx);
                        }
                    }
                    if (cur_right && !prev_right) {
                        if (seek_position < 20) {
                            seek_position++;
                            int target_frame = (total_frames > 0) ? (seek_position * total_frames / 20) : 0;
                            seek_to_frame(target_frame);
                            decode_single_frame(current_frame_idx);
                        }
                    }
                }
                /* Menu action on A */
                if (cur_a && !prev_a) {
                    switch (menu_selection) {
                        case 0:  /* Load File - open file browser */
                            file_browser_active = 1;
                            fb_scan_directory();
                            break;
                        case 1:  /* Go to Position - close menu and resume */
                            is_paused = was_paused_before_menu;
                            menu_active = 0;
                            if (!is_paused) {
                                icon_type = ICON_PLAY;
                                icon_timer = ICON_FRAMES;
                            }
                            break;
                        case 2:  /* Color Mode - open submenu */
                            color_submenu_active = 1;
                            /* Scroll to show current mode in view */
                            color_submenu_scroll = color_mode - 3;
                            if (color_submenu_scroll < 0) color_submenu_scroll = 0;
                            if (color_submenu_scroll > COLOR_MODE_COUNT - 8)
                                color_submenu_scroll = COLOR_MODE_COUNT - 8;
                            break;
                        case 3:  /* Resume - unpause and close menu */
                            is_paused = 0;
                            was_paused_before_menu = 0;
                            icon_type = ICON_PLAY;
                            icon_timer = ICON_FRAMES;
                            menu_active = 0;
                            decode_single_frame(current_frame_idx);
                            break;
                        case 4:  /* Show Time toggle */
                            show_time = !show_time;
                            break;
                        case 5:  /* Debug Info toggle */
                            show_debug = !show_debug;
                            break;
                        case 6:  /* Restart */
                            seek_to_frame(0);
                            is_paused = 0;
                            was_paused_before_menu = 0;
                            icon_type = ICON_PLAY;
                            icon_timer = ICON_FRAMES;
                            menu_active = 0;
                            break;
                        case 7:  /* Save Settings */
                            save_settings();
                            save_feedback_timer = SAVE_FEEDBACK_FRAMES;
                            break;
                        case 8:  /* Instructions */
                            submenu_active = 1;
                            break;
                        case 9:  /* About */
                            submenu_active = 2;
                            break;
                    }
                }

            }
        } else {
            /* Normal controls when menu is closed */
            /* A button: toggle pause (on press) */
            if (cur_a && !prev_a) {
                is_paused = !is_paused;
                if (is_paused) {
                    icon_type = ICON_PAUSE;
                    icon_timer = ICON_FRAMES;
                } else {
                    icon_type = ICON_PLAY;
                    icon_timer = ICON_FRAMES;
                }
            }

            /* Left: skip -15 seconds (on press) */
            if (cur_left && !prev_left && !is_paused) {
                int skip_frames = 15 * clip_fps;
                seek_to_frame(current_frame_idx - skip_frames);
                icon_type = ICON_SKIP_LEFT;
                icon_timer = ICON_FRAMES;
            }

            /* Right: skip +15 seconds (on press) */
            if (cur_right && !prev_right && !is_paused) {
                int skip_frames = 15 * clip_fps;
                seek_to_frame(current_frame_idx + skip_frames);
                icon_type = ICON_SKIP_RIGHT;
                icon_timer = ICON_FRAMES;
            }

            /* Up: skip +1 minute (on press) */
            if (cur_up && !prev_up && !is_paused) {
                int skip_frames = 60 * clip_fps;
                seek_to_frame(current_frame_idx + skip_frames);
                icon_type = ICON_SKIP_FWD_1M;
                icon_timer = ICON_FRAMES;
            }

            /* Down: skip -1 minute (on press) */
            if (cur_down && !prev_down && !is_paused) {
                int skip_frames = 60 * clip_fps;
                seek_to_frame(current_frame_idx - skip_frames);
                icon_type = ICON_SKIP_BACK_1M;
                icon_timer = ICON_FRAMES;
            }
        }
    }

    prev_a = cur_a;
    prev_b = cur_b;
    prev_left = cur_left;
    prev_right = cur_right;
    prev_start = cur_start;
    prev_up = cur_up;
    prev_down = cur_down;
    prev_l = cur_l;
    prev_r = cur_r;

    run_counter++;
    sec_counter++;
    if (sec_counter >= 30) {
        runs_per_sec = run_counter;
        decodes_per_sec = decode_counter;
        run_counter = 0;
        decode_counter = 0;
        sec_counter = 0;
    }

    if (is_playing && !is_paused) {
        /* Direct decode - no video buffer! */
        if (repeat_counter == 0) {
            /* New source frame needed - decode directly to framebuffer */
            if (current_frame_idx < total_frames) {
                decode_single_frame(current_frame_idx);
            }
        }
        /* else: same frame displayed again (repeat), framebuffer already has it */

        repeat_counter++;
        if (repeat_counter >= repeat_count) {
            repeat_counter = 0;
            current_frame_idx++;
        }

        /* Play audio synced to frame position */
        play_audio_for_frame();

        /* Loop when finished */
        if (current_frame_idx >= total_frames) {
            current_frame_idx = 0;
            audio_chunk_idx = 0;
            audio_chunk_pos = 0;
            audio_samples_sent = 0;
            aring_read = 0;
            aring_write = 0;
            aring_count = 0;
            repeat_counter = 0;
            refill_audio_ring();
        }
    }

    /* Clear black bars for videos smaller than screen - BEFORE any UI drawing */
    if (offset_y > 0) {
        int scaled_h = video_height * scale_factor;
        int bottom_start = offset_y + scaled_h;
        /* Top bar */
        memset(framebuffer, 0, offset_y * SCREEN_WIDTH * sizeof(pixel_t));
        /* Bottom bar */
        if (bottom_start < SCREEN_HEIGHT) {
            memset(&framebuffer[bottom_start * SCREEN_WIDTH], 0,
                   (SCREEN_HEIGHT - bottom_start) * SCREEN_WIDTH * sizeof(pixel_t));
        }
    }

    /* Time display in top left (when show_time enabled) */
    if (show_time && !menu_active) {
        /* Calculate current time from frame position */
        int total_secs = (clip_fps > 0) ? (current_frame_idx / clip_fps) : 0;
        int total_duration = (clip_fps > 0 && total_frames > 0) ? (total_frames / clip_fps) : 0;
        int cur_min = total_secs / 60;
        int cur_sec = total_secs % 60;
        int dur_min = total_duration / 60;
        int dur_sec = total_duration % 60;

        /* Draw time: MM:SS / MM:SS (handles 100+ minutes correctly) */
        int tx = 2;
        draw_num(tx, 2, cur_min, 0xFFFF);
        tx += num_width(cur_min);
        draw_str(tx, 2, ":", 0xFFFF); tx += 6;
        if (cur_sec < 10) { draw_str(tx, 2, "0", 0xFFFF); tx += 6; }
        draw_num(tx, 2, cur_sec, 0xFFFF);
        tx += num_width(cur_sec);
        draw_str(tx, 2, "/", 0x7BEF); tx += 6;
        draw_num(tx, 2, dur_min, 0x7BEF);
        tx += num_width(dur_min);
        draw_str(tx, 2, ":", 0x7BEF); tx += 6;
        if (dur_sec < 10) { draw_str(tx, 2, "0", 0x7BEF); tx += 6; }
        draw_num(tx, 2, dur_sec, 0x7BEF);
    }

    /* Key lock indicator - always show when locked */
    if (lock_indicator_timer > 0 || is_locked) {
        if (is_locked) {
            draw_str(220, 2, "KEY LOCK", 0xFFE0);  /* Yellow */
        } else {
            draw_str(220, 2, "UNLOCKED", 0x07E0);  /* Green */
        }
    }

    /* Pause indicator (only when not in menu, debug off, and time off) */
    if (is_paused && !is_locked && !menu_active && !show_debug && !show_time) {
        draw_str(2, 2, "PAUSED", 0xF800);  /* Red */
    } else if (is_paused && !is_locked && !menu_active && show_time && !show_debug) {
        draw_str(140, 2, "PAUSED", 0xF800);  /* Red, next to time */
    }

    /* DEBUG PANEL - only show when show_debug enabled */
    /* Line 1 (y=2) left empty so time display fits nicely above */
    if (show_debug) {
        draw_str(2, 12, "FPS:", 0xFFFF);
        draw_num(28, 12, clip_fps, 0xFFE0);
        draw_str(52, 12, "Rep:", 0xFFFF);
        draw_num(78, 12, repeat_count, 0xFFE0);

        /* Video dimensions and scale */
        draw_num(110, 12, video_width, 0x07FF);
        draw_str(140, 12, "x", 0xFFFF);
        draw_num(148, 12, video_height, 0x07FF);
        draw_str(178, 12, "S:", 0xFFFF);
        draw_num(192, 12, scale_factor, 0xF81F);

        if (is_paused) {
            draw_str(250, 12, "PAUSED", 0xF800);
        }

        draw_str(2, 22, "Frame:", 0xFFFF);
        draw_num(40, 22, current_frame_idx, 0x07FF);
        draw_str(82, 22, "/", 0xFFFF);
        draw_num(90, 22, total_frames, 0x07FF);

        draw_str(150, 22, "Dec/s:", 0xFFFF);
        draw_num(192, 22, decodes_per_sec, 0xF81F);

        /* Audio buffer status */
        if (has_audio) {
            draw_str(2, 32, "ABuf:", 0xFFFF);
            int apct = (aring_count * 100) / AUDIO_RING_SIZE;
            draw_num(34, 32, apct, apct > 50 ? 0x07E0 : 0xF800);
            draw_str(58, 32, "%", 0xFFFF);

            draw_str(80, 32, "Aud:", 0xFFFF);
            draw_num(106, 32, audio_sample_rate, 0xF81F);
        } else {
            draw_str(2, 32, "Audio: none", 0x7BEF);
        }
    }

    /* Draw menu overlay on top */
    if (menu_active) {
        draw_menu();
        /* Draw file browser on top of menu when active */
        if (file_browser_active) {
            draw_file_browser();
        }
        /* Draw save feedback popup */
        if (save_feedback_timer > 0) {
            int popup_x = 80;
            int popup_y = 100;
            int popup_w = 160;
            int popup_h = 40;
            /* Dark background with border */
            draw_fill_rect(popup_x, popup_y, popup_x + popup_w, popup_y + popup_h, 0x0000);
            draw_rect(popup_x, popup_y, popup_x + popup_w, popup_y + popup_h, 0x07E0);
            draw_rect(popup_x + 1, popup_y + 1, popup_x + popup_w - 1, popup_y + popup_h - 1, 0x07E0);
            /* Message */
            draw_str(popup_x + 20, popup_y + 12, "Settings Saved!", 0x07E0);
            draw_str(popup_x + 45, popup_y + 26, "a0player.cfg", 0x7BEF);
            save_feedback_timer--;
        }
    }

    /* No file message when opened without file */
    if (no_file_loaded && !menu_active) {
        /* Black background */
        memset(framebuffer, 0, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(pixel_t));
        /* Center message */
        draw_str(110, 110, "No file loaded", 0xFFFF);
        draw_str(80, 130, "Press START to open menu", 0x7BEF);
    }

    /* Draw visual feedback icon */
    if (icon_timer > 0 && !menu_active) {
        draw_icon(icon_type);
        icon_timer--;
        if (icon_timer == 0) {
            icon_type = ICON_NONE;
        }
    }

    video_cb(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(pixel_t));
}

bool retro_load_game(const struct retro_game_info *info) {
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) return false;

    /* Ensure VIDEOS directory exists */
    fb_ensure_videos_dir();

    if (info && info->path && info->path[0] != '\0') {
        if (open_video(info->path)) {
            strcpy(loaded_file_path, info->path);
            no_file_loaded = 0;
        } else {
            no_file_loaded = 1;
        }
    } else {
        /* No file provided - show file browser on start */
        no_file_loaded = 1;
        is_paused = 1;
    }
    return true;
}

void retro_unload_game(void) { if (video_file) fclose(video_file); is_playing = 0; }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n) { return false; }
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *d, size_t s) { return false; }
bool retro_unserialize(const void *d, size_t s) { return false; }
void *retro_get_memory_data(unsigned id) { return NULL; }
size_t retro_get_memory_size(unsigned id) { return 0; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned i, bool e, const char *c) {}
