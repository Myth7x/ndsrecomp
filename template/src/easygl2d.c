#include "easygl2d.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

static uint32_t framebuffer[EASYGL2D_WIDTH * EASYGL2D_HEIGHT];
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static bool is_headless;
static uint16_t keyinput = 0x03ffu;
static uint16_t extkeyin = 0x007fu;
static bool touch_down;
static uint16_t touch_x;
static uint16_t touch_y;
static EasyGL2DStats stats;

static bool button_down(uint16_t value, unsigned bit) {
    return (value & (uint16_t)(1u << bit)) == 0;
}

static void hud_button(float y, const char *button, const char *key, bool down) {
    const SDL_FRect row = {262.0f, y - 2.0f, 148.0f, 12.0f};
    if (down) {
        SDL_SetRenderDrawColor(renderer, 51, 209, 122, 255);
        SDL_RenderFillRect(renderer, &row);
        SDL_SetRenderDrawColor(renderer, 12, 23, 20, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 209, 218, 229, 255);
    }
    SDL_RenderDebugText(renderer, 268.0f, y, button);
    SDL_RenderDebugText(renderer, 336.0f, y, key);
}

static void render_hud(void) {
    const SDL_FRect panel = {256.0f, 0.0f, EASYGL2D_HUD_WIDTH,
                             EASYGL2D_HEIGHT};
    SDL_SetRenderDrawColor(renderer, 18, 23, 31, 255);
    SDL_RenderFillRect(renderer, &panel);

    SDL_SetRenderDrawColor(renderer, 89, 221, 255, 255);
    SDL_RenderDebugText(renderer, 268.0f, 10.0f, "INPUT HUD");
    SDL_SetRenderDrawColor(renderer, 108, 120, 137, 255);
    SDL_RenderDebugText(renderer, 268.0f, 28.0f, "BUTTON   KEY");

    hud_button(42.0f,  "A",      "X",         button_down(keyinput, 0u));
    hud_button(56.0f,  "B",      "Z",         button_down(keyinput, 1u));
    hud_button(70.0f,  "X",      "S",         button_down(extkeyin, 0u));
    hud_button(84.0f,  "Y",      "A",         button_down(extkeyin, 1u));
    hud_button(98.0f,  "L",      "Q",         button_down(keyinput, 9u));
    hud_button(112.0f, "R",      "W",         button_down(keyinput, 8u));
    hud_button(126.0f, "START",  "ENTER",     button_down(keyinput, 3u));
    hud_button(140.0f, "SELECT", "BACKSPACE", button_down(keyinput, 2u));
    hud_button(154.0f, "UP",     "UP",        button_down(keyinput, 6u));
    hud_button(168.0f, "DOWN",   "DOWN",      button_down(keyinput, 7u));
    hud_button(182.0f, "LEFT",   "LEFT",      button_down(keyinput, 5u));
    hud_button(196.0f, "RIGHT",  "RIGHT",     button_down(keyinput, 4u));

    SDL_SetRenderDrawColor(renderer, 52, 62, 76, 255);
    SDL_RenderLine(renderer, 264.0f, 216.0f, 408.0f, 216.0f);
    SDL_SetRenderDrawColor(renderer, 89, 221, 255, 255);
    SDL_RenderDebugText(renderer, 268.0f, 226.0f, "TOUCH");
    SDL_SetRenderDrawColor(renderer, touch_down ? 51 : 108,
                           touch_down ? 209 : 120,
                           touch_down ? 122 : 137, 255);
    SDL_RenderDebugText(renderer, 352.0f, 226.0f,
                        touch_down ? "DOWN" : "UP");

    const SDL_FRect touch = {272.0f, 250.0f, 128.0f, 96.0f};
    SDL_SetRenderDrawColor(renderer, 8, 12, 17, 255);
    SDL_RenderFillRect(renderer, &touch);
    SDL_SetRenderDrawColor(renderer, 80, 94, 112, 255);
    SDL_RenderRect(renderer, &touch);
    if (touch_down) {
        const float x = touch.x + (float)touch_x * (touch.w - 1.0f) / 255.0f;
        const float y = touch.y + (float)touch_y * (touch.h - 1.0f) / 191.0f;
        SDL_SetRenderDrawColor(renderer, 89, 221, 255, 255);
        SDL_RenderLine(renderer, x - 5.0f, y, x + 5.0f, y);
        SDL_RenderLine(renderer, x, y - 5.0f, x, y + 5.0f);
    }
    SDL_SetRenderDrawColor(renderer, 209, 218, 229, 255);
    SDL_RenderDebugTextFormat(renderer, 268.0f, 362.0f, "X %3u  Y %3u",
                              (unsigned)touch_x, (unsigned)touch_y);

    SDL_SetRenderDrawColor(renderer, 52, 62, 76, 255);
    SDL_RenderLine(renderer, 416.0f, 8.0f, 416.0f, 376.0f);
    SDL_SetRenderDrawColor(renderer, 89, 221, 255, 255);
    SDL_RenderDebugText(renderer, 428.0f, 10.0f, "DEBUG HUD");
    SDL_SetRenderDrawColor(renderer, stats.running ? 51 : 239,
                           stats.running ? 209 : 93,
                           stats.running ? 122 : 112, 255);
    SDL_RenderDebugText(renderer, 520.0f, 10.0f,
                        stats.running ? "RUN" : "TRAP");
    SDL_SetRenderDrawColor(renderer, 209, 218, 229, 255);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 30.0f, "FPS       %6.1f", stats.fps);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 44.0f, "FRAME     %6.2f MS", stats.frame_ms);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 58.0f, "EMU       %6.2f MS", stats.emulation_ms);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 72.0f, "VIDEO     %6.2f MS", stats.video_ms);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 86.0f, "PRESENT   %6.2f MS", stats.present_ms);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 100.0f, "HOST LOAD %6.1f %%", stats.host_load);

    SDL_SetRenderDrawColor(renderer, 52, 62, 76, 255);
    SDL_RenderLine(renderer, 424.0f, 122.0f, 568.0f, 122.0f);
    SDL_SetRenderDrawColor(renderer, 89, 221, 255, 255);
    SDL_RenderDebugText(renderer, 428.0f, 132.0f, "CPU");
    SDL_SetRenderDrawColor(renderer, 209, 218, 229, 255);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 150.0f, "ARM9      %6.1f MIPS", stats.arm9_mips);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 164.0f, "ARM7      %6.1f MIPS", stats.arm7_mips);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 182.0f, "PC9       %08X", stats.arm9_pc);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 196.0f, "PC7       %08X", stats.arm7_pc);

    SDL_SetRenderDrawColor(renderer, 52, 62, 76, 255);
    SDL_RenderLine(renderer, 424.0f, 218.0f, 568.0f, 218.0f);
    SDL_SetRenderDrawColor(renderer, 89, 221, 255, 255);
    SDL_RenderDebugText(renderer, 428.0f, 228.0f, "RENDER");
    SDL_SetRenderDrawColor(renderer, 209, 218, 229, 255);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 246.0f, "DISP A    %08X", stats.display_a);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 260.0f, "DISP B    %08X", stats.display_b);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 274.0f, "MODE A/B  %u / %u",
                              (stats.display_a >> 16) & 3u,
                              (stats.display_b >> 16) & 3u);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 288.0f, "VCOUNT    %3u",
                              (unsigned)stats.vcount);
    SDL_RenderDebugTextFormat(renderer, 428.0f, 306.0f, "FRAME     %llu",
                              (unsigned long long)stats.frame);
}

