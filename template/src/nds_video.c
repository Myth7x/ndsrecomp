#include "nds_video.h"

#include "easygl2d.h"
#include "nds_gpu.h"

#include <stdint.h>
#include <string.h>

#define IO_BASE UINT32_C(0x04000000)
#define PALETTE_BASE UINT32_C(0x05000000)
#define VIDEO_VRAM_BASE UINT32_C(0x06000000)
#define VIDEO_VRAM_END UINT32_C(0x068a0000)
#define VIDEO_VRAM_PAGE_SHIFT 12u
#define VIDEO_VRAM_PAGE_SIZE (UINT32_C(1) << VIDEO_VRAM_PAGE_SHIFT)
#define VIDEO_VRAM_PAGE_COUNT \
    ((VIDEO_VRAM_END - VIDEO_VRAM_BASE) / VIDEO_VRAM_PAGE_SIZE)

static uint8_t video_layer_map[2][256u * 192u];
static uint8_t video_obj_window[2][256u * 192u];
static const uint8_t *video_vram_pages[VIDEO_VRAM_PAGE_COUNT][9];
static uint8_t video_vram_page_counts[VIDEO_VRAM_PAGE_COUNT];
static uint8_t video_vram_controls[9];
static bool video_vram_cache_valid;

static void video_cache_vram(const NdsCpu *cpu) {
    static const unsigned control_offsets[9] = {
        0x240u, 0x241u, 0x242u, 0x243u, 0x244u,
        0x245u, 0x246u, 0x248u, 0x249u
    };
    uint8_t controls[9];
    for (unsigned bank = 0; bank < 9u; ++bank)
        controls[bank] = cpu->io[control_offsets[bank]];
    if (video_vram_cache_valid &&
        memcmp(video_vram_controls, controls, sizeof(controls)) == 0)
        return;
    memcpy(video_vram_controls, controls, sizeof(controls));
    video_vram_cache_valid = true;
    for (unsigned page = 0; page < VIDEO_VRAM_PAGE_COUNT; ++page) {
        const uint32_t address = VIDEO_VRAM_BASE +
                                 page * VIDEO_VRAM_PAGE_SIZE;
        unsigned count = 0u;
        for (unsigned bank = 0; bank < 9u; ++bank) {
            const uint8_t *pointer =
                nds_vram_bank_pointer(cpu, bank, address);
            if (pointer != NULL)
                video_vram_pages[page][count++] = pointer;
        }
        video_vram_page_counts[page] = (uint8_t)count;
    }
}

static uint8_t video_read8(const NdsCpu *cpu, uint32_t address) {
    if (address >= VIDEO_VRAM_BASE && address < VIDEO_VRAM_END) {
        const uint32_t relative = address - VIDEO_VRAM_BASE;
        const unsigned page = relative >> VIDEO_VRAM_PAGE_SHIFT;
        const unsigned offset = relative & (VIDEO_VRAM_PAGE_SIZE - 1u);
        const unsigned count = video_vram_page_counts[page];
        if (count == 0u)
            return 0u;
        if (count == 1u)
            return video_vram_pages[page][0][offset];
        uint8_t value = 0u;
        for (unsigned index = 0; index < count; ++index)
            value |= video_vram_pages[page][index][offset];
        return value;
    }
    return nds_read8(cpu, address);
}

static uint16_t video_read16(const NdsCpu *cpu, uint32_t address) {
    if (address >= VIDEO_VRAM_BASE && address + 1u < VIDEO_VRAM_END) {
        const uint32_t relative = address - VIDEO_VRAM_BASE;
        const unsigned page = relative >> VIDEO_VRAM_PAGE_SHIFT;
        const unsigned offset = relative & (VIDEO_VRAM_PAGE_SIZE - 1u);
        if (offset + 1u < VIDEO_VRAM_PAGE_SIZE &&
            video_vram_page_counts[page] == 1u) {
            const uint8_t *pointer = video_vram_pages[page][0] + offset;
            return (uint16_t)(pointer[0] | ((uint16_t)pointer[1] << 8));
        }
        return (uint16_t)(video_read8(cpu, address) |
                          ((uint16_t)video_read8(cpu, address + 1u) << 8));
    }
    return nds_read16(cpu, address);
}

static uint16_t io16(const NdsCpu *cpu, unsigned offset) {
    return (uint16_t)(cpu->io[offset] | ((uint16_t)cpu->io[offset + 1u] << 8));
}

static uint32_t io32(const NdsCpu *cpu, unsigned offset) {
    return io16(cpu, offset) | ((uint32_t)io16(cpu, offset + 2u) << 16);
}

static unsigned video_window_engine;
static unsigned video_window_layer;
static int video_window_screen_y;

