#include "nds_video.h"

#include "easygl2d.h"

#include <stdint.h>

#define IO_BASE UINT32_C(0x04000000)
#define PALETTE_BASE UINT32_C(0x05000000)

static uint16_t io16(const NdsCpu *cpu, unsigned offset) {
    return (uint16_t)(cpu->io[offset] | ((uint16_t)cpu->io[offset + 1u] << 8));
}

static uint32_t io32(const NdsCpu *cpu, unsigned offset) {
    return io16(cpu, offset) | ((uint32_t)io16(cpu, offset + 2u) << 16);
}

static void render_text_background(const NdsCpu *cpu, unsigned engine,
                                   unsigned background, int screen_y) {
    const unsigned registers = engine == 0u ? 0u : 0x1000u;
    const uint32_t vram_base = engine == 0u ? UINT32_C(0x06000000)
                                            : UINT32_C(0x06200000);
    const uint32_t palette_base = PALETTE_BASE + engine * 0x400u;
    const uint32_t display = io32(cpu, registers);
    const uint16_t control = io16(cpu, registers + 8u + background * 2u);
    const unsigned character_base = ((display >> 24) & 7u) * 0x10000u +
                                    ((control >> 2) & 3u) * 0x4000u;
    const unsigned screen_base = ((display >> 27) & 7u) * 0x10000u +
                                 ((control >> 8) & 31u) * 0x800u;
    const bool color_256 = (control & 0x80u) != 0;
    const unsigned size = control >> 14;
    const unsigned width = size & 1u ? 512u : 256u;
    const unsigned height = size & 2u ? 512u : 256u;
    const unsigned scroll_x = io16(cpu, registers + 0x10u + background * 4u) & 0x1ffu;
    const unsigned scroll_y = io16(cpu, registers + 0x12u + background * 4u) & 0x1ffu;

    for (unsigned y = 0; y < 192u; ++y) {
        const unsigned source_y = (y + scroll_y) & (height - 1u);
        const unsigned tile_y = source_y >> 3;
        for (unsigned x = 0; x < 256u; ++x) {
            const unsigned source_x = (x + scroll_x) & (width - 1u);
            const unsigned tile_x = source_x >> 3;
            unsigned screen_block = (tile_x >> 5);
            if (height == 512u)
                screen_block += (tile_y >> 5) * (width >> 8);
            const unsigned map_index = (tile_y & 31u) * 32u + (tile_x & 31u);
            const uint16_t entry = nds_read16(cpu, vram_base + screen_base +
                                                   screen_block * 0x800u + map_index * 2u);
            unsigned pixel_x = source_x & 7u;
            unsigned pixel_y = source_y & 7u;
            if (entry & 0x0400u) pixel_x = 7u - pixel_x;
            if (entry & 0x0800u) pixel_y = 7u - pixel_y;
            const unsigned tile = entry & 0x03ffu;
            unsigned palette_index;
            uint32_t color_address;
            if (color_256) {
                palette_index = nds_read8(cpu, vram_base + character_base +
                                               tile * 64u + pixel_y * 8u + pixel_x);
                color_address = palette_base + palette_index * 2u;
            } else {
                const uint8_t packed = nds_read8(cpu, vram_base + character_base +
                                                      tile * 32u + pixel_y * 4u + pixel_x / 2u);
                palette_index = pixel_x & 1u ? packed >> 4 : packed & 15u;
                color_address = palette_base + ((entry >> 12) & 15u) * 32u +
                                palette_index * 2u;
            }
            if (palette_index != 0u)
                glPutPixel((int)x, screen_y + (int)y, nds_read16(cpu, color_address));
        }
    }
}

static bool background_is_text(unsigned mode, unsigned background) {
    static const uint8_t text_masks[8] = {
        0x0fu, 0x0bu, 0x03u, 0x00u, 0x00u, 0x03u, 0x00u, 0x00u
    };
    return (text_masks[mode & 7u] & (1u << background)) != 0u;
}

static void render_engine(const NdsCpu *cpu, unsigned engine, int screen_y) {
    const unsigned registers = engine == 0u ? 0u : 0x1000u;
    const uint32_t display = io32(cpu, registers);
    const unsigned display_mode = (display >> 16) & 3u;
    const uint16_t backdrop = nds_read16(cpu, PALETTE_BASE + engine * 0x400u);
    const int base_color = display_mode == 0u ? 0x7fffu : backdrop;
    for (int y = 0; y < 192; ++y)
        for (int x = 0; x < 256; ++x)
            glPutPixel(x, screen_y + y, base_color);
    if (display_mode != 1u)
        return;

    const unsigned mode = display & 7u;
    for (int priority = 3; priority >= 0; --priority) {
        for (int background = 3; background >= 0; --background) {
            if (!(display & (UINT32_C(1) << (8u + (unsigned)background))))
                continue;
            if ((io16(cpu, registers + 8u + (unsigned)background * 2u) & 3u) !=
                (unsigned)priority)
                continue;
            if (background_is_text(mode, (unsigned)background))
                render_text_background(cpu, engine, (unsigned)background, screen_y);
        }
    }
}

void nds_video_render(const NdsCpu *cpu) {
    const bool swap = (io16(cpu, 0x304u) & 0x8000u) != 0u;
    render_engine(cpu, 0u, swap ? 192 : 0);
    render_engine(cpu, 1u, swap ? 0 : 192);
}