static void update_key(SDL_Keycode key, bool pressed) {
    uint16_t *register_value = &keyinput;
    unsigned bit;
    switch (key) {
    case SDLK_X: bit = 0u; break;
    case SDLK_Z: bit = 1u; break;
    case SDLK_BACKSPACE: bit = 2u; break;
    case SDLK_RETURN: bit = 3u; break;
    case SDLK_RIGHT: bit = 4u; break;
    case SDLK_LEFT: bit = 5u; break;
    case SDLK_UP: bit = 6u; break;
    case SDLK_DOWN: bit = 7u; break;
    case SDLK_W: bit = 8u; break;
    case SDLK_Q: bit = 9u; break;
    case SDLK_S: register_value = &extkeyin; bit = 0u; break;
    case SDLK_A: register_value = &extkeyin; bit = 1u; break;
    default: return;
    }
    if (pressed) *register_value &= (uint16_t)~(1u << bit);
    else *register_value |= (uint16_t)(1u << bit);
}

static void update_touch(float window_x, float window_y, bool pressed) {
    float logical_x = window_x;
    float logical_y = window_y;
    if (renderer != NULL)
        (void)SDL_RenderCoordinatesFromWindow(renderer, window_x, window_y,
                                              &logical_x, &logical_y);
    touch_down = pressed && logical_x >= 0.0f && logical_x < 256.0f &&
                 logical_y >= 192.0f && logical_y < 384.0f;
    if (touch_down) {
        touch_x = (uint16_t)logical_x;
        touch_y = (uint16_t)(logical_y - 192.0f);
        extkeyin &= (uint16_t)~0x40u;
    } else {
        extkeyin |= 0x40u;
    }
}