static bool window_contains(uint16_t horizontal, uint16_t vertical,
                            unsigned x, unsigned y) {
    const unsigned left = horizontal & 0xffu;
    const unsigned right = horizontal >> 8;
    const unsigned top = vertical & 0xffu;
    const unsigned bottom = vertical >> 8;
    const bool in_x = left <= right ? x >= left && x < right :
                                      x >= left || x < right;
    const bool in_y = top <= bottom ? y >= top && y < bottom :
                                      y >= top || y < bottom;
    return in_x && in_y;
}

static bool video_window_allows(const NdsCpu *cpu, int x, int y) {
    const unsigned registers = video_window_engine == 0u ? 0u : 0x1000u;
    const uint32_t display = io32(cpu, registers);
    if ((display & (7u << 13)) == 0u)
        return true;
    const unsigned local_y = (unsigned)(y - video_window_screen_y);
    uint8_t mask;
    if ((display & (1u << 13)) != 0u &&
        window_contains(io16(cpu, registers + 0x40u),
                        io16(cpu, registers + 0x44u), (unsigned)x, local_y))
        mask = (uint8_t)(io16(cpu, registers + 0x48u) & 0x3fu);
    else if ((display & (1u << 14)) != 0u &&
             window_contains(io16(cpu, registers + 0x42u),
                             io16(cpu, registers + 0x46u),
                             (unsigned)x, local_y))
        mask = (uint8_t)(io16(cpu, registers + 0x48u) >> 8);
    else if ((display & (1u << 15)) != 0u &&
             video_obj_window[video_window_engine][local_y * 256u +
                                                   (unsigned)x] != 0u)
        mask = (uint8_t)(io16(cpu, registers + 0x4au) >> 8);
    else
        mask = (uint8_t)(io16(cpu, registers + 0x4au) & 0x3fu);
    return (mask & (1u << video_window_layer)) != 0u;
}

static void video_put_pixel_ex(const NdsCpu *cpu, int x, int y, int color,
                               unsigned sprite_alpha) {
    if (!video_window_allows(cpu, x, y))
        return;
    const unsigned screen = (unsigned)y / 192u;
    if (screen >= 2u || (unsigned)x >= 256u)
        return;
    const unsigned offset = ((unsigned)y % 192u) * 256u + (unsigned)x;
    const unsigned registers = video_window_engine == 0u ? 0u : 0x1000u;
    const uint16_t blend = io16(cpu, registers + 0x50u);
    const unsigned effect = (blend >> 6) & 3u;
    const unsigned source_bit = 1u << video_window_layer;
    const unsigned destination_bit = 1u << video_layer_map[screen][offset];
    const uint16_t source = (uint16_t)color;
    uint16_t result = source;
    const bool destination_selected =
        (blend & (destination_bit << 8)) != 0u;
    if (sprite_alpha != 0u && destination_selected) {
        const unsigned eva = sprite_alpha > 16u ? 16u : sprite_alpha;
        const unsigned evb = 16u - eva;
        const uint32_t pixel = easygl2d_framebuffer()[y * 256u + (unsigned)x];
        const uint16_t destination = (uint16_t)
            ((pixel >> 24) * 31u / 255u |
             (((pixel >> 16) & 255u) * 31u / 255u << 5) |
             (((pixel >> 8) & 255u) * 31u / 255u << 10));
        const unsigned red = (((source & 31u) * eva) +
                              ((destination & 31u) * evb)) / 16u;
        const unsigned green = ((((source >> 5) & 31u) * eva) +
                                (((destination >> 5) & 31u) * evb)) / 16u;
        const unsigned blue = ((((source >> 10) & 31u) * eva) +
                               (((destination >> 10) & 31u) * evb)) / 16u;
        result = (uint16_t)((red > 31u ? 31u : red) |
                            ((green > 31u ? 31u : green) << 5) |
                            ((blue > 31u ? 31u : blue) << 10));
    } else if (effect == 1u && (blend & source_bit) != 0u &&
               destination_selected) {
        unsigned eva = io16(cpu, registers + 0x52u) & 31u;
        unsigned evb = io16(cpu, registers + 0x52u) >> 8;
        if (eva > 16u) eva = 16u;
        if (evb > 16u) evb = 16u;
        const uint32_t pixel = easygl2d_framebuffer()[y * 256u + (unsigned)x];
        const uint16_t destination = (uint16_t)
            ((pixel >> 24) * 31u / 255u |
             (((pixel >> 16) & 255u) * 31u / 255u << 5) |
             (((pixel >> 8) & 255u) * 31u / 255u << 10));
        const unsigned red = (((source & 31u) * eva) +
                              ((destination & 31u) * evb)) / 16u;
        const unsigned green = ((((source >> 5) & 31u) * eva) +
                                (((destination >> 5) & 31u) * evb)) / 16u;
        const unsigned blue = ((((source >> 10) & 31u) * eva) +
                               (((destination >> 10) & 31u) * evb)) / 16u;
        result = (uint16_t)((red > 31u ? 31u : red) |
                            ((green > 31u ? 31u : green) << 5) |
                            ((blue > 31u ? 31u : blue) << 10));
    } else if ((effect == 2u || effect == 3u) &&
               (blend & source_bit) != 0u) {
        unsigned amount = io16(cpu, registers + 0x54u) & 31u;
        if (amount > 16u) amount = 16u;
        const unsigned red = source & 31u;
        const unsigned green = (source >> 5) & 31u;
        const unsigned blue = (source >> 10) & 31u;
        const unsigned adjusted_red = effect == 2u ?
            red + (31u - red) * amount / 16u : red - red * amount / 16u;
        const unsigned adjusted_green = effect == 2u ?
            green + (31u - green) * amount / 16u : green - green * amount / 16u;
        const unsigned adjusted_blue = effect == 2u ?
            blue + (31u - blue) * amount / 16u : blue - blue * amount / 16u;
        result = (uint16_t)(adjusted_red | (adjusted_green << 5) |
                            (adjusted_blue << 10));
    }
    glPutPixel(x, y, result);
    video_layer_map[screen][offset] = (uint8_t)video_window_layer;
}

