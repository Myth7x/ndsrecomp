#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GL_FLIP_NONE (1 << 0)
#define GL_FLIP_V (1 << 1)
#define GL_FLIP_H (1 << 2)
#define EASYGL2D_WIDTH 256
#define EASYGL2D_HEIGHT 384
#define EASYGL2D_HUD_WIDTH 320
#define EASYGL2D_WINDOW_WIDTH (EASYGL2D_WIDTH + EASYGL2D_HUD_WIDTH)

typedef int32_t s32;
typedef uint16_t u16;
typedef uint8_t u8;

typedef struct {
    int width;
    int height;
    int u_off;
    int v_off;
    int textureID;
} glImage;

typedef struct {
    uint64_t frame;
    double fps;
    double frame_ms;
    double emulation_ms;
    double video_ms;
    double present_ms;
    double host_load;
    double arm9_mips;
    double arm7_mips;
    uint32_t arm9_pc;
    uint32_t arm7_pc;
    uint32_t display_a;
    uint32_t display_b;
    uint16_t vcount;
    bool running;
} EasyGL2DStats;

bool easygl2d_init(const char *title, int scale, bool headless);
bool easygl2d_poll(void);
uint16_t easygl2d_keyinput(void);
uint16_t easygl2d_extkeyin(void);
bool easygl2d_touching(void);
uint16_t easygl2d_touch_x(void);
uint16_t easygl2d_touch_y(void);
void easygl2d_set_stats(const EasyGL2DStats *value);
void easygl2d_present(void);
void easygl2d_shutdown(void);
const uint32_t *easygl2d_framebuffer(void);
uint64_t easygl2d_framebuffer_hash(void);

void glScreen2D(void);
void glBegin2D(void);
void glEnd2D(void);
void glPutPixel(int x, int y, int color);
void glLine(int x1, int y1, int x2, int y2, int color);
void glBox(int x1, int y1, int x2, int y2, int color);
void glBoxFilled(int x1, int y1, int x2, int y2, int color);
void glBoxFilledGradient(int x1, int y1, int x2, int y2,
                         int color1, int color2, int color3, int color4);
void glTriangle(int x1, int y1, int x2, int y2, int x3, int y3, int color);
void glTriangleFilled(int x1, int y1, int x2, int y2, int x3, int y3, int color);