static uint32_t rgba15(int color) {
    const unsigned r5 = (unsigned)color & 31u;
    const unsigned g5 = ((unsigned)color >> 5) & 31u;
    const unsigned b5 = ((unsigned)color >> 10) & 31u;
    const uint32_t r = (r5 * 255u + 15u) / 31u;
    const uint32_t g = (g5 * 255u + 15u) / 31u;
    const uint32_t b = (b5 * 255u + 15u) / 31u;
    return (r << 24) | (g << 16) | (b << 8) | 0xffu;
}

bool easygl2d_init(const char *title, int scale, bool headless) {
    is_headless = headless;
    glScreen2D();
    if (headless)
        return true;
    if (!SDL_Init(SDL_INIT_VIDEO))
        return false;
    if (scale < 1)
        scale = 1;
    if (!SDL_CreateWindowAndRenderer(title, EASYGL2D_WINDOW_WIDTH * scale,
                                     EASYGL2D_HEIGHT * scale,
                                     SDL_WINDOW_RESIZABLE, &window, &renderer))
        return false;
    SDL_SetRenderLogicalPresentation(renderer, EASYGL2D_WINDOW_WIDTH,
                                     EASYGL2D_HEIGHT,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STREAMING,
                                EASYGL2D_WIDTH, EASYGL2D_HEIGHT);
    return texture != NULL;
}

bool easygl2d_poll(void) {
    SDL_Event event;
    if (is_headless)
        return false;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT)
            return false;
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)
            return false;
        if (event.type == SDL_EVENT_KEY_DOWN)
            update_key(event.key.key, true);
        else if (event.type == SDL_EVENT_KEY_UP)
            update_key(event.key.key, false);
        else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                 event.button.button == SDL_BUTTON_LEFT)
            update_touch(event.button.x, event.button.y, true);
        else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                 event.button.button == SDL_BUTTON_LEFT)
            update_touch(event.button.x, event.button.y, false);
        else if (event.type == SDL_EVENT_MOUSE_MOTION && touch_down)
            update_touch(event.motion.x, event.motion.y, true);
    }
    return true;
}

uint16_t easygl2d_keyinput(void) {
    return keyinput;
}

uint16_t easygl2d_extkeyin(void) {
    return extkeyin;
}

bool easygl2d_touching(void) {
    return touch_down;
}

uint16_t easygl2d_touch_x(void) {
    return touch_x;
}

uint16_t easygl2d_touch_y(void) {
    return touch_y;
}

void easygl2d_set_stats(const EasyGL2DStats *value) {
    if (value != NULL)
        stats = *value;
}