static void video_put_pixel(const NdsCpu *cpu, int x, int y, int color) {
    video_put_pixel_ex(cpu, x, y, color, 0u);
}

static void video_clear_layer_map(int screen_y) {
    memset(video_layer_map[screen_y / 192], 5, 256u * 192u);
    memset(video_obj_window[screen_y / 192], 0, 256u * 192u);
}

static void video_mark_3d(int screen_y) {
    const unsigned screen = (unsigned)screen_y / 192u;
    const uint32_t *framebuffer = easygl2d_framebuffer();
    for (unsigned y = 0; y < 192u; ++y)
        for (unsigned x = 0; x < 256u; ++x) {
            const unsigned offset = y * 256u + x;
            /* Transparent 3D pixels leave the already-rendered lower-priority
               2D layer in place. */
            if ((framebuffer[(screen_y + (int)y) * 256u + x] & 0xffu) != 0u)
                video_layer_map[screen][offset] = 0u;
        }
}

static uint32_t bg_extended_palette_address(unsigned engine,
                                            unsigned background,
                                            uint16_t control,
                                            unsigned palette,
                                            unsigned index) {
    unsigned slot = background;
    if (background == 0u && (control & (1u << 13)) != 0u)
        slot = 2u;
    else if (background == 1u && (control & (1u << 13)) != 0u)
        slot = 3u;
    return UINT32_C(0x06880000) + engine * 0x10000u + slot * 0x2000u +
           palette * 0x200u + index * 2u;
}

static void render_text_background(const NdsCpu *cpu, unsigned engine,
                                   unsigned background, int screen_y) {
    const unsigned registers = engine == 0u ? 0u : 0x1000u;
    const uint32_t vram_base = engine == 0u ? UINT32_C(0x06000000)
                                            : UINT32_C(0x06200000);
    const uint32_t palette_base = PALETTE_BASE + engine * 0x400u;
    const uint32_t display = io32(cpu, registers);
    const uint16_t control = io16(cpu, registers + 8u + background * 2u);
    const bool extended_palette = (display & (1u << 30)) != 0u;
    const unsigned character_base = ((display >> 24) & 7u) * 0x10000u +
                                    ((control >> 2) & 15u) * 0x4000u;
    const unsigned screen_base = ((display >> 27) & 7u) * 0x10000u +
                                 ((control >> 8) & 31u) * 0x800u;
    const bool color_256 = (control & 0x80u) != 0;
    const unsigned size = control >> 14;
    const unsigned width = size & 1u ? 512u : 256u;
    const unsigned height = size & 2u ? 512u : 256u;
    const unsigned scroll_x = io16(cpu, registers + 0x10u + background * 4u) & 0x1ffu;
    const unsigned scroll_y = io16(cpu, registers + 0x12u + background * 4u) & 0x1ffu;
    const uint16_t mosaic = io16(cpu, registers + 0x4cu);
    const unsigned mosaic_width = 1u + (mosaic & 15u);
    const unsigned mosaic_height = 1u + ((mosaic >> 4) & 15u);
    const bool use_mosaic = (control & (1u << 6)) != 0u;

    for (unsigned y = 0; y < 192u; ++y) {
        const unsigned sample_y = use_mosaic ? y - y % mosaic_height : y;
        const unsigned source_y = (sample_y + scroll_y) & (height - 1u);
        const unsigned tile_y = source_y >> 3;
        for (unsigned x = 0; x < 256u; ++x) {
            const unsigned sample_x = use_mosaic ? x - x % mosaic_width : x;
            const unsigned source_x = (sample_x + scroll_x) & (width - 1u);
            const unsigned tile_x = source_x >> 3;
            unsigned screen_block = (tile_x >> 5);
            if (height == 512u)
                screen_block += (tile_y >> 5) * (width >> 8);
            const unsigned map_index = (tile_y & 31u) * 32u + (tile_x & 31u);
            const uint16_t entry = video_read16(cpu, vram_base + screen_base +
                                                   screen_block * 0x800u + map_index * 2u);
            unsigned pixel_x = source_x & 7u;
            unsigned pixel_y = source_y & 7u;
            if (entry & 0x0400u) pixel_x = 7u - pixel_x;
            if (entry & 0x0800u) pixel_y = 7u - pixel_y;
            const unsigned tile = entry & 0x03ffu;
            unsigned palette_index;
            uint32_t color_address;
            if (color_256) {
                palette_index = video_read8(cpu, vram_base + character_base +
                                               tile * 64u + pixel_y * 8u + pixel_x);
                color_address = extended_palette ?
                    bg_extended_palette_address(engine, background, control,
                                                (entry >> 12) & 15u,
                                                palette_index) :
                    palette_base + palette_index * 2u;
            } else {
                const uint8_t packed = video_read8(cpu, vram_base + character_base +
                                                      tile * 32u + pixel_y * 4u + pixel_x / 2u);
                palette_index = pixel_x & 1u ? packed >> 4 : packed & 15u;
                color_address = palette_base + ((entry >> 12) & 15u) * 32u +
                                palette_index * 2u;
            }
            if (palette_index != 0u)
                video_put_pixel(cpu, (int)x, screen_y + (int)y,
                                video_read16(cpu, color_address));
        }
    }
}