void easygl2d_present(void) {
    if (is_headless || texture == NULL)
        return;
    SDL_UpdateTexture(texture, NULL, framebuffer, EASYGL2D_WIDTH * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    const SDL_FRect game = {0.0f, 0.0f, EASYGL2D_WIDTH, EASYGL2D_HEIGHT};
    SDL_RenderTexture(renderer, texture, NULL, &game);
    render_hud();
    SDL_RenderPresent(renderer);
}

void easygl2d_shutdown(void) {
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    texture = NULL;
    renderer = NULL;
    window = NULL;
    if (!is_headless)
        SDL_Quit();
}

const uint32_t *easygl2d_framebuffer(void) {
    return framebuffer;
}

uint64_t easygl2d_framebuffer_hash(void) {
    uint64_t hash = UINT64_C(1469598103934665603);
    const unsigned char *bytes = (const unsigned char *)framebuffer;
    for (size_t i = 0; i < sizeof(framebuffer); ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void glScreen2D(void) {
    memset(framebuffer, 0, sizeof(framebuffer));
}

void glBegin2D(void) {}
void glEnd2D(void) {}

void glPutPixel(int x, int y, int color) {
    if ((unsigned)x < EASYGL2D_WIDTH && (unsigned)y < EASYGL2D_HEIGHT)
        framebuffer[y * EASYGL2D_WIDTH + x] = rgba15(color);
}

void glLine(int x1, int y1, int x2, int y2, int color) {
    const int dx = abs(x2 - x1);
    const int sx = x1 < x2 ? 1 : -1;
    const int dy = -abs(y2 - y1);
    const int sy = y1 < y2 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        glPutPixel(x1, y1, color);
        if (x1 == x2 && y1 == y2)
            break;
        const int twice = error * 2;
        if (twice >= dy) {
            error += dy;
            x1 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y1 += sy;
        }
    }
}

void glBox(int x1, int y1, int x2, int y2, int color) {
    glLine(x1, y1, x2, y1, color);
    glLine(x2, y1, x2, y2, color);
    glLine(x2, y2, x1, y2, color);
    glLine(x1, y2, x1, y1, color);
}

void glBoxFilled(int x1, int y1, int x2, int y2, int color) {
    if (x1 > x2) { const int swap = x1; x1 = x2; x2 = swap; }
    if (y1 > y2) { const int swap = y1; y1 = y2; y2 = swap; }
    if (x2 < 0 || y2 < 0 || x1 >= EASYGL2D_WIDTH || y1 >= EASYGL2D_HEIGHT)
        return;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= EASYGL2D_WIDTH) x2 = EASYGL2D_WIDTH - 1;
    if (y2 >= EASYGL2D_HEIGHT) y2 = EASYGL2D_HEIGHT - 1;
    const uint32_t rgba = rgba15(color);
    for (int y = y1; y <= y2; ++y)
        for (int x = x1; x <= x2; ++x)
            framebuffer[y * EASYGL2D_WIDTH + x] = rgba;
}

void glBoxFilledGradient(int x1, int y1, int x2, int y2,
                         int color1, int color2, int color3, int color4) {
    (void)color2;
    (void)color3;
    (void)color4;
    glBoxFilled(x1, y1, x2, y2, color1);
}

void glTriangle(int x1, int y1, int x2, int y2, int x3, int y3, int color) {
    glLine(x1, y1, x2, y2, color);
    glLine(x2, y2, x3, y3, color);
    glLine(x3, y3, x1, y1, color);
}

static int edge(int ax, int ay, int bx, int by, int px, int py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

void glTriangleFilled(int x1, int y1, int x2, int y2, int x3, int y3, int color) {
    int min_x = x1 < x2 ? (x1 < x3 ? x1 : x3) : (x2 < x3 ? x2 : x3);
    int max_x = x1 > x2 ? (x1 > x3 ? x1 : x3) : (x2 > x3 ? x2 : x3);
    int min_y = y1 < y2 ? (y1 < y3 ? y1 : y3) : (y2 < y3 ? y2 : y3);
    int max_y = y1 > y2 ? (y1 > y3 ? y1 : y3) : (y2 > y3 ? y2 : y3);
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const int a = edge(x1, y1, x2, y2, x, y);
            const int b = edge(x2, y2, x3, y3, x, y);
            const int c = edge(x3, y3, x1, y1, x, y);
            if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0))
                glPutPixel(x, y, color);
        }
    }
}