static void render_affine_background(const NdsCpu *cpu, unsigned engine,
                                     unsigned background, unsigned mode,
                                     int screen_y) {
    const unsigned registers = engine == 0u ? 0u : 0x1000u;
    const uint32_t vram_base = engine == 0u ? UINT32_C(0x06000000)
                                            : UINT32_C(0x06200000);
    const uint32_t palette_base = PALETTE_BASE + engine * 0x400u;
    const uint32_t display = io32(cpu, registers);
    const uint16_t control = io16(cpu, registers + 8u + background * 2u);
    const unsigned size_code = (control >> 14) & 3u;
    const bool large_bitmap = mode == 6u && background == 2u;
    const bool extended = mode >= 3u;
    const bool bitmap = extended && (control & 0x80u) != 0u;
    const bool direct_color = bitmap && (control & 4u) != 0u;
    static const unsigned affine_dimensions[4][2] = {
        {128u, 128u}, {256u, 256u}, {512u, 512u}, {1024u, 1024u}
    };
    static const unsigned bitmap_dimensions[4][2] = {
        {128u, 128u}, {256u, 256u}, {512u, 256u}, {512u, 512u}
    };
    static const unsigned large_dimensions[4][2] = {
        {512u, 1024u}, {1024u, 512u}, {512u, 256u}, {512u, 512u}
    };
    const unsigned (*dimensions)[2] = large_bitmap ? large_dimensions :
                                      (bitmap ? bitmap_dimensions : affine_dimensions);
    const unsigned width = dimensions[size_code][0];
    const unsigned height = dimensions[size_code][1];
    const unsigned display_screen_base = engine == 0u ?
        ((display >> 27) & 7u) * 0x10000u : 0u;
    const unsigned screen_base = large_bitmap ? 0u : (bitmap ?
        ((control >> 8) & 31u) * 0x4000u :
        display_screen_base + ((control >> 8) & 31u) * 0x800u);
    const unsigned character_base = (engine == 0u ?
        ((display >> 24) & 7u) * 0x10000u : 0u) +
        ((control >> 2) & 15u) * 0x4000u;
    const bool map_16 = extended && !bitmap && !large_bitmap;
    const bool extended_palette = (display & (1u << 30)) != 0u;
    const unsigned affine = background - 2u;
    const unsigned affine_registers = registers + 0x20u + affine * 16u;
    const int pa = (int16_t)io16(cpu, affine_registers + 0u);
    const int pb = (int16_t)io16(cpu, affine_registers + 2u);
    const int pc = (int16_t)io16(cpu, affine_registers + 4u);
    const int pd = (int16_t)io16(cpu, affine_registers + 6u);
    const int32_t ref_x = (int32_t)io32(cpu, affine_registers + 8u);
    const int32_t ref_y = (int32_t)io32(cpu, affine_registers + 12u);
    const uint16_t mosaic = io16(cpu, registers + 0x4cu);
    const unsigned mosaic_width = 1u + (mosaic & 15u);
    const unsigned mosaic_height = 1u + ((mosaic >> 4) & 15u);
    const bool use_mosaic = (control & (1u << 6)) != 0u;

    for (unsigned y = 0; y < 192u; ++y) {
        const unsigned sample_y = use_mosaic ? y - y % mosaic_height : y;
        const int32_t source_x0 = ref_x + pb * (int)sample_y;
        const int32_t source_y0 = ref_y + pd * (int)sample_y;
        for (unsigned x = 0; x < 256u; ++x) {
            const unsigned sample_x = use_mosaic ? x - x % mosaic_width : x;
            const int32_t source_x = source_x0 + pa * (int)sample_x;
            const int32_t source_y = source_y0 + pc * (int)sample_x;
            const int sx = source_x >> 8;
            const int sy = source_y >> 8;
            if (bitmap || large_bitmap) {
                const bool wrap = (control & (1u << 13)) != 0u;
                if (!wrap && ((unsigned)sx >= width || (unsigned)sy >= height))
                    continue;
                const unsigned bitmap_x = wrap ? (unsigned)sx & (width - 1u) :
                                               (unsigned)sx;
                const unsigned bitmap_y = wrap ? (unsigned)sy & (height - 1u) :
                                               (unsigned)sy;
                const uint32_t address = vram_base + screen_base +
                    bitmap_y * width * (direct_color ? 2u : 1u) +
                    bitmap_x * (direct_color ? 2u : 1u);
                if (!direct_color) {
                    const unsigned index = video_read8(cpu, address);
                    if (index != 0u) {
                        const uint32_t color_address = extended_palette && map_16 ?
                            bg_extended_palette_address(engine, background,
                                                        control, 0u, index) :
                            palette_base + index * 2u;
                        video_put_pixel(cpu, (int)x, screen_y + (int)y,
                                        video_read16(cpu, color_address));
                    }
                } else {
                    const uint16_t color = video_read16(cpu, address);
                    if (color & 0x8000u)
                        video_put_pixel(cpu, (int)x, screen_y + (int)y, color);
                }
                continue;
            }
            const bool wrap = (control & (1u << 13)) != 0u;
            if (!wrap && ((unsigned)sx >= width || (unsigned)sy >= height))
                continue;
            const unsigned wrapped_x = (unsigned)sx & (width - 1u);
            const unsigned wrapped_y = (unsigned)sy & (height - 1u);
            const unsigned tile_x = wrapped_x >> 3;
            const unsigned tile_y = wrapped_y >> 3;
            unsigned pixel_x = wrapped_x & 7u;
            unsigned pixel_y = wrapped_y & 7u;
            const unsigned map_width = width >> 3;
            uint16_t entry = 0u;
            if (map_16) {
                entry = video_read16(cpu, vram_base + screen_base +
                    (tile_y * map_width + tile_x) * 2u);
                if ((entry & 0x0400u) != 0u) pixel_x = 7u - pixel_x;
                if ((entry & 0x0800u) != 0u) pixel_y = 7u - pixel_y;
            } else {
                entry = video_read8(cpu, vram_base + screen_base +
                    tile_y * map_width + tile_x);
            }
            const unsigned tile = entry & 0x03ffu;
            const unsigned pixel = video_read8(cpu, vram_base + character_base +
                tile * 64u + pixel_y * 8u + pixel_x);
            if (pixel != 0u)
                           video_put_pixel(cpu, (int)x, screen_y + (int)y,
                           video_read16(cpu, map_16 && extended_palette ?
                               bg_extended_palette_address(engine, background,
                                                           control,
                                                           (entry >> 12) & 15u,
                                                           pixel) :
                               palette_base + pixel * 2u));
        }
    }
}

static void obj_dimensions(unsigned shape, unsigned size,
                           unsigned *width, unsigned *height) {
    static const uint8_t dimensions[3][4][2] = {
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}}
    };
    if (shape >= 3u) shape = 0u;
    *width = dimensions[shape][size & 3u][0];
    *height = dimensions[shape][size & 3u][1];
}

static uint32_t obj_tile_address(uint32_t display, unsigned tile,
                                 bool color_256, unsigned tile_x,
                                 unsigned tile_y, unsigned width_tiles) {
    const unsigned tile_bytes = color_256 ? 64u : 32u;
    if ((display & (1u << 4)) != 0u) {
        const unsigned boundary = 0x20u << ((display >> 20) & 3u);
        return tile * boundary +
               (tile_y * width_tiles + tile_x) * tile_bytes;
    }
    const unsigned name = (color_256 ? tile & ~1u : tile) + tile_y * 32u +
                          tile_x * (color_256 ? 2u : 1u);
    return name * 32u;
}

static bool render_obj(const NdsCpu *cpu, unsigned engine, unsigned priority,
                       int screen_y) {
    const unsigned registers = engine == 0u ? 0u : 0x1000u;
    const uint32_t display = io32(cpu, registers);
    if ((display & (1u << 12)) == 0u)
        return false;
    const uint32_t vram_base = engine == 0u ? UINT32_C(0x06400000)
                                            : UINT32_C(0x06600000);
    const uint32_t palette_base = PALETTE_BASE + engine * 0x400u + 0x200u;
    const bool extended_palette = (display & (1u << 31)) != 0u;
    bool rendered = false;

    /* OAM entries are eight bytes apart; the intervening halfword stores
       affine parameters for the 32 rotation/scaling sets.  Iterate backwards
       so lower OAM indices, which have higher tie priority, land on top. */
    for (int object = 127; object >= 0; --object) {
        const uint32_t oam = UINT32_C(0x07000000) + engine * 0x400u +
                             (unsigned)object * 8u;
        const uint16_t attr0 = video_read16(cpu, oam + 0u);
        const uint16_t attr1 = video_read16(cpu, oam + 2u);
        const uint16_t attr2 = video_read16(cpu, oam + 4u);
        const bool affine = (attr0 & (1u << 8)) != 0u;
        const unsigned sprite_mode = (attr0 >> 10) & 3u;
        if (!affine && (attr0 & (1u << 9)) != 0u)
            continue;
        if (priority == 4u) {
            if (sprite_mode != 2u)
                continue;
        } else {
            if (sprite_mode == 2u || ((attr2 >> 10) & 3u) != priority)
                continue;
        }
        unsigned width, height;
        obj_dimensions((attr0 >> 14) & 3u, (attr1 >> 14) & 3u,
                       &width, &height);
        const unsigned draw_width = affine && (attr0 & (1u << 9)) ?
                                    width * 2u : width;
        const unsigned draw_height = affine && (attr0 & (1u << 9)) ?
                                     height * 2u : height;
        int origin_x = attr1 & 0x1ffu;
        int origin_y = attr0 & 0xffu;
        if (origin_x >= 256) origin_x -= 512;
        if (origin_y >= 192) origin_y -= 256;
        if (affine) {
            /* Affine OAM coordinates name the sprite centre.  The previous
               path only centred double-size sprites, shifting every normal
               rotated/scaled OBJ by half its dimensions. */
            origin_x -= (int)draw_width / 2;
            origin_y -= (int)draw_height / 2;
        }
        const unsigned rotation = (attr1 >> 9) & 31u;
        const int16_t pa = affine ? (int16_t)video_read16(cpu,
            UINT32_C(0x07000000) + engine * 0x400u + 6u + rotation * 32u) : 0x100;
        const int16_t pb = affine ? (int16_t)video_read16(cpu,
            UINT32_C(0x07000000) + engine * 0x400u + 14u + rotation * 32u) : 0;
        const int16_t pc = affine ? (int16_t)video_read16(cpu,
            UINT32_C(0x07000000) + engine * 0x400u + 22u + rotation * 32u) : 0;
        const int16_t pd = affine ? (int16_t)video_read16(cpu,
            UINT32_C(0x07000000) + engine * 0x400u + 30u + rotation * 32u) : 0x100;
        const bool color_256 = (attr0 & (1u << 13)) != 0u;
        const unsigned tile = attr2 & 0x3ffu;
        const unsigned palette = (attr2 >> 12) & 15u;
        const unsigned source_width_tiles = width / 8u;
        for (unsigned y = 0; y < draw_height; ++y) {
            const int output_y = origin_y + (int)y;
            if (output_y < 0 || output_y >= 192)
                continue;
            unsigned source_y = y;
            if (affine) {
                const int dy = (int)y - (int)draw_height / 2;
                source_y = (unsigned)(((pb * dy) >> 8) + (int)height / 2);
            } else if ((attr1 & (1u << 13)) != 0u)
                source_y = height - 1u - (source_y % height);
            for (unsigned x = 0; x < draw_width; ++x) {
                const int output_x = origin_x + (int)x;
                if (output_x < 0 || output_x >= 256)
                    continue;
                unsigned source_x = x;
                if (affine) {
                    const int dx = (int)x - (int)draw_width / 2;
                    const int dy = (int)y - (int)draw_height / 2;
                    source_x = (unsigned)(((pa * dx + pb * dy) >> 8) +
                                          (int)width / 2);
                    source_y = (unsigned)(((pc * dx + pd * dy) >> 8) +
                                          (int)height / 2);
                } else if ((attr1 & (1u << 12)) != 0u)
                    source_x = width - 1u - (source_x % width);
                if (source_x >= width || source_y >= height)
                    continue;
                if (sprite_mode == 3u) {
                    const unsigned alpha = (attr2 >> 12) + 1u;
                    if (display & (1u << 6)) {
                        if (display & (1u << 5))
                            continue;
                        const uint32_t base = (tile <<
                            (7u + ((display >> 22) & 1u))) +
                            source_y * width * 2u;
                        const uint16_t color = video_read16(cpu, vram_base +
                            base + source_x * 2u);
                        if ((color & 0x8000u) != 0u)
                            video_put_pixel_ex(cpu, output_x,
                                               screen_y + output_y, color,
                                               alpha);
                    } else {
                        const uint32_t base = (display & (1u << 5)) != 0u ?
                            ((tile & 0x1fu) << 4) +
                            ((tile & 0x3e0u) << 7) + source_y * 512u :
                            ((tile & 0x0fu) << 4) +
                            ((tile & 0x3f0u) << 7) + source_y * 256u;
                        const uint16_t color = video_read16(cpu, vram_base +
                            base + source_x * 2u);
                        if ((color & 0x8000u) != 0u)
                            video_put_pixel_ex(cpu, output_x,
                                               screen_y + output_y, color,
                                               alpha);
                    }
                    rendered = true;
                    continue;
                }
                const unsigned tile_x = source_x >> 3;
                const unsigned tile_y = source_y >> 3;
                const unsigned pixel_x = source_x & 7u;
                const unsigned pixel_y = source_y & 7u;
                const uint32_t address = vram_base +
                    obj_tile_address(display, tile, color_256, tile_x, tile_y,
                                     source_width_tiles) +
                    pixel_y * (color_256 ? 8u : 4u) +
                    (color_256 ? pixel_x : pixel_x / 2u);
                unsigned index;
                if (color_256) {
                    index = video_read8(cpu, address);
                } else {
                    const uint8_t packed = video_read8(cpu, address);
                    index = (pixel_x & 1u) ? packed >> 4 : packed & 15u;
                    index += palette * 16u;
                }
                if (index == 0u)
                    continue;
                if (sprite_mode == 2u) {
                    const unsigned screen = (unsigned)(screen_y + output_y) / 192u;
                    const unsigned offset = ((unsigned)output_y % 192u) * 256u +
                                            (unsigned)output_x;
                    if (screen < 2u)
                        video_obj_window[screen][offset] = 1u;
                    rendered = true;
                    continue;
                }
                const uint32_t color_address = extended_palette && color_256 ?
                    (engine == 0u ? UINT32_C(0x06890000) :
                                    UINT32_C(0x06898000)) +
                    palette * 0x200u + index * 2u :
                    palette_base + index * 2u;
                const uint16_t color = video_read16(cpu, color_address);
                video_put_pixel(cpu, output_x, screen_y + output_y, color);
                rendered = true;
            }
        }
    }
    return rendered;
}

static bool background_is_text(unsigned mode, unsigned background) {
    static const uint8_t text_masks[8] = {
        0x0fu, 0x07u, 0x03u, 0x07u, 0x03u, 0x03u, 0x00u, 0x00u
    };
    return (text_masks[mode & 7u] & (1u << background)) != 0u;
}

static bool background_is_affine(unsigned mode, unsigned background) {
    return (mode == 1u && background == 3u) ||
           (mode == 2u && (background == 2u || background == 3u)) ||
           (mode == 3u && background == 3u) ||
           (mode == 4u && (background == 2u || background == 3u)) ||
           (mode == 5u && background >= 2u);
}

static bool background_is_large_bitmap(unsigned mode, unsigned background) {
    return mode == 6u && background == 2u;
}

static void render_engine(const NdsCpu *cpu, unsigned engine, int screen_y,
                          bool clear, int highest_priority,
                          int lowest_priority) {
    const unsigned registers = engine == 0u ? 0u : 0x1000u;
    const uint16_t power = io16(cpu, 0x304u);
    video_window_engine = engine;
    video_window_screen_y = screen_y;
    const uint32_t display = io32(cpu, registers);
    const unsigned display_mode = (display >> 16) & 3u;
    const bool engine_enabled = (power & (engine == 0u ? (1u << 1) :
                                          (1u << 9))) != 0u;
    if (clear) {
        const uint16_t backdrop = video_read16(cpu, PALETTE_BASE + engine * 0x400u);
        const int base_color = display_mode == 0u ? 0x7fffu : backdrop;
        glBoxFilled(0, screen_y, 255, screen_y + 191, base_color);
        video_clear_layer_map(screen_y);
    }
    /* POWCNT1 can disable either 2D engine independently.  Disabled A is
       black and disabled B is the DS's white/gray blank level; no BG/OBJ
       data from the previous frame may leak through. */
    if (!engine_enabled) {
        glBoxFilled(0, screen_y, 255, screen_y + 191,
                    engine == 0u ? 0u : 0x7fffu);
        video_clear_layer_map(screen_y);
        return;
    }
    if ((display & (1u << 7)) != 0u) {
        glBoxFilled(0, screen_y, 255, screen_y + 191, 0x7fffu);
        return;
    }
    if (display_mode == 2u && engine == 0u) {
        /* VRAM display reads the selected LCDC bank, not the BG mapping at
           0x06000000. */
        const uint32_t base = UINT32_C(0x06800000) +
                              ((display >> 18) & 3u) * 0x20000u;
        for (unsigned y = 0; y < 192u; ++y)
            for (unsigned x = 0; x < 256u; ++x)
                glPutPixel((int)x, screen_y + (int)y,
                           video_read16(cpu, base + (y * 256u + x) * 2u));
        easygl2d_apply_brightness(screen_y,
                                  io16(cpu, registers + 0x6cu));
        return;
    }
    if (display_mode == 3u && engine == 0u) {
        for (unsigned y = 0; y < 192u; ++y)
            for (unsigned x = 0; x < 256u; ++x)
                glPutPixel((int)x, screen_y + (int)y,
                           cpu->display_fifo == NULL ? 0u :
                           cpu->display_fifo[y * 256u + x]);
        easygl2d_apply_brightness(screen_y,
                                  io16(cpu, registers + 0x6cu));
        return;
    }
    if (display_mode != 1u) {
        if (!clear || engine == 1u)
            easygl2d_apply_brightness(screen_y,
                                      io16(cpu, registers + 0x6cu));
        return;
    }

    const unsigned mode = display & 7u;
    if ((display & (1u << 15)) != 0u) {
        video_window_layer = 4u;
        /* Generate OBJ-window coverage before priority compositing. */
        render_obj(cpu, engine, 4u, screen_y);
    }
    for (int priority = highest_priority; priority >= lowest_priority; --priority) {
        for (int background = 3; background >= 0; --background) {
            if (!(display & (UINT32_C(1) << (8u + (unsigned)background))))
                continue;
            if (engine == 0u && background == 0u && (display & 8u) != 0u)
                continue;
            if ((io16(cpu, registers + 8u + (unsigned)background * 2u) & 3u) !=
                (unsigned)priority)
                continue;
            video_window_layer = (unsigned)background;
            if (background_is_text(mode, (unsigned)background))
                render_text_background(cpu, engine, (unsigned)background, screen_y);
            else if (background_is_affine(mode, (unsigned)background))
                render_affine_background(cpu, engine, (unsigned)background, mode, screen_y);
            else if (background_is_large_bitmap(mode, (unsigned)background))
                render_affine_background(cpu, engine, (unsigned)background, mode, screen_y);
        }
        video_window_layer = 4u;
        render_obj(cpu, engine, (unsigned)priority, screen_y);
    }
    if (!clear || engine == 1u)
        easygl2d_apply_brightness(screen_y,
                                  io16(cpu, registers + 0x6cu));
}

void nds_video_render(const NdsCpu *cpu) {
    video_cache_vram(cpu);
    /* POWCNT1 bit 15 selects the physical destination of display A:
       set means upper LCD, clear means lower LCD. */
    const bool main_upper = (io16(cpu, 0x304u) & 0x8000u) != 0u;
    const uint16_t power = io16(cpu, 0x304u);
    const int main_y = main_upper ? 0 : 192;
    const int sub_y = main_upper ? 192 : 0;
    const uint32_t main_display = io32(cpu, 0u);
    const bool main_has_3d = (power & (1u << 3)) != 0u &&
                             (main_display & 8u) != 0u &&
                             (main_display & (1u << 7)) == 0u;
    if (main_has_3d) {
        render_engine(cpu, 0u, main_y, true, 3, 1);
        nds_gpu_render(cpu, main_y);
        video_mark_3d(main_y);
        render_engine(cpu, 0u, main_y, false, 0, 0);
    } else {
        render_engine(cpu, 0u, main_y, true, 3, 0);
    }
    render_engine(cpu, 1u, sub_y, true, 3, 0);
}
