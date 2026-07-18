#include "nds_runtime.h"

#include "rom_config.h"
#include "rom_data.h"
#include "nds_gpu.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ITCM_WINDOW_END UINT32_C(0x02000000)
#define ITCM_SIZE (32u * 1024u)
#define DTCM_SIZE (16u * 1024u)
#define MAIN_BASE UINT32_C(0x02000000)
#define MAIN_SIZE (16u * 1024u * 1024u)
#define ARM7_WRAM_BASE UINT32_C(0x037f8000)
#define ARM7_WRAM_SIZE (96u * 1024u)
#define SDK_SYNC_ARM9 UINT32_C(0x02fffc24)
#define SDK_SYNC_ARM7 UINT32_C(0x02fffc26)
#define SDK_BOOT_STAGE UINT32_C(0x02fffc28)
#define SDK_PROBE_MAILBOX UINT32_C(0x02fffff0)
#define SDK_CALLBACKS_ARM9 UINT32_C(0x02ffff88)
#define SDK_CALLBACKS_ARM7 UINT32_C(0x02ffff8c)
#define IO_BASE UINT32_C(0x04000000)
#define IO_SIZE (64u * 1024u)
#define PALETTE_BASE UINT32_C(0x05000000)
#define PALETTE_SIZE (2u * 1024u)
#define VRAM_BASE UINT32_C(0x06000000)
#define VRAM_END UINT32_C(0x07000000)
#define VRAM_SIZE UINT32_C(0x000a4000)
#define OAM_BASE UINT32_C(0x07000000)
#define OAM_SIZE (2u * 1024u)
#define DISPLAY_FIFO_PIXELS (256u * 192u)
#define REG_IPCSYNC UINT32_C(0x04000180)
#define REG_IPCFIFOCNT UINT32_C(0x04000184)
#define REG_IPCFIFOSEND UINT32_C(0x04000188)
#define REG_VRAMCNT UINT32_C(0x04000240)
#define REG_1DOT_DEPTH UINT32_C(0x04000610)
#define REG_AUXSPICNT UINT32_C(0x040001a0)
#define REG_AUXSPIDATA UINT32_C(0x040001a2)
#define REG_DISPSTAT UINT32_C(0x04000004)
#define REG_VCOUNT UINT32_C(0x04000006)
#define REG_ROMCTRL UINT32_C(0x040001a4)
#define REG_CARDCMD UINT32_C(0x040001a8)
#define REG_IME UINT32_C(0x04000208)
#define REG_IE UINT32_C(0x04000210)
#define REG_IF UINT32_C(0x04000214)
#define REG_POSTFLG UINT32_C(0x04000300)
#define REG_TIMER0 UINT32_C(0x04000100)
#define REG_KEYINPUT UINT32_C(0x04000130)
#define REG_EXTKEYIN UINT32_C(0x04000136)
#define REG_SOUNDBIAS UINT32_C(0x04000504)
#define REG_SPICNT UINT32_C(0x040001c0)
#define REG_SPIDATA UINT32_C(0x040001c2)
#define REG_DIVCNT UINT32_C(0x04000280)
#define REG_DIVNUMER UINT32_C(0x04000290)
#define REG_DIVDENOM UINT32_C(0x04000298)
#define REG_DIVRESULT UINT32_C(0x040002a0)
#define REG_DIVREM UINT32_C(0x040002a8)
#define REG_SQRTCNT UINT32_C(0x040002b0)
#define REG_SQRTRESULT UINT32_C(0x040002b4)
#define REG_SQRTPARAM UINT32_C(0x040002b8)
#define DEFAULT_SAVE_SIZE (8u * 1024u)
#define NDS_FRAME_CYCLES UINT32_C(560190)
#define NDS_LINE_CYCLES UINT32_C(2130)
#define NDS_HBLANK_CYCLES UINT32_C(1536)
#define REG_IPCFIFORECV UINT32_C(0x04100000)
#define REG_CARDDATA UINT32_C(0x04100010)
#define REG_GXFIFO UINT32_C(0x04000400)
#define REG_GXSTAT UINT32_C(0x04000600)
#define REG_RAM_COUNT UINT32_C(0x04000604)
#define REG_POS_RESULT UINT32_C(0x04000620)
#define REG_VEC_RESULT UINT32_C(0x04000630)
#define REG_DISP_MMEM_FIFO UINT32_C(0x04000068)
#define REG_CLIP_MATRIX UINT32_C(0x04000640)
#define REG_VECTOR_MATRIX UINT32_C(0x04000680)

static int gx_port_command(uint32_t address) {
    static const uint8_t commands[] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
        0x29, 0x2a, 0x2b,
        0x30, 0x31, 0x32, 0x33, 0x34,
        0x40, 0x41, 0x50, 0x60, 0x70, 0x71, 0x72
    };
    static const uint16_t offsets[] = {
        0x440, 0x444, 0x448, 0x44c, 0x450, 0x454, 0x458, 0x45c,
        0x460, 0x464, 0x468, 0x46c, 0x470,
        0x480, 0x484, 0x488, 0x48c, 0x490, 0x494, 0x498, 0x49c, 0x4a0,
        0x4a4, 0x4a8, 0x4ac,
        0x4c0, 0x4c4, 0x4c8, 0x4cc, 0x4d0,
        0x500, 0x504, 0x540, 0x580, 0x5c0, 0x5c4, 0x5c8
    };
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
        if (address == UINT32_C(0x04000000) + offsets[i])
            return commands[i];
    return -1;
}
#define IRQ_IPC_RECV (UINT32_C(1) << 18)
#define IRQ_GXFIFO (UINT32_C(1) << 21)
#define IRQ_VBLANK UINT32_C(1)
#define IRQ_HBLANK (UINT32_C(1) << 1)
#define IRQ_VCOUNT (UINT32_C(1) << 2)
#define SDK_IRQ_HANDLER_ARM7 UINT32_C(0x0380fffc)
#define IRQ_IPC_SYNC (UINT32_C(1) << 16)
#define IRQ_RETURN_SENTINEL UINT32_C(0xfffffe00)
#define FLAG_N (UINT32_C(1) << 31)
#define FLAG_Z (UINT32_C(1) << 30)
#define FLAG_C (UINT32_C(1) << 29)
#define FLAG_V (UINT32_C(1) << 28)
#define FLAG_T (UINT32_C(1) << 5)

static unsigned mode_bank(uint32_t status) {
    const unsigned mode = status & 31u;
    return mode == 0x10u || mode == 0x1fu ? 0x10u : mode;
}

static bool mode_has_spsr(unsigned mode) {
    return mode == 0x11u || mode == 0x12u || mode == 0x13u ||
           mode == 0x17u || mode == 0x1bu;
}

static void replace_cpsr(NdsCpu *cpu, uint32_t status) {
    const unsigned old_bank = mode_bank(cpu->cpsr);
    const unsigned new_bank = mode_bank(status);
    if (old_bank != new_bank) {
        if (mode_has_spsr(old_bank))
            cpu->banked_spsr[old_bank] = cpu->spsr;
        cpu->banked_r13[old_bank] = cpu->r[13];
        cpu->banked_r14[old_bank] = cpu->r[14];
        cpu->r[13] = cpu->banked_r13[new_bank];
        cpu->r[14] = cpu->banked_r14[new_bank];
        cpu->spsr = mode_has_spsr(new_bank) ? cpu->banked_spsr[new_bank] : 0u;
    }
    cpu->cpsr = status;
}

static uint32_t rotate_right(uint32_t value, unsigned amount) {
    amount &= 31u;
    return amount == 0 ? value : (value >> amount) | (value << (32u - amount));
}

static int32_t sign_extend32(uint32_t value, unsigned bits) {
    const uint32_t sign = UINT32_C(1) << (bits - 1u);
    return (int32_t)((value ^ sign) - sign);
}

static void thumb_exchange(NdsCpu *cpu, uint32_t target);

static uint32_t register_value(const NdsCpu *cpu, unsigned reg, uint32_t pc) {
    return reg == 15 ? pc + 8u : cpu->r[reg];
}

static uint8_t *mapped(NdsCpu *cpu, uint32_t address) {
    if (cpu->cpu_id == 9u && address < ITCM_WINDOW_END && cpu->itcm != NULL)
        return &cpu->itcm[address & (ITCM_SIZE - 1u)];
    if (cpu->cpu_id == 9u && address >= cpu->dtcm_base &&
        address - cpu->dtcm_base < DTCM_SIZE)
        return &cpu->dtcm[address - cpu->dtcm_base];
    if (address >= MAIN_BASE && address - MAIN_BASE < MAIN_SIZE)
        return &cpu->main_ram[address - MAIN_BASE];
    if (cpu->cpu_id == 7u && address >= ARM7_WRAM_BASE &&
        address - ARM7_WRAM_BASE < ARM7_WRAM_SIZE)
        return &cpu->wram[address - ARM7_WRAM_BASE];
    if (address >= IO_BASE && address - IO_BASE < IO_SIZE)
        return &cpu->io[address - IO_BASE];
    if (address >= PALETTE_BASE && address - PALETTE_BASE < PALETTE_SIZE)
        return &cpu->palette[address - PALETTE_BASE];
    if (address >= OAM_BASE && address - OAM_BASE < OAM_SIZE)
        return &cpu->oam[address - OAM_BASE];
    return NULL;
}

static const uint8_t *mapped_const(const NdsCpu *cpu, uint32_t address) {
    return mapped((NdsCpu *)cpu, address);
}

static const unsigned vram_register_offsets[9] = {
    0x240u, 0x241u, 0x242u, 0x243u, 0x244u,
    0x245u, 0x246u, 0x248u, 0x249u
};

static const uint32_t vram_bank_offsets[9] = {
    0x00000u, 0x20000u, 0x40000u, 0x60000u, 0x80000u,
    0x90000u, 0x94000u, 0x98000u, 0xa0000u
};

static const uint32_t vram_bank_sizes[9] = {
    0x20000u, 0x20000u, 0x20000u, 0x20000u, 0x10000u,
    0x04000u, 0x04000u, 0x08000u, 0x04000u
};

static uint8_t vram_control(const NdsCpu *cpu, unsigned bank) {
    const NdsCpu *owner = cpu->cpu_id == 9u || cpu->peer == NULL ? cpu : cpu->peer;
    return owner->io[vram_register_offsets[bank]];
}

static bool vram_bank_address(const NdsCpu *cpu, unsigned bank, uint32_t address,
                              uint32_t *physical) {
    const uint8_t control = vram_control(cpu, bank);
    if (!(control & 0x80u))
        return false;
    const unsigned mode = control & 7u;
    const unsigned offset = (control >> 3) & 3u;
    uint32_t relative;

    /* LCDC mode exposes every bank at its fixed 0x06800000-based address,
     * including E-I (texture-palette memory).  The old mapper only covered
     * A-D here, so palette uploads to 0x06880000+ were discarded. */
    if (mode == 0u) {
        const uint32_t base = UINT32_C(0x06800000) +
                              vram_bank_offsets[bank];
        if (address >= base && address - base < vram_bank_sizes[bank]) {
            *physical = vram_bank_offsets[bank] + address - base;
            return true;
        }
        return false;
    }

    if (cpu->cpu_id == 7u && address >= UINT32_C(0x06000000) &&
        address < UINT32_C(0x06040000)) {
        if ((bank == 2u || bank == 3u) && mode == 2u) {
            relative = address - UINT32_C(0x06000000);
            const uint32_t base = (offset & 1u) * 0x20000u;
            if (relative >= base && relative - base < 0x20000u) {
                *physical = vram_bank_offsets[bank] + (relative & 0x1ffffu);
                return true;
            }
        }
        return false;
    }

    if (address >= UINT32_C(0x06800000) && address < UINT32_C(0x06880000)) {
        relative = address - UINT32_C(0x06800000);
        if (mode == 0u) {
            const uint32_t base = vram_bank_offsets[bank];
            const uint32_t size = vram_bank_sizes[bank];
            if (relative >= base && relative - base < size) {
                *physical = base + relative - base;
                return true;
            }
        } else if (mode == 3u && bank <= 3u) {
            const uint32_t base = offset * 0x20000u;
            if (relative >= base && relative - base < 0x20000u) {
                *physical = vram_bank_offsets[bank] + relative - base;
                return true;
            }
        }
        return false;
    }

    if (address >= UINT32_C(0x06880000) && address < UINT32_C(0x06890000)) {
        if (bank == 4u && mode == 3u) {
            *physical = vram_bank_offsets[bank] + address - UINT32_C(0x06880000);
            return true;
        }
        if (bank == 4u && mode == 4u && address - UINT32_C(0x06880000) < 0x8000u) {
            *physical = vram_bank_offsets[bank] + address - UINT32_C(0x06880000);
            return true;
        }
        if ((bank == 5u || bank == 6u) && mode == 3u) {
            const unsigned slot = (offset & 1u) + ((offset & 2u) ? 4u : 0u);
            const uint32_t base = UINT32_C(0x06880000) + slot * 0x4000u;
            if (address >= base && address - base < 0x4000u) {
                *physical = vram_bank_offsets[bank] + address - base;
                return true;
            }
        }
        if ((bank == 5u || bank == 6u) && mode == 4u) {
            const uint32_t base = (offset & 1u) * 0x4000u;
            if (address >= UINT32_C(0x06880000) + base &&
                address - (UINT32_C(0x06880000) + base) < 0x4000u) {
                *physical = vram_bank_offsets[bank] +
                            address - (UINT32_C(0x06880000) + base);
                return true;
            }
        }
        return false;
    }

    if (address >= UINT32_C(0x06898000) && address < UINT32_C(0x068a0000) &&
        bank == 7u && mode == 2u) {
        *physical = vram_bank_offsets[bank] + address - UINT32_C(0x06898000);
        return true;
    }

    /* OBJ extended palettes occupy 0x06890000-0x06897fff when banks F/G
       select mode 5.  The old test lived inside the preceding interval,
       which ends at 0x06890000 and could never match these addresses. */
    if (address >= UINT32_C(0x06890000) && address < UINT32_C(0x06898000) &&
        mode == 5u) {
        if (bank == 5u && address < UINT32_C(0x06894000)) {
            *physical = vram_bank_offsets[bank] +
                        address - UINT32_C(0x06890000);
            return true;
        }
        if (bank == 6u && address >= UINT32_C(0x06894000)) {
            *physical = vram_bank_offsets[bank] +
                        address - UINT32_C(0x06894000);
            return true;
        }
        return false;
    }

    if (address >= UINT32_C(0x06880000) && address < UINT32_C(0x06898000)) {
        if ((bank == 5u || bank == 6u) && mode == 3u) {
            const unsigned slot = (offset & 1u) + ((offset & 2u) ? 4u : 0u);
            const uint32_t base = UINT32_C(0x06880000) + slot * 0x4000u;
            if (address >= base && address - base < 0x4000u) {
                *physical = vram_bank_offsets[bank] + address - base;
                return true;
            }
        }
        return false;
    }

    if (address >= UINT32_C(0x06000000) && address < UINT32_C(0x06200000)) {
        relative = (address - UINT32_C(0x06000000)) & 0x7ffffu;
        if (bank <= 3u && mode == 1u) {
            const uint32_t base = offset * 0x20000u;
            if (relative >= base && relative - base < 0x20000u) {
                *physical = vram_bank_offsets[bank] + (relative & 0x1ffffu);
                return true;
            }
        } else if (bank == 4u && mode == 1u && relative < 0x10000u) {
            *physical = vram_bank_offsets[bank] + relative;
            return true;
        } else if ((bank == 5u || bank == 6u) && mode == 1u) {
            const unsigned block = relative >> 14;
            const unsigned base = (offset & 1u) + ((offset & 2u) << 1);
            if (block == base || block == base + 2u) {
                *physical = vram_bank_offsets[bank] + (relative & 0x3fffu);
                return true;
            }
        }
        return false;
    }

    if (address >= UINT32_C(0x06200000) && address < UINT32_C(0x06400000)) {
        relative = (address - UINT32_C(0x06200000)) & 0x1ffffu;
        const unsigned block = relative >> 14;
        if (bank == 2u && mode == 4u) {
            *physical = vram_bank_offsets[bank] + relative;
            return true;
        }
        if (bank == 7u && mode == 1u &&
            (block == 0u || block == 1u || block == 4u || block == 5u)) {
            *physical = vram_bank_offsets[bank] + (relative & 0x7fffu);
            return true;
        }
        if (bank == 8u && mode == 1u &&
            (block == 2u || block == 3u || block == 6u || block == 7u)) {
            *physical = vram_bank_offsets[bank] + (relative & 0x3fffu);
            return true;
        }
        return false;
    }

    if (address >= UINT32_C(0x06400000) && address < UINT32_C(0x06600000)) {
        relative = (address - UINT32_C(0x06400000)) & 0x3ffffu;
        if ((bank == 0u || bank == 1u) && mode == 2u) {
            const uint32_t base = (offset & 1u) * 0x20000u;
            if (relative >= base && relative - base < 0x20000u) {
                *physical = vram_bank_offsets[bank] + (relative & 0x1ffffu);
                return true;
            }
        } else if (bank == 4u && mode == 2u && relative < 0x10000u) {
            *physical = vram_bank_offsets[bank] + relative;
            return true;
        } else if ((bank == 5u || bank == 6u) && mode == 2u) {
            const unsigned block = relative >> 14;
            const unsigned base = (offset & 1u) + ((offset & 2u) << 1);
            if (block == base || block == base + 2u) {
                *physical = vram_bank_offsets[bank] + (relative & 0x3fffu);
                return true;
            }
        }
        return false;
    }

    if (address >= UINT32_C(0x06600000) && address < UINT32_C(0x06800000)) {
        relative = (address - UINT32_C(0x06600000)) & 0x1ffffu;
        if (bank == 3u && mode == 4u) {
            *physical = vram_bank_offsets[bank] + relative;
            return true;
        }
        if (bank == 8u && mode == 2u) {
            *physical = vram_bank_offsets[bank] + (relative & 0x3fffu);
            return true;
        }
    }
    return false;
}

const uint8_t *nds_vram_bank_pointer(const NdsCpu *cpu, unsigned bank,
                                     uint32_t address) {
    uint32_t physical;
    if (bank >= 9u || !vram_bank_address(cpu, bank, address, &physical))
        return NULL;
    return cpu->vram + physical;
}

static uint8_t vram_read8(const NdsCpu *cpu, uint32_t address) {
    uint8_t value = 0u;
    for (unsigned bank = 0; bank < 9u; ++bank) {
        uint32_t physical;
        if (vram_bank_address(cpu, bank, address, &physical))
            value |= cpu->vram[physical];
    }
    return value;
}

static NdsGpu *gpu_for_cpu(NdsCpu *cpu) {
    if (cpu->gpu != NULL)
        return cpu->gpu;
    return cpu->peer == NULL ? NULL : cpu->peer->gpu;
}

static void vram_write8(NdsCpu *cpu, uint32_t address, uint8_t value) {
    bool changed = false;
    for (unsigned bank = 0; bank < 9u; ++bank) {
        uint32_t physical;
        if (vram_bank_address(cpu, bank, address, &physical) &&
            cpu->vram[physical] != value) {
            cpu->vram[physical] = value;
            changed = true;
        }
    }
    if (changed)
        nds_gpu_invalidate_textures(gpu_for_cpu(cpu));
}

static void run_dma(NdsCpu *cpu, uint32_t control_address, uint32_t control);

static void raise_irq(NdsCpu *cpu, uint32_t irq) {
    uint8_t *flags = mapped(cpu, REG_IF);
    if (flags != NULL) {
        const uint32_t pending = nds_read32(cpu, REG_IF) | irq;
        memcpy(flags, &pending, sizeof(pending));
    }
}

static void update_gxfifo_irq(NdsCpu *cpu) {
    if (cpu->cpu_id != 9u || cpu->gpu == NULL)
        return;
    const uint32_t status = nds_gpu_read_status(cpu->gpu);
    const unsigned mode = (status >> 30) & 3u;
    if ((mode == 1u && status & (UINT32_C(1) << 25)) ||
        (mode == 2u && status & (UINT32_C(1) << 26)))
        raise_irq(cpu, IRQ_GXFIFO);
}

static bool initialize_save_memory(NdsCpu *cpu, const char *rom_path) {
    const char *slash = strrchr(rom_path, '/');
    const char *backslash = strrchr(rom_path, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash))
        slash = backslash;
    const char *extension = strrchr(rom_path, '.');
    const size_t stem_length = extension != NULL && (slash == NULL || extension > slash)
        ? (size_t)(extension - rom_path) : strlen(rom_path);
    cpu->save_path = malloc(stem_length + sizeof(".sav"));
    if (cpu->save_path == NULL)
        return false;
    cpu->owns_save_memory = true;
    memcpy(cpu->save_path, rom_path, stem_length);
    memcpy(cpu->save_path + stem_length, ".sav", sizeof(".sav"));
    FILE *save = fopen(cpu->save_path, "rb");
    cpu->save_size = DEFAULT_SAVE_SIZE;
    if (save != NULL && fseek(save, 0, SEEK_END) == 0) {
        const long length = ftell(save);
        if (length >= 512 && length <= 8 * 1024 * 1024 &&
            ((size_t)length & ((size_t)length - 1u)) == 0)
            cpu->save_size = (size_t)length;
        rewind(save);
    }
    cpu->save_data = malloc(cpu->save_size);
    if (cpu->save_data == NULL) {
        if (save != NULL) fclose(save);
        return false;
    }
    memset(cpu->save_data, 0xff, cpu->save_size);
    if (save != NULL) {
        (void)fread(cpu->save_data, 1, cpu->save_size, save);
        fclose(save);
    }
    return true;
}

static bool resize_save_memory(NdsCpu *cpu, size_t size) {
    if (size <= cpu->save_size)
        return true;
    uint8_t *resized = realloc(cpu->save_data, size);
    if (resized == NULL)
        return false;
    memset(resized + cpu->save_size, 0xff, size - cpu->save_size);
    cpu->save_data = resized;
    cpu->save_size = size;
    if (cpu->peer != NULL) {
        cpu->peer->save_data = resized;
        cpu->peer->save_size = size;
    }
    return true;
}

static unsigned backup_address_bytes(const NdsCpu *cpu) {
    return cpu->save_size <= 512u ? 1u : (cpu->save_size <= 65536u ? 2u : 3u);
}

static void grow_flash_for_address(NdsCpu *cpu) {
    if (backup_address_bytes(cpu) != 3u || cpu->backup_address < cpu->save_size)
        return;
    size_t size = cpu->save_size;
    while (size <= cpu->backup_address && size < 8u * 1024u * 1024u)
        size *= 2u;
    (void)resize_save_memory(cpu, size);
}

static void flush_save_memory(NdsCpu *cpu) {
    if (!cpu->backup_dirty || cpu->save_path == NULL || cpu->save_data == NULL)
        return;
    FILE *save = fopen(cpu->save_path, "wb");
    if (save == NULL)
        return;
    if (fwrite(cpu->save_data, 1, cpu->save_size, save) == cpu->save_size)
        cpu->backup_dirty = false;
    fclose(save);
}

static void backup_release(NdsCpu *cpu) {
    switch (cpu->backup_command) {
    case 0x01u:
    case 0x02u:
    case 0x0au:
    case 0xc7u:
    case 0xd8u:
    case 0xdbu:
        cpu->backup_status &= (uint8_t)~2u;
        flush_save_memory(cpu);
        break;
    default:
        break;
    }
    cpu->backup_selected = false;
    cpu->backup_position = 0u;
}

static uint8_t backup_transfer(NdsCpu *cpu, uint8_t value) {
    uint8_t response = 0xffu;
    if (!cpu->backup_selected) {
        cpu->backup_selected = true;
        cpu->backup_position = 0u;
        cpu->backup_address = 0u;
    }
    if (cpu->backup_position == 0u) {
        cpu->backup_command = value;
        if (cpu->save_size > 512u &&
            (value == 0x0bu || value == 0xc7u || value == 0xd8u || value == 0xdbu)) {
            (void)resize_save_memory(cpu, 256u * 1024u);
        }
        if (value == 0x04u)
            cpu->backup_status &= (uint8_t)~2u;
        else if (value == 0x06u)
            cpu->backup_status |= 2u;
        else if (value == 0xc7u && (cpu->backup_status & 2u)) {
            memset(cpu->save_data, 0xff, cpu->save_size);
            cpu->backup_dirty = true;
        }
        response = 0u;
    } else {
        switch (cpu->backup_command) {
        case 0x01u:
            if (cpu->backup_position == 1u)
                cpu->backup_status = (cpu->backup_status & 2u) | (value & 0x0cu);
            response = 0u;
            break;
        case 0x05u:
            response = cpu->backup_status;
            break;
        case 0x02u:
        case 0x0au:
            if (cpu->backup_position <= backup_address_bytes(cpu)) {
                cpu->backup_address = (cpu->backup_address << 8) | value;
                if (cpu->backup_position == backup_address_bytes(cpu))
                    grow_flash_for_address(cpu);
                response = 0u;
            } else if (cpu->backup_status & 2u) {
                cpu->save_data[cpu->backup_address++ & (cpu->save_size - 1u)] = value;
                cpu->backup_dirty = true;
                response = 0u;
            }
            break;
        case 0x03u:
            if (cpu->backup_position <= backup_address_bytes(cpu)) {
                cpu->backup_address = (cpu->backup_address << 8) | value;
                if (cpu->backup_position == backup_address_bytes(cpu))
                    grow_flash_for_address(cpu);
                response = 0u;
            } else {
                response = cpu->save_data[cpu->backup_address++ & (cpu->save_size - 1u)];
            }
            break;
        case 0x0bu:
            if (cpu->backup_position <= backup_address_bytes(cpu)) {
                cpu->backup_address = (cpu->backup_address << 8) | value;
                if (cpu->backup_position == backup_address_bytes(cpu))
                    grow_flash_for_address(cpu);
                response = 0u;
            } else if (cpu->backup_position == backup_address_bytes(cpu) + 1u) {
                response = 0u;
            } else {
                response = cpu->save_data[cpu->backup_address++ & (cpu->save_size - 1u)];
            }
            break;
        case 0xd8u:
        case 0xdbu:
            cpu->backup_address = (cpu->backup_address << 8) | value;
            response = 0u;
            if (cpu->backup_position == backup_address_bytes(cpu) && (cpu->backup_status & 2u)) {
                grow_flash_for_address(cpu);
                const size_t erase_size = cpu->backup_command == 0xd8u ? 0x10000u : 0x100u;
                const size_t start = cpu->backup_address & (cpu->save_size - 1u) & ~(erase_size - 1u);
                memset(cpu->save_data + start, 0xff, erase_size);
                cpu->backup_dirty = true;
            }
            break;
        case 0x9fu:
            response = 0xffu;
            break;
        default:
            break;
        }
    }
    cpu->backup_position++;
    return response;
}

static uint16_t crc16(const uint8_t *data, size_t size, uint16_t crc) {
    while (size-- != 0u) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8u; ++bit)
            crc = (uint16_t)((crc >> 1) ^ (crc & 1u ? 0xa001u : 0u));
    }
    return crc;
}

static uint8_t firmware_read_byte(const NdsCpu *cpu, uint32_t address) {
    address &= UINT32_C(0x0007ffff);
    if (address >= UINT32_C(0x0007fe00) && address < UINT32_C(0x00080000))
        return cpu->firmware_user[(address - UINT32_C(0x0007fe00)) & 0xffu];
    if (address == UINT32_C(0x20))
        return 0xc0u; /* 0x7fe00 >> 3, little endian. */
    if (address == UINT32_C(0x21))
        return 0xffu;
    return 0u;
}

static uint8_t firmware_transfer(NdsCpu *cpu, uint8_t value) {
    if (!cpu->firmware_selected) {
        cpu->firmware_selected = true;
        cpu->firmware_command = value;
        cpu->firmware_position = 0u;
        cpu->firmware_address = 0u;
        return 0u;
    }

    uint8_t response = 0u;
    switch (cpu->firmware_command) {
    case 0x03u:
        if (cpu->firmware_position < 3u)
            cpu->firmware_address = (cpu->firmware_address << 8) | value;
        else
            response = firmware_read_byte(cpu, cpu->firmware_address++);
        break;
    case 0x05u:
        response = 0u;
        break;
    case 0x9fu:
        response = 0xffu;
        break;
    default:
        break;
    }
    cpu->firmware_position++;
    return response;
}

static uint32_t cartridge_id(void) {
    uint32_t id = UINT32_C(0x000000c2);
    if (NDS_DEVICE_CAPACITY >= 3u && NDS_DEVICE_CAPACITY < 31u)
        id |= ((UINT32_C(1) << (NDS_DEVICE_CAPACITY - 3u)) - 1u) << 8;
    if (NDS_UNIT_CODE != 0u)
        id |= UINT32_C(0x40000000);
    return id;
}

static bool initialize_direct_boot(NdsCpu *cpu) {
    uint8_t header[0x170];
    if (fseek(cpu->rom_file, 0, SEEK_SET) != 0 ||
        fread(header, 1, sizeof(header), cpu->rom_file) != sizeof(header))
        return false;
    memcpy(cpu->main_ram + (UINT32_C(0x027ffe00) - MAIN_BASE), header, sizeof(header));

    /* Match the header/cart values the firmware leaves in main RAM.  SDK
       startup code uses these before it starts the game-specific boot. */
    const uint32_t cart_id = cartridge_id();
    uint16_t header_crc;
    uint16_t secure_crc;
    memcpy(&header_crc, header + 0x15e, sizeof(header_crc));
    memcpy(&secure_crc, header + 0x06c, sizeof(secure_crc));
    memcpy(cpu->main_ram + (UINT32_C(0x027ff800) - MAIN_BASE), &cart_id, sizeof(cart_id));
    memcpy(cpu->main_ram + (UINT32_C(0x027ff804) - MAIN_BASE), &cart_id, sizeof(cart_id));
    memcpy(cpu->main_ram + (UINT32_C(0x027ff808) - MAIN_BASE), &header_crc, sizeof(header_crc));
    memcpy(cpu->main_ram + (UINT32_C(0x027ff80a) - MAIN_BASE), &secure_crc, sizeof(secure_crc));
    memcpy(cpu->main_ram + (UINT32_C(0x027ffc00) - MAIN_BASE), &cart_id, sizeof(cart_id));
    memcpy(cpu->main_ram + (UINT32_C(0x027ffc04) - MAIN_BASE), &cart_id, sizeof(cart_id));
    memcpy(cpu->main_ram + (UINT32_C(0x027ffc08) - MAIN_BASE), &header_crc, sizeof(header_crc));
    memcpy(cpu->main_ram + (UINT32_C(0x027ffc0a) - MAIN_BASE), &secure_crc, sizeof(secure_crc));

    const uint16_t boot_indicator = 1u;
    const uint16_t gba_header = UINT16_C(0xffff);
    const uint16_t chip_type = UINT16_C(0x5835);
    memcpy(cpu->main_ram + (UINT32_C(0x027ffc40) - MAIN_BASE),
           &boot_indicator, sizeof(boot_indicator));
    memcpy(cpu->main_ram + (UINT32_C(0x027ffc30) - MAIN_BASE),
           &gba_header, sizeof(gba_header));
    memcpy(cpu->main_ram + (UINT32_C(0x027ff850) - MAIN_BASE),
           &chip_type, sizeof(chip_type));
    memcpy(cpu->main_ram + (UINT32_C(0x027ffc10) - MAIN_BASE),
           &chip_type, sizeof(chip_type));

    const uint32_t firmware_user_offset = UINT32_C(0x0007fe00);
    memcpy(cpu->main_ram + (UINT32_C(0x027ff868) - MAIN_BASE),
           &firmware_user_offset, sizeof(firmware_user_offset));
    uint8_t *user = cpu->firmware_user;
    memset(user, 0, sizeof(cpu->firmware_user));
    user[0] = 5u;
    user[3] = 1u;
    user[4] = 1u;
    static const char nickname[] = "ndsrecomp";
    for (unsigned index = 0; index < sizeof(nickname) - 1u; ++index)
        user[6u + index * 2u] = (uint8_t)nickname[index];
    user[26] = (uint8_t)(sizeof(nickname) - 1u);
    const uint16_t calibration_x2 = UINT16_C(255) << 4;
    const uint16_t calibration_y2 = UINT16_C(191) << 4;
    memcpy(user + 94u, &calibration_x2, sizeof(calibration_x2));
    memcpy(user + 96u, &calibration_y2, sizeof(calibration_y2));
    user[98] = 255u;
    user[99] = 191u;
    user[100] = 0x31u; /* English, maximum backlight. */
    user[102] = 26u;
    const uint16_t checksum = crc16(user, 0x70u, UINT16_C(0xffff));
    memcpy(user + 0x72u, &checksum, sizeof(checksum));
    memcpy(cpu->main_ram + (UINT32_C(0x027ffc80) - MAIN_BASE), user, 0x70u);
    return true;
}

bool nds_cpu_init_arm9(NdsCpu *cpu, const char *rom_path) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->cpu_id = 9u;
    cpu->dtcm_base = UINT32_C(0x03000000);
    cpu->dtcm_control = cpu->dtcm_base | 0xau;
    cpu->cp15_control = UINT32_C(0x00052078);
    cpu->owns_shared_memory = true;
    cpu->itcm = calloc(1, ITCM_SIZE);
    cpu->dtcm = calloc(1, DTCM_SIZE);
    cpu->main_ram = calloc(1, MAIN_SIZE);
    cpu->io = calloc(1, IO_SIZE);
    cpu->palette = calloc(1, PALETTE_SIZE);
    cpu->vram = calloc(1, VRAM_SIZE);
    cpu->oam = calloc(1, OAM_SIZE);
    cpu->display_fifo = calloc(DISPLAY_FIFO_PIXELS, sizeof(*cpu->display_fifo));
    cpu->rom_file = fopen(rom_path, "rb");
    cpu->gpu = nds_gpu_create();
    if (cpu->itcm == NULL || cpu->dtcm == NULL || cpu->main_ram == NULL || cpu->io == NULL ||
        cpu->palette == NULL || cpu->vram == NULL || cpu->oam == NULL ||
        cpu->display_fifo == NULL ||
        cpu->gpu == NULL || cpu->rom_file == NULL || !initialize_save_memory(cpu, rom_path) ||
        !initialize_direct_boot(cpu)) {
        nds_cpu_destroy(cpu);
        return false;
    }
    if (NDS_ARM9_RAM_ADDRESS < MAIN_BASE ||
        NDS_ARM9_RAM_ADDRESS - MAIN_BASE + nds_arm9_image_size > MAIN_SIZE) {
        nds_cpu_destroy(cpu);
        return false;
    }
    memcpy(cpu->main_ram + NDS_ARM9_RAM_ADDRESS - MAIN_BASE,
           nds_arm9_image, nds_arm9_image_size);
    cpu->io[REG_POSTFLG - IO_BASE] = 1u; /* Firmware has completed direct boot. */
    const uint16_t exmemcnt = UINT16_C(0xe880);
    const uint16_t power = UINT16_C(0x820f);
    const uint16_t keys_released = UINT16_C(0x03ff);
    const uint16_t extended_keys_released = UINT16_C(0x007f);
    memcpy(cpu->io + (UINT32_C(0x04000204) - IO_BASE), &exmemcnt, sizeof(exmemcnt));
    memcpy(cpu->io + (UINT32_C(0x04000304) - IO_BASE), &power, sizeof(power));
    const uint32_t rom_control = UINT32_C(0x20000000);
    const uint16_t aux_spi_control = UINT16_C(0x8000);
    const uint16_t sound_bias = UINT16_C(0x0200);
    memcpy(cpu->io + (REG_ROMCTRL - IO_BASE), &rom_control, sizeof(rom_control));
    memcpy(cpu->io + (REG_AUXSPICNT - IO_BASE), &aux_spi_control, sizeof(aux_spi_control));
    memcpy(cpu->io + (REG_SOUNDBIAS - IO_BASE), &sound_bias, sizeof(sound_bias));
    memcpy(cpu->io + (REG_KEYINPUT - IO_BASE), &keys_released, sizeof(keys_released));
    memcpy(cpu->io + (REG_EXTKEYIN - IO_BASE), &extended_keys_released,
           sizeof(extended_keys_released));
    if (NDS_ARM9_WAS_COMPRESSED &&
        NDS_ARM9_BUILD_INFO_OFFSET + 24u <= nds_arm9_image_size) {
        uint8_t *compressed_end = cpu->main_ram + NDS_ARM9_RAM_ADDRESS - MAIN_BASE +
                                  NDS_ARM9_BUILD_INFO_OFFSET + 20u;
        memset(compressed_end, 0, 4u);
    }
    cpu->cpsr = UINT32_C(0x1f);
    cpu->touch_y = UINT16_C(0x0fff);
    cpu->r[12] = NDS_ARM9_ENTRY;
    cpu->r[13] = UINT32_C(0x03002f7c);
    cpu->r[14] = NDS_ARM9_ENTRY;
    cpu->r[15] = NDS_ARM9_ENTRY;
    cpu->banked_r13[0x12] = UINT32_C(0x03003f80);
    cpu->banked_r13[0x13] = UINT32_C(0x03003fc0);
    return true;
}

bool nds_cpu_init_arm7(NdsCpu *cpu, NdsCpu *arm9) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->cpu_id = 7u;
    cpu->main_ram = arm9->main_ram;
    cpu->palette = arm9->palette;
    cpu->vram = arm9->vram;
    cpu->oam = arm9->oam;
    cpu->rom_file = arm9->rom_file;
    cpu->save_data = arm9->save_data;
    cpu->save_size = arm9->save_size;
    cpu->save_path = arm9->save_path;
    memcpy(cpu->firmware_user, arm9->firmware_user, sizeof(cpu->firmware_user));
    cpu->io = calloc(1, IO_SIZE);
    cpu->wram = calloc(1, ARM7_WRAM_SIZE);
    if (cpu->io == NULL || cpu->wram == NULL) {
        nds_cpu_destroy(cpu);
        return false;
    }
    if (NDS_ARM7_RAM_ADDRESS >= MAIN_BASE &&
        NDS_ARM7_RAM_ADDRESS - MAIN_BASE + nds_arm7_image_size <= MAIN_SIZE) {
        memcpy(cpu->main_ram + NDS_ARM7_RAM_ADDRESS - MAIN_BASE,
               nds_arm7_image, nds_arm7_image_size);
    } else if (NDS_ARM7_RAM_ADDRESS >= ARM7_WRAM_BASE &&
               NDS_ARM7_RAM_ADDRESS - ARM7_WRAM_BASE + nds_arm7_image_size <=
                   ARM7_WRAM_SIZE) {
        memcpy(cpu->wram + NDS_ARM7_RAM_ADDRESS - ARM7_WRAM_BASE,
               nds_arm7_image, nds_arm7_image_size);
    } else {
        nds_cpu_destroy(cpu);
        return false;
    }
    cpu->io[REG_POSTFLG - IO_BASE] = 1u;
    const uint16_t exmemstat = UINT16_C(0x0080);
    const uint16_t power = 1u;
    const uint16_t keys_released = UINT16_C(0x03ff);
    const uint16_t extended_keys_released = UINT16_C(0x007f);
    const uint32_t rom_control = UINT32_C(0x20000000);
    const uint16_t aux_spi_control = UINT16_C(0x8000);
    const uint16_t sound_bias = UINT16_C(0x0200);
    memcpy(cpu->io + (UINT32_C(0x04000204) - IO_BASE), &exmemstat, sizeof(exmemstat));
    memcpy(cpu->io + (UINT32_C(0x04000304) - IO_BASE), &power, sizeof(power));
    memcpy(cpu->io + (REG_ROMCTRL - IO_BASE), &rom_control, sizeof(rom_control));
    memcpy(cpu->io + (REG_AUXSPICNT - IO_BASE), &aux_spi_control, sizeof(aux_spi_control));
    memcpy(cpu->io + (REG_SOUNDBIAS - IO_BASE), &sound_bias, sizeof(sound_bias));
    memcpy(cpu->io + (REG_KEYINPUT - IO_BASE), &keys_released, sizeof(keys_released));
    memcpy(cpu->io + (REG_EXTKEYIN - IO_BASE), &extended_keys_released,
           sizeof(extended_keys_released));
    cpu->cpsr = UINT32_C(0x1f);
    cpu->touch_y = UINT16_C(0x0fff);
    cpu->r[12] = NDS_ARM7_ENTRY;
    cpu->r[13] = UINT32_C(0x0380fd80);
    cpu->r[14] = NDS_ARM7_ENTRY;
    cpu->r[15] = NDS_ARM7_ENTRY;
    cpu->banked_r13[0x12] = UINT32_C(0x0380ff80);
    cpu->banked_r13[0x13] = UINT32_C(0x0380ffc0);
    return true;
}

void nds_cpu_link(NdsCpu *arm9, NdsCpu *arm7) {
    arm9->peer = arm7;
    arm7->peer = arm9;
}

void nds_set_touch(NdsCpu *cpu, bool touching, uint16_t x, uint16_t y) {
    if (touching) {
        if (x > 255u) x = 255u;
        if (y > 191u) y = 191u;
        cpu->touch_x = (uint16_t)(x << 4);
        cpu->touch_y = (uint16_t)(y << 4);
        cpu->io[REG_EXTKEYIN - IO_BASE] &= (uint8_t)~0x40u;
    } else {
        cpu->touch_x = 0u;
        cpu->touch_y = UINT16_C(0x0fff);
        cpu->io[REG_EXTKEYIN - IO_BASE] |= 0x40u;
    }
}

void nds_tick(NdsCpu *cpu, uint32_t cycles) {
    const uint32_t video_cycles = cpu->cpu_id == 9u ? cycles / 2u : cycles;
    if (video_cycles != 0u) {
        const uint32_t old_tick = cpu->tick_count;
        const uint32_t advanced = old_tick + video_cycles;
        const bool wrapped = advanced >= NDS_FRAME_CYCLES;
        const uint32_t next_tick = advanced % NDS_FRAME_CYCLES;
        const unsigned old_line = old_tick / NDS_LINE_CYCLES;
        const unsigned next_line = next_tick / NDS_LINE_CYCLES;
        const unsigned old_line_cycle = old_tick % NDS_LINE_CYCLES;
        const unsigned next_line_cycle = next_tick % NDS_LINE_CYCLES;
        const bool line_start = wrapped || next_line != old_line;
        const bool entering_hblank = old_line_cycle < NDS_HBLANK_CYCLES &&
                                     old_line_cycle + video_cycles >= NDS_HBLANK_CYCLES;
        const bool entering_vblank = !wrapped && old_line < 192u && next_line >= 192u;
        uint16_t status;
        memcpy(&status, cpu->io + (REG_DISPSTAT - IO_BASE), sizeof(status));
        status = (uint16_t)((status & (uint16_t)~3u) |
                            (next_line >= 192u ? 1u : 0u) |
                            (next_line_cycle >= NDS_HBLANK_CYCLES ? 2u : 0u));
        const unsigned comparison = (status >> 8) | ((status & 0x80u) << 1);
        if (next_line == comparison) {
            status |= 4u;
            if (line_start && status & 0x20u)
                raise_irq(cpu, IRQ_VCOUNT);
        } else {
            status &= (uint16_t)~4u;
        }
        if (entering_hblank && status & 0x10u)
            raise_irq(cpu, IRQ_HBLANK);
        if (entering_vblank) {
            if (cpu->cpu_id == 9u && cpu->gpu != NULL)
                nds_gpu_vblank(cpu->gpu);
            if (status & 8u)
                raise_irq(cpu, IRQ_VBLANK);
        }
        const uint16_t count = (uint16_t)next_line;
        memcpy(cpu->io + (REG_VCOUNT - IO_BASE), &count, sizeof(count));
        memcpy(cpu->io + (REG_DISPSTAT - IO_BASE), &status, sizeof(status));
        cpu->tick_count = next_tick;
    }
    static const uint32_t divisors[4] = {1u, 64u, 256u, 1024u};
    bool cascade = false;
    for (unsigned timer = 0; timer < 4u; ++timer) {
        const uint32_t address = REG_TIMER0 + timer * 4u;
        const uint16_t control = nds_read16(cpu, address + 2u);
        if (!(control & 0x80u)) {
            cascade = false;
            continue;
        }
        uint32_t increments = 0;
        if (control & 4u) {
            increments = cascade ? 1u : 0u;
        } else {
            cpu->timer_accum[timer] += cycles;
            const uint32_t divisor = divisors[control & 3u];
            increments = cpu->timer_accum[timer] / divisor;
            cpu->timer_accum[timer] %= divisor;
        }
        cascade = false;
        while (increments-- != 0u) {
            const uint16_t counter = nds_read16(cpu, address);
            if (counter == 0xffffu) {
                const uint16_t configured = cpu->timer_reload[timer];
                nds_write8(cpu, address, (uint8_t)configured);
                nds_write8(cpu, address + 1u, (uint8_t)(configured >> 8));
                cascade = true;
                if (control & 0x40u)
                    raise_irq(cpu, UINT32_C(1) << (3u + timer));
            } else {
                const uint16_t next = (uint16_t)(counter + 1u);
                nds_write8(cpu, address, (uint8_t)next);
                nds_write8(cpu, address + 1u, (uint8_t)(next >> 8));
            }
        }
    }
}

void nds_cpu_destroy(NdsCpu *cpu) {
    nds_gpu_destroy(cpu->gpu);
    cpu->gpu = NULL;
    free(cpu->io);
    free(cpu->wram);
    if (cpu->owns_save_memory) {
        flush_save_memory(cpu);
        free(cpu->save_data);
        free(cpu->save_path);
    }
    if (cpu->owns_shared_memory) {
        free(cpu->itcm);
        free(cpu->dtcm);
        free(cpu->main_ram);
        free(cpu->palette);
        free(cpu->vram);
        free(cpu->oam);
        free(cpu->display_fifo);
        if (cpu->rom_file != NULL)
            fclose(cpu->rom_file);
    }
    cpu->itcm = NULL;
    cpu->dtcm = NULL;
    cpu->main_ram = NULL;
    cpu->io = NULL;
    cpu->palette = NULL;
    cpu->vram = NULL;
    cpu->oam = NULL;
    cpu->display_fifo = NULL;
    cpu->display_fifo_position = 0u;
    cpu->wram = NULL;
    cpu->save_data = NULL;
    cpu->save_path = NULL;
    cpu->save_size = 0u;
    cpu->rom_file = NULL;
}

NdsRunResult nds_cpu_trap(NdsCpu *cpu, NdsRunResult result, uint32_t pc, uint32_t word) {
    /* A statically translated instruction can be replaced by a ROM loader,
       overlay, or self-modifying code.  Interpret that one changed opcode
       instead of treating the normal dynamic-code path as a fatal trap. */
    if (result == NDS_RUN_CODE_MISMATCH) {
        const bool executed = nds_cpu_is_thumb(cpu) ?
            nds_exec_thumb(cpu, (uint16_t)word, pc) :
            nds_exec_arm(cpu, word, pc);
        if (executed)
            return NDS_RUN_BUDGET_EXHAUSTED;
        result = NDS_RUN_UNSUPPORTED;
    }
    cpu->trap_pc = pc;
    cpu->trap_word = word;
    return result;
}

bool nds_condition(const NdsCpu *cpu, unsigned condition) {
    const bool n = (cpu->cpsr & FLAG_N) != 0;
    const bool z = (cpu->cpsr & FLAG_Z) != 0;
    const bool c = (cpu->cpsr & FLAG_C) != 0;
    const bool v = (cpu->cpsr & FLAG_V) != 0;
    switch (condition & 15u) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && n == v;
    case 0xD: return z || n != v;
    case 0xE: return true;
    default: return false;
    }
}

bool nds_cpu_is_thumb(const NdsCpu *cpu) {
    return (cpu->cpsr & FLAG_T) != 0;
}

uint8_t nds_read8(const NdsCpu *cpu, uint32_t address) {
    if (address >= VRAM_BASE && address < VRAM_END)
        return vram_read8(cpu, address);
    const uint8_t *data = mapped_const(cpu, address);
    return data == NULL ? 0 : *data;
}

uint16_t nds_read16(const NdsCpu *cpu, uint32_t address) {
    if (cpu->cpu_id == 9u && address >= REG_RAM_COUNT &&
        address < REG_RAM_COUNT + 4u) {
        const uint32_t result = nds_gpu_read_ram_count(cpu->gpu);
        return (uint16_t)(result >> ((address & 2u) * 8u));
    }
    if (cpu->cpu_id == 9u && address >= REG_POS_RESULT &&
        address < REG_POS_RESULT + 16u) {
        const uint32_t result = nds_gpu_read_position_result(cpu->gpu,
            (address - REG_POS_RESULT) / 4u);
        return (uint16_t)(result >> ((address & 2u) * 8u));
    }
    if (cpu->cpu_id == 9u && address >= REG_VEC_RESULT &&
        address < REG_VEC_RESULT + 6u)
        return nds_gpu_read_vector_result(cpu->gpu,
                                          (address - REG_VEC_RESULT) / 2u);
    if (cpu->cpu_id == 9u && address == REG_GXSTAT)
        return (uint16_t)nds_gpu_read_status(cpu->gpu);
    if (cpu->cpu_id == 9u && address >= REG_CLIP_MATRIX &&
        address < REG_CLIP_MATRIX + 64u) {
        const uint32_t result = nds_gpu_read_clip_matrix(cpu->gpu,
            (address - REG_CLIP_MATRIX) / 4u);
        return (uint16_t)(result >> ((address & 2u) * 8u));
    }
    if (cpu->cpu_id == 9u && address >= REG_VECTOR_MATRIX &&
        address < REG_VECTOR_MATRIX + 36u) {
        const uint32_t result = nds_gpu_read_vector_matrix(cpu->gpu,
            (address - REG_VECTOR_MATRIX) / 4u);
        return (uint16_t)(result >> ((address & 2u) * 8u));
    }
    if (address == REG_IPCFIFOCNT) {
        uint16_t value = (uint16_t)(nds_read8(cpu, address) |
                                    ((uint16_t)nds_read8(cpu, address + 1u) << 8));
        value &= (uint16_t)~0x0303u;
        const unsigned send_count = cpu->peer == NULL ? 0u : cpu->peer->fifo_count;
        if (send_count == 0u) value |= 1u;
        if (send_count == 16u) value |= 2u;
        if (cpu->fifo_count == 0u) value |= 1u << 8;
        if (cpu->fifo_count == 16u) value |= 1u << 9;
        return value;
    }
    return (uint16_t)(nds_read8(cpu, address) | ((uint16_t)nds_read8(cpu, address + 1u) << 8));
}

uint32_t nds_read32(const NdsCpu *cpu, uint32_t address) {
    if (cpu->cpu_id == 9u && address == REG_GXSTAT)
        return nds_gpu_read_status(cpu->gpu);
    if (cpu->cpu_id == 9u && address == REG_RAM_COUNT)
        return nds_gpu_read_ram_count(cpu->gpu);
    if (cpu->cpu_id == 9u && address >= REG_POS_RESULT &&
        address < REG_POS_RESULT + 16u && (address & 3u) == 0u)
        return nds_gpu_read_position_result(cpu->gpu,
                                             (address - REG_POS_RESULT) / 4u);
    if (cpu->cpu_id == 9u && address == REG_VEC_RESULT)
        return (uint32_t)nds_gpu_read_vector_result(cpu->gpu, 0u) |
               ((uint32_t)nds_gpu_read_vector_result(cpu->gpu, 1u) << 16);
    if (cpu->cpu_id == 9u && address == REG_VEC_RESULT + 4u)
        return nds_gpu_read_vector_result(cpu->gpu, 2u);
    if (cpu->cpu_id == 9u && address >= REG_CLIP_MATRIX &&
        address < REG_CLIP_MATRIX + 64u && (address & 3u) == 0u)
        return nds_gpu_read_clip_matrix(cpu->gpu,
                                        (address - REG_CLIP_MATRIX) / 4u);
    if (cpu->cpu_id == 9u && address >= REG_VECTOR_MATRIX &&
        address < REG_VECTOR_MATRIX + 36u && (address & 3u) == 0u)
        return nds_gpu_read_vector_matrix(cpu->gpu,
                                          (address - REG_VECTOR_MATRIX) / 4u);
    if (address == REG_IPCFIFORECV) {
        NdsCpu *mutable = (NdsCpu *)cpu;
        uint32_t value = mutable->fifo_word;
        if (mutable->fifo_count != 0u) {
            value = mutable->fifo_queue[mutable->fifo_head++ & 15u];
            mutable->fifo_count--;
            mutable->fifo_word = value;
            mutable->fifo_received++;
        }
        mutable->fifo_pending = mutable->fifo_count != 0u;
        return value;
    }
    if (address == REG_CARDDATA) {
        NdsCpu *mutable = (NdsCpu *)cpu;
        uint32_t value = UINT32_C(0xffffffff);
        if (mutable->card_remaining != 0) {
            const uint8_t *command = mapped_const(mutable, REG_CARDCMD);
            if (command != NULL && command[0] == 0xb8u) {
                value = cartridge_id();
            } else if (command != NULL && command[0] == 0xb7u && mutable->rom_file != NULL) {
                FILE *rom = mutable->rom_file;
                uint8_t bytes[4] = {0xff, 0xff, 0xff, 0xff};
                if (fseek(rom, (long)mutable->card_address, SEEK_SET) == 0)
                    (void)fread(bytes, 1, sizeof(bytes), rom);
                value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
            }
            const uint32_t consumed = mutable->card_remaining < 4u ? mutable->card_remaining : 4u;
            mutable->card_address += consumed;
            mutable->card_remaining -= consumed;
            if (mutable->card_remaining == 0) {
                uint8_t *control = mapped(mutable, REG_ROMCTRL);
                if (control != NULL) {
                    control[2] &= (uint8_t)~0x80u; /* no data word ready */
                    control[3] &= (uint8_t)~0x80u; /* transfer complete */
                }
            }
        }
        return value;
    }
    const uint32_t aligned = address & ~3u;
    const uint32_t value = nds_read16(cpu, aligned) | ((uint32_t)nds_read16(cpu, aligned + 2u) << 16);
    return rotate_right(value, (address & 3u) * 8u);
}

static uint64_t io_read64(const NdsCpu *cpu, uint32_t address) {
    uint64_t value = 0;
    memcpy(&value, cpu->io + address - IO_BASE, sizeof(value));
    return value;
}

static void io_write64(NdsCpu *cpu, uint32_t address, uint64_t value) {
    memcpy(cpu->io + address - IO_BASE, &value, sizeof(value));
}

static void update_division(NdsCpu *cpu) {
    const unsigned mode = nds_read16(cpu, REG_DIVCNT) & 3u;
    const uint64_t raw_numerator = io_read64(cpu, REG_DIVNUMER);
    const uint64_t raw_denominator = io_read64(cpu, REG_DIVDENOM);
    int64_t numerator;
    int64_t denominator;
    if (mode == 0u) {
        numerator = (int32_t)raw_numerator;
        denominator = (int32_t)raw_denominator;
    } else if (mode == 1u || mode == 3u) {
        numerator = (int64_t)raw_numerator;
        denominator = (int32_t)raw_denominator;
    } else {
        numerator = (int64_t)raw_numerator;
        denominator = (int64_t)raw_denominator;
    }

    int64_t quotient;
    int64_t remainder;
    if (denominator == 0) {
        quotient = numerator < 0 ? 1 : -1;
        remainder = numerator;
        if (mode == 0u)
            quotient ^= (int64_t)UINT64_C(0xffffffff00000000);
    } else if ((mode == 0u && numerator == INT32_MIN && denominator == -1) ||
               (mode != 0u && numerator == INT64_MIN && denominator == -1)) {
        quotient = mode == 0u ? INT32_MIN : INT64_MIN;
        remainder = 0;
    } else {
        quotient = numerator / denominator;
        remainder = numerator % denominator;
    }

    io_write64(cpu, REG_DIVRESULT, (uint64_t)quotient);
    io_write64(cpu, REG_DIVREM, (uint64_t)remainder);
    const uint16_t control = (uint16_t)(mode | (raw_denominator == 0u ? 0x4000u : 0u));
    memcpy(cpu->io + REG_DIVCNT - IO_BASE, &control, sizeof(control));
}

static uint32_t integer_sqrt(uint64_t value) {
    uint64_t remainder = value;
    uint64_t result = 0;
    uint64_t bit = UINT64_C(1) << 62;
    while (bit > remainder)
        bit >>= 2;
    while (bit != 0u) {
        if (remainder >= result + bit) {
            remainder -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)result;
}

static void update_square_root(NdsCpu *cpu) {
    const uint16_t control = nds_read16(cpu, REG_SQRTCNT) & 1u;
    const uint64_t parameter = io_read64(cpu, REG_SQRTPARAM);
    const uint32_t result = integer_sqrt(control ? parameter : (uint32_t)parameter);
    memcpy(cpu->io + REG_SQRTRESULT - IO_BASE, &result, sizeof(result));
    memcpy(cpu->io + REG_SQRTCNT - IO_BASE, &control, sizeof(control));
}

void nds_write8(NdsCpu *cpu, uint32_t address, uint8_t value) {
    if (address >= VRAM_BASE && address < VRAM_END) {
        vram_write8(cpu, address, value);
        return;
    }
    if (address == REG_AUXSPIDATA && cpu->save_data != NULL) {
        uint8_t response = 0u;
        const uint16_t control = nds_read16(cpu, REG_AUXSPICNT);
        if ((control & 0xa000u) == 0xa000u)
            response = backup_transfer(cpu, value);
        uint8_t *data = mapped(cpu, address);
        if (data != NULL)
            *data = response;
        if (cpu->backup_selected && !(control & 0x40u))
            backup_release(cpu);
        return;
    }
    if (address == REG_SPIDATA && cpu->cpu_id == 7u) {
        uint8_t response = 0u;
        const uint16_t control = nds_read16(cpu, REG_SPICNT);
        const unsigned device = (control >> 8) & 3u;
        if ((control & 0x8000u) && device == 1u) {
            response = firmware_transfer(cpu, value);
            if (!(control & 0x0800u))
                cpu->firmware_selected = false;
        } else if ((control & 0x8000u) && device == 2u) {
            if (cpu->touch_phase == 1u)
                response = (uint8_t)((cpu->touch_result >> 5) & 0x7fu);
            else if (cpu->touch_phase == 2u)
                response = (uint8_t)(cpu->touch_result << 3);
            if (value & 0x80u) {
                cpu->touch_phase = 1u;
                if ((value & 0x70u) == 0x10u)
                    cpu->touch_result = cpu->touch_y;
                else if ((value & 0x70u) == 0x50u)
                    cpu->touch_result = cpu->touch_x;
                else
                    cpu->touch_result = UINT16_C(0x0fff);
                if (value & 8u)
                    cpu->touch_result &= UINT16_C(0x0ff0);
            } else {
                cpu->touch_phase++;
            }
        }
        uint8_t *data = mapped(cpu, address);
        if (data != NULL)
            *data = response;
        return;
    }
    uint8_t *data = mapped(cpu, address);
    if (data != NULL) {
        if (address >= REG_IF && address < REG_IF + 4u)
            *data &= (uint8_t)~value;
        else
            *data = value;
    }
    if (address >= REG_VRAMCNT && address < REG_VRAMCNT + 9u)
        nds_gpu_invalidate_textures(gpu_for_cpu(cpu));
    if (cpu->cpu_id == 9u && address >= REG_1DOT_DEPTH &&
        address < REG_1DOT_DEPTH + 2u)
        nds_gpu_set_1dot_depth(cpu->gpu, nds_read16(cpu, REG_1DOT_DEPTH));
    if (address == SDK_PROBE_MAILBOX) {
        uint8_t *mailbox = mapped(cpu, SDK_PROBE_MAILBOX);
        if (mailbox != NULL) {
            mailbox[1] = value;
            if ((value & 15u) == 15u)
                mailbox[2] = 1u;
        }
    }
}

void nds_write16(NdsCpu *cpu, uint32_t address, uint16_t value) {
    if (cpu->cpu_id == 9u && address == REG_GXSTAT) {
        nds_gpu_write_status(cpu->gpu, value);
        return;
    }
    if (address == REG_AUXSPICNT && cpu->backup_selected &&
        (nds_read16(cpu, address) & 0x2000u) && !(value & 0x2000u))
        backup_release(cpu);
    if (address >= REG_TIMER0 && address < REG_TIMER0 + 16u &&
        (address - REG_TIMER0) % 4u == 0u)
        cpu->timer_reload[(address - REG_TIMER0) / 4u] = value;
    if (address == REG_DISPSTAT) {
        value = (uint16_t)((value & ~7u) | (nds_read16(cpu, address) & 7u));
    }
    if (address == REG_VCOUNT)
        return;
    if (address == REG_IPCSYNC) {
        cpu->reschedule = true;
        const uint16_t previous = nds_read16(cpu, address);
        value = (uint16_t)((value & 0x6f00u) | (previous & 15u));
        if (cpu->peer != NULL) {
            uint8_t *peer_sync = mapped(cpu->peer, REG_IPCSYNC);
            if (peer_sync != NULL)
                peer_sync[0] = (uint8_t)((peer_sync[0] & 0xf0u) | ((value >> 8) & 15u));
            if (value & 0x2000u && nds_read16(cpu->peer, REG_IPCSYNC) & 0x4000u) {
                uint8_t *flags = mapped(cpu->peer, REG_IF);
                if (flags != NULL) {
                    const uint32_t pending = nds_read32(cpu->peer, REG_IF) | IRQ_IPC_SYNC;
                    memcpy(flags, &pending, sizeof(pending));
                }
            }
        }
    } else if (address == REG_IPCFIFOCNT) {
        if (value & (1u << 3)) {
            cpu->fifo_head = 0;
            cpu->fifo_tail = 0;
            cpu->fifo_count = 0;
            cpu->fifo_pending = false;
        }
        value = (uint16_t)(value & 0x8404u);
    }
    nds_write8(cpu, address, (uint8_t)value);
    nds_write8(cpu, address + 1u, (uint8_t)(value >> 8));
    if (cpu->cpu_id == 9u &&
        (address == REG_DIVCNT ||
         (address >= REG_DIVNUMER && address < REG_DIVDENOM + 8u)))
        update_division(cpu);
    if (cpu->cpu_id == 9u &&
        (address == REG_SQRTCNT ||
         (address >= REG_SQRTPARAM && address < REG_SQRTPARAM + 8u)))
        update_square_root(cpu);
}

void nds_write32(NdsCpu *cpu, uint32_t address, uint32_t value) {
    const uint32_t aligned = address & ~3u;
    if (cpu->cpu_id == 9u && aligned == REG_DISP_MMEM_FIFO &&
        cpu->display_fifo != NULL) {
        cpu->display_fifo[cpu->display_fifo_position % DISPLAY_FIFO_PIXELS] =
            (uint16_t)value;
        cpu->display_fifo[(cpu->display_fifo_position + 1u) % DISPLAY_FIFO_PIXELS] =
            (uint16_t)(value >> 16);
        cpu->display_fifo_position =
            (cpu->display_fifo_position + 2u) % DISPLAY_FIFO_PIXELS;
        return;
    }
    if (cpu->cpu_id == 9u && aligned == REG_GXSTAT) {
        nds_gpu_write_status(cpu->gpu, value);
        return;
    }
    /* GXFIFO is mirrored through 0x0400043f.  ARM9 display-list code uses
       STRD/STM against those mirrors to submit one unpacked command plus its
       parameters. */
    if (cpu->cpu_id == 9u && aligned >= REG_GXFIFO &&
        aligned < REG_GXFIFO + 0x40u) {
        nds_gpu_write32(cpu->gpu, value);
        return;
    }
    if (cpu->cpu_id == 9u) {
        const int command = gx_port_command(aligned);
        if (command >= 0) {
            nds_gpu_write_port(cpu->gpu, (uint8_t)command, value);
            return;
        }
    }
    nds_write16(cpu, aligned, (uint16_t)value);
    nds_write16(cpu, aligned + 2u, (uint16_t)(value >> 16));
    if (aligned == REG_ROMCTRL && value & UINT32_C(0x80000000)) {
        const uint8_t *command = mapped_const(cpu, REG_CARDCMD);
        if (command != NULL) {
            const unsigned block = (value >> 24) & 7u;
            cpu->card_remaining = block == 7u ? 4u : (block == 0u ? 0u : 256u << block);
            if (command[0] == 0xb7u) {
                cpu->card_address = ((uint32_t)command[1] << 24) |
                                    ((uint32_t)command[2] << 16) |
                                    ((uint32_t)command[3] << 8) | command[4];
            }
            uint8_t *control = mapped(cpu, REG_ROMCTRL);
            if (control != NULL) {
                if (cpu->card_remaining != 0)
                    control[2] |= 0x80u; /* data word ready */
                else
                    control[3] &= (uint8_t)~0x80u;
            }
            for (unsigned channel = 0; channel < 4u && cpu->card_remaining != 0; ++channel) {
                const uint32_t dma_address = UINT32_C(0x040000b8) + channel * 12u;
                const uint32_t dma = nds_read32(cpu, dma_address);
                const unsigned timing = cpu->cpu_id == 9u ? (dma >> 27) & 7u
                                                          : (dma >> 28) & 3u;
                const unsigned card_timing = cpu->cpu_id == 9u ? 5u : 2u;
                if (dma & UINT32_C(0x80000000) && timing == card_timing)
                    run_dma(cpu, dma_address, dma);
            }
        }
    }
    if (aligned >= UINT32_C(0x040000b8) && aligned <= UINT32_C(0x040000dc) &&
        (aligned - UINT32_C(0x040000b8)) % 12u == 0 && value & UINT32_C(0x80000000)) {
        const unsigned timing = cpu->cpu_id == 9u ? (value >> 27) & 7u
                                                  : (value >> 28) & 3u;
        const unsigned card_timing = cpu->cpu_id == 9u ? 5u : 2u;
        if (timing != card_timing)
            run_dma(cpu, aligned, value);
    }
    if (aligned == REG_IPCFIFOSEND) {
        cpu->reschedule = true;
        cpu->fifo_sent++;
        cpu->fifo_word = value;
        NdsCpu *receiver = cpu->peer;
        if (receiver != NULL && receiver->fifo_count < 16u) {
            receiver->fifo_queue[receiver->fifo_tail++ & 15u] = value;
            receiver->fifo_count++;
            receiver->fifo_word = value;
            receiver->fifo_pending = true;
            if (nds_read16(receiver, REG_IPCFIFOCNT) & (1u << 10)) {
                uint8_t *flags = mapped(receiver, REG_IF);
                if (flags != NULL) {
                    const uint32_t pending = nds_read32(receiver, REG_IF) | IRQ_IPC_RECV;
                    memcpy(flags, &pending, sizeof(pending));
                }
            }
        }
    }
}

void nds_poll_interrupts(NdsCpu *cpu) {
    update_gxfifo_irq(cpu);
    if (cpu->cpsr & (UINT32_C(1) << 7))
        return;
    if (!(nds_read32(cpu, REG_IME) & 1u))
        return;
    const uint32_t pending = nds_read32(cpu, REG_IE) & nds_read32(cpu, REG_IF);
    if (!pending)
        return;
    const uint32_t handler_address = cpu->cpu_id == 7u
        ? SDK_IRQ_HANDLER_ARM7 : cpu->dtcm_base + DTCM_SIZE - 4u;
    const uint32_t handler = nds_read32(cpu, handler_address);
    if (handler == 0)
        return;
    cpu->halted = false;
    cpu->irq_count++;
    cpu->irq_sources |= pending;
    cpu->last_irq_handler = handler;
    const uint32_t return_address = cpu->r[15];
    const uint32_t saved_status = cpu->cpsr;
    replace_cpsr(cpu, (saved_status & ~UINT32_C(0x3f)) | UINT32_C(0x92) |
                      (handler & 1u ? FLAG_T : 0u));
    cpu->spsr = saved_status;
    cpu->banked_spsr[0x12] = saved_status;

    /* The NDS BIOS wrapper saves this frame before calling the SDK handler.
       NitroSDK's thread switcher consumes and rebuilds it, so bypassing the
       wrapper loses the selected thread's return PC and register state. */
    cpu->r[13] -= 6u * sizeof(uint32_t);
    nds_write32(cpu, cpu->r[13] + 0u, cpu->r[0]);
    nds_write32(cpu, cpu->r[13] + 4u, cpu->r[1]);
    nds_write32(cpu, cpu->r[13] + 8u, cpu->r[2]);
    nds_write32(cpu, cpu->r[13] + 12u, cpu->r[3]);
    nds_write32(cpu, cpu->r[13] + 16u, cpu->r[12]);
    nds_write32(cpu, cpu->r[13] + 20u, return_address + 4u);
    cpu->r[14] = IRQ_RETURN_SENTINEL;
    cpu->r[15] = handler & ~1u;
}

bool nds_finish_interrupt(NdsCpu *cpu) {
    if (cpu->r[15] != IRQ_RETURN_SENTINEL)
        return false;
    const uint32_t frame = cpu->r[13];
    cpu->r[0] = nds_read32(cpu, frame + 0u);
    cpu->r[1] = nds_read32(cpu, frame + 4u);
    cpu->r[2] = nds_read32(cpu, frame + 8u);
    cpu->r[3] = nds_read32(cpu, frame + 12u);
    cpu->r[12] = nds_read32(cpu, frame + 16u);
    const uint32_t hardware_lr = nds_read32(cpu, frame + 20u);
    const uint32_t return_address = hardware_lr - 4u;
    cpu->r[14] = hardware_lr;
    cpu->r[13] = frame + 6u * sizeof(uint32_t);
    const uint32_t saved_status = cpu->spsr;
    replace_cpsr(cpu, saved_status);
    cpu->r[15] = saved_status & FLAG_T ? return_address & ~1u
                                       : return_address & ~3u;
    cpu->irq_completed++;
    return true;
}

bool nds_exec_arm(NdsCpu *cpu, uint32_t word, uint32_t pc) {
    const uint32_t next = pc + 4u;
    if ((word & 0xfe000000u) == 0xfa000000u) {
        const int32_t displacement = sign_extend32(
            ((word & 0x00ffffffu) << 2) | ((word >> 23) & 2u), 26
        );
        nds_branch_link_exchange_immediate(
            cpu, (uint32_t)(pc + 8u + displacement), next
        );
        return true;
    }
    const unsigned condition = word >> 28;
    if (condition == 0xfu)
        return false;
    if (!nds_condition(cpu, condition)) {
        cpu->r[15] = next;
        return true;
    }
    if ((word & 0x0ffffff0u) == 0x012fff10u)
        return nds_branch_exchange(cpu, word & 15u, false, pc);
    if ((word & 0x0ffffff0u) == 0x012fff30u)
        return nds_branch_exchange(cpu, word & 15u, true, pc);
    if ((word & 0x0e000000u) == 0x0a000000u) {
        if (word & (1u << 24))
            cpu->r[14] = next;
        cpu->r[15] = (uint32_t)(pc + 8u +
                                 sign_extend32(word & 0x00ffffffu, 24) * 4);
        return true;
    }
    bool handled;
    if ((word & 0x0fc000f0u) == 0x00000090u)
        handled = nds_exec_multiply(cpu, word, pc);
    else if ((word & 0x0f8000f0u) == 0x00800090u)
        handled = nds_exec_long_multiply(cpu, word, pc);
    else if ((word & 0x0e000090u) == 0x00000090u)
        handled = nds_exec_half_transfer(cpu, word, pc);
    else if ((word & 0x0fff0ff0u) == 0x016f0f10u) {
        nds_exec_clz(cpu, word, pc);
        handled = true;
    } else if ((word & 0x0fbf0fffu) == 0x010f0000u ||
               (word & 0x0fb0fff0u) == 0x0120f000u ||
               (word & 0x0fb0f000u) == 0x0320f000u)
        handled = nds_exec_status(cpu, word, pc);
    else if ((word & 0x0c000000u) == 0x00000000u)
        handled = nds_exec_data_processing(cpu, word, pc);
    else if ((word & 0x0c000000u) == 0x04000000u)
        handled = nds_exec_single_transfer(cpu, word, pc);
    else if ((word & 0x0e000000u) == 0x08000000u)
        handled = nds_exec_block_transfer(cpu, word, pc);
    else if ((word & 0x0f000010u) == 0x0e000010u)
        handled = nds_exec_coprocessor(cpu, word, pc);
    else if ((word & 0x0f000000u) == 0x0f000000u)
        handled = nds_exec_swi(cpu, word & 0x00ffffffu, pc, next);
    else
        handled = false;
    if (handled && cpu->r[15] == pc)
        cpu->r[15] = next;
    return handled;
}

static void run_dma(NdsCpu *cpu, uint32_t control_address, uint32_t control) {
    uint32_t source = nds_read32(cpu, control_address - 8u);
    uint32_t destination = nds_read32(cpu, control_address - 4u);
    uint32_t count = control & UINT32_C(0x001fffff);
    if (count == 0)
        count = UINT32_C(0x00200000);
    const bool word = (control & (UINT32_C(1) << 26)) != 0;
    const uint32_t unit = word ? 4u : 2u;
    const unsigned destination_mode = (control >> 21) & 3u;
    const unsigned source_mode = (control >> 23) & 3u;
    while (count-- != 0) {
        if (word) nds_write32(cpu, destination, nds_read32(cpu, source));
        else nds_write16(cpu, destination, nds_read16(cpu, source));
        if (source_mode == 0) source += unit;
        else if (source_mode == 1) source -= unit;
        if (destination_mode == 0 || destination_mode == 3) destination += unit;
        else if (destination_mode == 1) destination -= unit;
    }
    uint8_t *data = mapped(cpu, control_address);
    if (data != NULL) {
        const uint32_t complete = control & ~UINT32_C(0x80000000);
        data[0] = (uint8_t)complete;
        data[1] = (uint8_t)(complete >> 8);
        data[2] = (uint8_t)(complete >> 16);
        data[3] = (uint8_t)(complete >> 24);
    }
    if (control & (UINT32_C(1) << 30)) {
        const unsigned channel = (control_address - UINT32_C(0x040000b8)) / 12u;
        raise_irq(cpu, UINT32_C(1) << (8u + channel));
    }
}

static uint32_t shifted_operand(const NdsCpu *cpu, uint32_t word, uint32_t pc, bool *carry) {
    if (word & (1u << 25)) {
        const unsigned amount = ((word >> 8) & 15u) * 2u;
        const uint32_t value = rotate_right(word & 255u, amount);
        if (amount != 0)
            *carry = (value >> 31) != 0;
        return value;
    }

    uint32_t value = register_value(cpu, word & 15u, pc);
    const unsigned type = (word >> 5) & 3u;
    unsigned amount = (word & (1u << 4))
        ? (register_value(cpu, (word >> 8) & 15u, pc) & 255u)
        : ((word >> 7) & 31u);
    if ((word & (1u << 4)) && amount == 0)
        return value;
    switch (type) {
    case 0:
        if (amount >= 32) { *carry = amount == 32 && (value & 1u); return 0; }
        if (amount != 0) { *carry = ((value >> (32u - amount)) & 1u) != 0; value <<= amount; }
        break;
    case 1:
        if (!(word & (1u << 4)) && amount == 0) amount = 32;
        if (amount >= 32) { *carry = amount == 32 && ((value >> 31) & 1u); return 0; }
        *carry = ((value >> (amount - 1u)) & 1u) != 0;
        value >>= amount;
        break;
    case 2:
        if (!(word & (1u << 4)) && amount == 0) amount = 32;
        if (amount >= 32) { *carry = (value >> 31) != 0; return *carry ? UINT32_MAX : 0; }
        *carry = ((value >> (amount - 1u)) & 1u) != 0;
        value = (uint32_t)((int32_t)value >> amount);
        break;
    default:
        if (!(word & (1u << 4)) && amount == 0) {
            const bool old_carry = *carry;
            *carry = (value & 1u) != 0;
            return (value >> 1) | (old_carry ? UINT32_C(0x80000000) : 0);
        }
        amount &= 31u;
        if (amount != 0) { value = rotate_right(value, amount); *carry = (value >> 31) != 0; }
        break;
    }
    return value;
}

static void set_nz(NdsCpu *cpu, uint32_t value) {
    cpu->cpsr &= ~(FLAG_N | FLAG_Z);
    if (value == 0) cpu->cpsr |= FLAG_Z;
    if (value & UINT32_C(0x80000000)) cpu->cpsr |= FLAG_N;
}

bool nds_exec_status(NdsCpu *cpu, uint32_t word, uint32_t pc) {
    if ((word & 0x0FBF0FFFu) == 0x010F0000u) { /* MRS Rd, CPSR */
        cpu->r[(word >> 12) & 15u] = word & (1u << 22) ? cpu->spsr : cpu->cpsr;
    } else if ((word & 0x0FB0FFF0u) == 0x0120F000u) { /* MSR CPSR_fields, Rm */
        const uint32_t value = register_value(cpu, word & 15u, pc);
        const unsigned fields = (word >> 16) & 15u;
        uint32_t mask = 0;
        if (fields & 1u) mask |= UINT32_C(0x000000ff);
        if (fields & 2u) mask |= UINT32_C(0x0000ff00);
        if (fields & 4u) mask |= UINT32_C(0x00ff0000);
        if (fields & 8u) mask |= UINT32_C(0xff000000);
        if (word & (1u << 22))
            cpu->spsr = (cpu->spsr & ~mask) | (value & mask);
        else
            replace_cpsr(cpu, (cpu->cpsr & ~mask) | (value & mask));
    } else if ((word & 0x0FB0F000u) == 0x0320F000u) { /* MSR CPSR_fields, #imm */
        const uint32_t value = rotate_right(word & 255u, ((word >> 8) & 15u) * 2u);
        const unsigned fields = (word >> 16) & 15u;
        uint32_t mask = 0;
        if (fields & 1u) mask |= UINT32_C(0x000000ff);
        if (fields & 2u) mask |= UINT32_C(0x0000ff00);
        if (fields & 4u) mask |= UINT32_C(0x00ff0000);
        if (fields & 8u) mask |= UINT32_C(0xff000000);
        if (word & (1u << 22))
            cpu->spsr = (cpu->spsr & ~mask) | (value & mask);
        else
            replace_cpsr(cpu, (cpu->cpsr & ~mask) | (value & mask));
    } else {
        return false;
    }
    cpu->r[15] = pc + 4u;
    return true;
}

void nds_exec_clz(NdsCpu *cpu, uint32_t word, uint32_t pc) {
    uint32_t value = cpu->r[word & 15u];
    uint32_t count = 0;
    while (count < 32u && !(value & UINT32_C(0x80000000))) {
        value <<= 1;
        ++count;
    }
    cpu->r[(word >> 12) & 15u] = count;
    cpu->r[15] = pc + 4u;
}

bool nds_exec_data_processing(NdsCpu *cpu, uint32_t word, uint32_t pc) {
    const unsigned opcode = (word >> 21) & 15u;
    const unsigned rn = (word >> 16) & 15u;
    const unsigned rd = (word >> 12) & 15u;
    const bool set_flags = (word & (1u << 20)) != 0;
    bool carry = (cpu->cpsr & FLAG_C) != 0;
    const bool carry_in = carry;
    const uint32_t a = register_value(cpu, rn, pc);
    const uint32_t b = shifted_operand(cpu, word, pc, &carry);
    uint32_t result = 0;
    bool write_result = true;
    bool arithmetic_flags = false;
    bool overflow = false;
    switch (opcode) {
    case 0x0: result = a & b; break;
    case 0x1: result = a ^ b; break;
    case 0x2: result = a - b; carry = a >= b; overflow = ((a ^ b) & (a ^ result) & UINT32_C(0x80000000)) != 0; arithmetic_flags = true; break;
    case 0x3: result = b - a; carry = b >= a; overflow = ((b ^ a) & (b ^ result) & UINT32_C(0x80000000)) != 0; arithmetic_flags = true; break;
    case 0x4: { const uint64_t wide = (uint64_t)a + b; result = (uint32_t)wide; carry = (wide >> 32) != 0; overflow = (~(a ^ b) & (a ^ result) & UINT32_C(0x80000000)) != 0; arithmetic_flags = true; break; }
    case 0x5: { const unsigned input = carry_in ? 1u : 0u; const uint64_t wide = (uint64_t)a + b + input; const int64_t signed_wide = (int64_t)(int32_t)a + (int32_t)b + input; result = (uint32_t)wide; carry = (wide >> 32) != 0; overflow = signed_wide > INT32_MAX || signed_wide < INT32_MIN; arithmetic_flags = true; break; }
    case 0x6: { const unsigned borrow = carry_in ? 0u : 1u; const uint64_t subtrahend = (uint64_t)b + borrow; const int64_t signed_wide = (int64_t)(int32_t)a - (int32_t)b - borrow; result = a - b - borrow; carry = (uint64_t)a >= subtrahend; overflow = signed_wide > INT32_MAX || signed_wide < INT32_MIN; arithmetic_flags = true; break; }
    case 0x7: { const unsigned borrow = carry_in ? 0u : 1u; const uint64_t subtrahend = (uint64_t)a + borrow; const int64_t signed_wide = (int64_t)(int32_t)b - (int32_t)a - borrow; result = b - a - borrow; carry = (uint64_t)b >= subtrahend; overflow = signed_wide > INT32_MAX || signed_wide < INT32_MIN; arithmetic_flags = true; break; }
    case 0x8: result = a & b; write_result = false; break;
    case 0x9: result = a ^ b; write_result = false; break;
    case 0xA: result = a - b; carry = a >= b; overflow = ((a ^ b) & (a ^ result) & UINT32_C(0x80000000)) != 0; write_result = false; arithmetic_flags = true; break;
    case 0xB: { const uint64_t wide = (uint64_t)a + b; result = (uint32_t)wide; carry = (wide >> 32) != 0; overflow = (~(a ^ b) & (a ^ result) & UINT32_C(0x80000000)) != 0; write_result = false; arithmetic_flags = true; break; }
    case 0xC: result = a | b; break;
    case 0xD: result = b; break;
    case 0xE: result = a & ~b; break;
    case 0xF: result = ~b; break;
    default: return false;
    }
    if (write_result)
        cpu->r[rd] = result;
    if (write_result && rd == 15 && set_flags) {
        replace_cpsr(cpu, cpu->spsr);
    } else if (set_flags || !write_result) {
        set_nz(cpu, result);
        cpu->cpsr = carry ? cpu->cpsr | FLAG_C : cpu->cpsr & ~FLAG_C;
        if (arithmetic_flags)
            cpu->cpsr = overflow ? cpu->cpsr | FLAG_V : cpu->cpsr & ~FLAG_V;
    }
    if (!write_result || rd != 15)
        cpu->r[15] = pc + 4u;
    else
        cpu->r[15] &= ~3u;
    return true;
}

static uint32_t transfer_offset(const NdsCpu *cpu, uint32_t word, uint32_t pc) {
    if (!(word & (1u << 25)))
        return word & 0xfffu;
    bool carry = false;
    return shifted_operand(cpu, word & ~(1u << 25), pc, &carry);
}

bool nds_exec_single_transfer(NdsCpu *cpu, uint32_t word, uint32_t pc) {
    const unsigned rn = (word >> 16) & 15u;
    const unsigned rd = (word >> 12) & 15u;
    const bool pre = (word & (1u << 24)) != 0;
    const bool up = (word & (1u << 23)) != 0;
    const bool byte = (word & (1u << 22)) != 0;
    const bool writeback = (word & (1u << 21)) != 0;
    const bool load = (word & (1u << 20)) != 0;
    const uint32_t base = register_value(cpu, rn, pc);
    const uint32_t offset = transfer_offset(cpu, word, pc);
    const uint32_t adjusted = up ? base + offset : base - offset;
    const uint32_t address = pre ? adjusted : base;
    if (load)
        cpu->r[rd] = byte ? nds_read8(cpu, address) : nds_read32(cpu, address);
    else {
        const uint32_t value = rd == 15 ? pc + 12u : cpu->r[rd];
        if (byte) nds_write8(cpu, address, (uint8_t)value);
        else nds_write32(cpu, address, value);
    }
    if (!pre || writeback)
        cpu->r[rn] = adjusted;
    if (load && rd == 15) {
        const uint32_t target = cpu->r[15];
        thumb_exchange(cpu, target);
    } else
        cpu->r[15] = pc + 4u;
    return true;
}

bool nds_exec_half_transfer(NdsCpu *cpu, uint32_t word, uint32_t pc) {
    const unsigned rn = (word >> 16) & 15u;
    const unsigned rd = (word >> 12) & 15u;
    if ((word & UINT32_C(0x0fb00ff0)) == UINT32_C(0x01000090)) {
        const unsigned rm = word & 15u;
        if (rn == 15u || rd == 15u || rm == 15u)
            return false;
        const uint32_t address = cpu->r[rn];
        const uint32_t old = word & (1u << 22) ? nds_read8(cpu, address)
                                               : nds_read32(cpu, address);
        if (word & (1u << 22))
            nds_write8(cpu, address, (uint8_t)cpu->r[rm]);
        else
            nds_write32(cpu, address, cpu->r[rm]);
        cpu->r[rd] = old;
        cpu->r[15] = pc + 4u;
        return true;
    }
    const bool pre = (word & (1u << 24)) != 0;
    const bool up = (word & (1u << 23)) != 0;
    const bool immediate = (word & (1u << 22)) != 0;
    const bool writeback = (word & (1u << 21)) != 0;
    const bool load = (word & (1u << 20)) != 0;
    const unsigned kind = (word >> 5) & 3u;
    const uint32_t offset = immediate ? (((word >> 4) & 0xf0u) | (word & 15u)) : cpu->r[word & 15u];
    const uint32_t base = register_value(cpu, rn, pc);
    const uint32_t adjusted = up ? base + offset : base - offset;
    const uint32_t address = pre ? adjusted : base;
    if (load) {
        if (kind == 1) cpu->r[rd] = nds_read16(cpu, address);
        else if (kind == 2) cpu->r[rd] = (uint32_t)(int32_t)(int8_t)nds_read8(cpu, address);
        else if (kind == 3) cpu->r[rd] = (uint32_t)(int32_t)(int16_t)nds_read16(cpu, address);
        else return false;
    } else {
        if (kind != 1) return false;
        nds_write16(cpu, address, (uint16_t)cpu->r[rd]);
    }
    if (!pre || writeback) cpu->r[rn] = adjusted;
    cpu->r[15] = (load && rd == 15) ? (cpu->r[15] & ~3u) : pc + 4u;
    return true;
}

bool nds_exec_block_transfer(NdsCpu *cpu, uint32_t word, uint32_t pc) {
    const unsigned rn = (word >> 16) & 15u;
    const uint32_t list = word & 0xffffu;
    const bool pre = (word & (1u << 24)) != 0;
    const bool up = (word & (1u << 23)) != 0;
    const bool restore_status = (word & (1u << 22)) != 0 &&
                                (word & (1u << 20)) != 0 && (list & 0x8000u);
    const bool user_bank = (word & (1u << 22)) != 0 && !restore_status;
    const bool writeback = (word & (1u << 21)) != 0;
    const bool load = (word & (1u << 20)) != 0;
    unsigned count = 0;
    for (unsigned reg = 0; reg < 16; ++reg) count += (list >> reg) & 1u;
    if (count == 0) return false;
    const uint32_t base = register_value(cpu, rn, pc);
    uint32_t address = up ? base + (pre ? 4u : 0u) : base - count * 4u + (pre ? 0u : 4u);
    for (unsigned reg = 0; reg < 16; ++reg) {
        if (!(list & (1u << reg))) continue;
        if (load) {
            const uint32_t value = nds_read32(cpu, address);
            if (user_bank && reg == 13) cpu->banked_r13[0x10] = value;
            else if (user_bank && reg == 14) cpu->banked_r14[0x10] = value;
            else cpu->r[reg] = value;
        } else {
            uint32_t value = reg == 15 ? pc + 12u : cpu->r[reg];
            if (user_bank && reg == 13) value = cpu->banked_r13[0x10];
            else if (user_bank && reg == 14) value = cpu->banked_r14[0x10];
            nds_write32(cpu, address, value);
        }
        address += 4u;
    }
    if (writeback) cpu->r[rn] = up ? base + count * 4u : base - count * 4u;
    if (load && (list & 0x8000u)) {
        const uint32_t target = cpu->r[15];
        if (restore_status) {
            const uint32_t saved_status = cpu->spsr;
            replace_cpsr(cpu, saved_status);
            cpu->r[15] = saved_status & FLAG_T ? target & ~1u : target & ~3u;
        } else {
            thumb_exchange(cpu, target);
        }
    } else {
        cpu->r[15] = pc + 4u;
    }
    return true;
}

bool nds_exec_multiply(NdsCpu *cpu, uint32_t word, uint32_t pc) {
    const unsigned rd = (word >> 16) & 15u;
    const unsigned rn = (word >> 12) & 15u;
    const unsigned rs = (word >> 8) & 15u;
    const unsigned rm = word & 15u;
    uint32_t value = cpu->r[rm] * cpu->r[rs];
    if (word & (1u << 21)) value += cpu->r[rn];
    cpu->r[rd] = value;
    if (word & (1u << 20)) set_nz(cpu, value);
    cpu->r[15] = pc + 4u;
    return rd != 15;
}

bool nds_exec_long_multiply(NdsCpu *cpu, uint32_t word, uint32_t pc) {
    const unsigned high = (word >> 16) & 15u;
    const unsigned low = (word >> 12) & 15u;
    const unsigned rs = (word >> 8) & 15u;
    const unsigned rm = word & 15u;
    uint64_t value;
    if (word & (1u << 22))
        value = (uint64_t)((int64_t)(int32_t)cpu->r[rm] * (int32_t)cpu->r[rs]);
    else
        value = (uint64_t)cpu->r[rm] * cpu->r[rs];
    if (word & (1u << 21))
        value += ((uint64_t)cpu->r[high] << 32) | cpu->r[low];
    cpu->r[low] = (uint32_t)value;
    cpu->r[high] = (uint32_t)(value >> 32);
    if (word & (1u << 20)) {
        cpu->cpsr &= ~(FLAG_N | FLAG_Z);
        if (value == 0) cpu->cpsr |= FLAG_Z;
        if (value >> 63) cpu->cpsr |= FLAG_N;
    }
    cpu->r[15] = pc + 4u;
    return high != 15 && low != 15;
}

bool nds_exec_coprocessor(NdsCpu *cpu, uint32_t word, uint32_t pc) {
    const unsigned cp = (word >> 8) & 15u;
    const unsigned rd = (word >> 12) & 15u;
    const unsigned crn = (word >> 16) & 15u;
    const unsigned crm = word & 15u;
    const unsigned opcode2 = (word >> 5) & 7u;
    if (cp != 15) return false;
    if (word & (1u << 20)) {
        if (crn == 1u && crm == 0u && opcode2 == 0u)
            cpu->r[rd] = cpu->cp15_control;
        else if (crn == 9u && crm == 1u && opcode2 == 0u)
            cpu->r[rd] = cpu->dtcm_control;
        else
            cpu->r[rd] = 0u;
    } else if (crn == 1u && crm == 0u && opcode2 == 0u) {
        cpu->cp15_control = cpu->r[rd];
    } else if (crn == 9u && crm == 1u && opcode2 == 0u) {
        cpu->dtcm_control = cpu->r[rd];
        cpu->dtcm_base = cpu->dtcm_control & UINT32_C(0xfffff000);
    } else if ((word & UINT32_C(0x0fff0fff)) == UINT32_C(0x0e070f90))
        cpu->halted = true; /* MCR p15, 0, Rd, c7, c0, 4: wait for interrupt. */
    cpu->r[15] = pc + 4u;
    return rd != 15;
}

static uint32_t bios_sine_table(uint32_t index) {
    if (index > 0x3fu)
        index = 0x3fu;
    const double value = sin((double)index * 3.14159265358979323846 / 128.0) * 32768.0;
    const uint32_t rounded = (uint32_t)(value + 0.5);
    return rounded > 0x7fffu ? 0x7fffu : rounded;
}

static uint32_t bios_pitch_table(uint32_t index) {
    if (index > 0x2ffu)
        index = 0x2ffu;
    const double value = (pow(2.0, (double)index / 768.0) - 1.0) * 65536.0;
    return (uint32_t)(value + 0.5) & 0xffffu;
}

static uint32_t bios_volume_table(uint32_t index) {
    if (index == 0u)
        return 0u;
    if (index > 0x2d3u)
        index = 0x2d3u;

    double value;
    if (index <= 482u)
        value = 127.0 * pow(10.0, ((double)index - 482.0) / 200.0);
    else if (index <= 602u)
        value = 32.0 * pow(10.0, ((double)index - 483.0) / 200.0);
    else if (index <= 662u)
        value = 64.0 * pow(10.0, ((double)index - 603.0) / 200.0);
    else
        value = 64.0 * pow(10.0, ((double)index - 663.0) / 200.0);
    uint32_t rounded = (uint32_t)(value + 0.5);
    if (rounded == 0u)
        rounded = 1u;
    return rounded > 0x7fu ? 0x7fu : rounded;
}

bool nds_exec_swi(NdsCpu *cpu, unsigned immediate, uint32_t pc, uint32_t next_pc) {
    (void)pc;
    if (immediate == 3u) {
        cpu->wait_cycles = cpu->r[0];
        uint8_t *mailbox = mapped(cpu, SDK_PROBE_MAILBOX);
        if (mailbox != NULL && mailbox[2] != 0) {
            mailbox[0] = (uint8_t)(mailbox[0] + 1u);
            mailbox[3] = 1u;
        }
    } else if (immediate == 6u || immediate == 7u) {
        cpu->halted = true;
    } else if (immediate == 8u && cpu->cpu_id == 7u) {
        /* ARM7 sound-bias ramping is timing-only for the host audio model. */
    } else if (immediate == 9u) {
        const int32_t numerator = (int32_t)cpu->r[0];
        const int32_t denominator = (int32_t)cpu->r[1];
        const int64_t quotient = denominator == 0 ? (numerator < 0 ? 1 : -1)
                                                  : (int64_t)numerator / denominator;
        const int64_t remainder = denominator == 0 ? numerator
                                                   : (int64_t)numerator % denominator;
        cpu->r[0] = (uint32_t)quotient;
        cpu->r[1] = (uint32_t)remainder;
        cpu->r[3] = quotient < 0 ? (uint32_t)-quotient : (uint32_t)quotient;
    } else if (immediate == 0x0bu || immediate == 0x0cu) {
        uint32_t source = cpu->r[0];
        uint32_t destination = cpu->r[1];
        const uint32_t control = cpu->r[2];
        uint32_t count = control & UINT32_C(0x001fffff);
        const bool fixed = (control & (UINT32_C(1) << 24)) != 0;
        const bool word = immediate == 0x0cu || (control & (UINT32_C(1) << 26)) != 0;
        if (immediate == 0x0cu)
            count &= ~7u;
        if (word) {
            const uint32_t fill = nds_read32(cpu, source);
            while (count-- != 0) {
                nds_write32(cpu, destination, fixed ? fill : nds_read32(cpu, source));
                if (!fixed) source += 4u;
                destination += 4u;
            }
        } else {
            const uint16_t fill = nds_read16(cpu, source);
            while (count-- != 0) {
                nds_write16(cpu, destination, fixed ? fill : nds_read16(cpu, source));
                if (!fixed) source += 2u;
                destination += 2u;
            }
        }
    } else if (immediate == 0x0du) {
        uint32_t value = cpu->r[0];
        uint32_t root = 0;
        uint32_t bit = UINT32_C(1) << 30;
        while (bit > value) bit >>= 2;
        while (bit != 0) {
            if (value >= root + bit) {
                value -= root + bit;
                root = (root >> 1) + bit;
            } else {
                root >>= 1;
            }
            bit >>= 2;
        }
        cpu->r[0] = root;
    } else if (immediate == 0x0eu) {
        uint16_t crc = (uint16_t)cpu->r[0];
        uint32_t source = cpu->r[1];
        uint32_t size = cpu->r[2];
        while (size-- != 0u) {
            crc ^= nds_read8(cpu, source++);
            for (unsigned bit = 0; bit < 8u; ++bit)
                crc = (uint16_t)((crc >> 1) ^ (crc & 1u ? 0xa001u : 0u));
        }
        cpu->r[0] = crc;
    } else if (immediate == 0x0fu) {
        cpu->r[0] = 1u;
    } else if (immediate == 0x1au && cpu->cpu_id == 7u) {
        cpu->r[0] = bios_sine_table(cpu->r[0]);
    } else if (immediate == 0x1bu && cpu->cpu_id == 7u) {
        cpu->r[0] = bios_pitch_table(cpu->r[0]);
    } else if (immediate == 0x1cu && cpu->cpu_id == 7u) {
        cpu->r[0] = bios_volume_table(cpu->r[0]);
    } else {
        return false;
    }
    cpu->r[15] = next_pc;
    return true;
}

static void set_add_flags(NdsCpu *cpu, uint32_t a, uint32_t b, uint32_t carry_in, uint32_t result) {
    const uint64_t wide = (uint64_t)a + b + carry_in;
    const int64_t signed_wide = (int64_t)(int32_t)a + (int32_t)b + carry_in;
    set_nz(cpu, result);
    cpu->cpsr = wide >> 32 ? cpu->cpsr | FLAG_C : cpu->cpsr & ~FLAG_C;
    cpu->cpsr = signed_wide > INT32_MAX || signed_wide < INT32_MIN
        ? cpu->cpsr | FLAG_V : cpu->cpsr & ~FLAG_V;
}

static void set_sub_flags(NdsCpu *cpu, uint32_t a, uint32_t b, uint32_t borrow, uint32_t result) {
    const uint64_t subtrahend = (uint64_t)b + borrow;
    const int64_t signed_wide = (int64_t)(int32_t)a - (int32_t)b - borrow;
    set_nz(cpu, result);
    cpu->cpsr = (uint64_t)a >= subtrahend ? cpu->cpsr | FLAG_C : cpu->cpsr & ~FLAG_C;
    cpu->cpsr = signed_wide > INT32_MAX || signed_wide < INT32_MIN
        ? cpu->cpsr | FLAG_V : cpu->cpsr & ~FLAG_V;
}

static uint32_t thumb_shift(NdsCpu *cpu, uint32_t value, unsigned kind, unsigned amount, bool immediate) {
    bool carry = (cpu->cpsr & FLAG_C) != 0;
    if (!immediate && amount == 0)
        return value;
    if (kind == 0) {
        if (amount >= 32) { carry = amount == 32 && (value & 1u); value = 0; }
        else if (amount != 0) { carry = (value >> (32u - amount)) & 1u; value <<= amount; }
    } else if (kind == 1) {
        if (immediate && amount == 0) amount = 32;
        if (amount >= 32) { carry = amount == 32 && (value >> 31); value = 0; }
        else { carry = (value >> (amount - 1u)) & 1u; value >>= amount; }
    } else if (kind == 2) {
        if (immediate && amount == 0) amount = 32;
        if (amount >= 32) { carry = value >> 31; value = carry ? UINT32_MAX : 0; }
        else { carry = (value >> (amount - 1u)) & 1u; value = (uint32_t)((int32_t)value >> amount); }
    } else {
        amount &= 31u;
        if (amount != 0) { value = rotate_right(value, amount); carry = value >> 31; }
    }
    cpu->cpsr = carry ? cpu->cpsr | FLAG_C : cpu->cpsr & ~FLAG_C;
    return value;
}

static void thumb_exchange(NdsCpu *cpu, uint32_t target) {
    if (target & 1u) {
        cpu->cpsr |= FLAG_T;
        cpu->r[15] = target & ~1u;
    } else {
        cpu->cpsr &= ~FLAG_T;
        cpu->r[15] = target & ~3u;
    }
}

bool nds_exec_thumb(NdsCpu *cpu, uint16_t half, uint32_t pc) {
    const uint32_t next = pc + 2u;
    if ((half & 0xf800u) < 0x1800u) { /* LSL/LSR/ASR immediate */
        const unsigned kind = (half >> 11) & 3u;
        const unsigned amount = (half >> 6) & 31u;
        const unsigned rs = (half >> 3) & 7u;
        const unsigned rd = half & 7u;
        cpu->r[rd] = thumb_shift(cpu, cpu->r[rs], kind, amount, true);
        set_nz(cpu, cpu->r[rd]);
    } else if ((half & 0xf800u) == 0x1800u) { /* ADD/SUB register/immediate */
        const unsigned rd = half & 7u;
        const uint32_t a = cpu->r[(half >> 3) & 7u];
        const uint32_t b = half & 0x0400u ? (half >> 6) & 7u : cpu->r[(half >> 6) & 7u];
        if (half & 0x0200u) {
            cpu->r[rd] = a - b;
            set_sub_flags(cpu, a, b, 0, cpu->r[rd]);
        } else {
            cpu->r[rd] = a + b;
            set_add_flags(cpu, a, b, 0, cpu->r[rd]);
        }
    } else if ((half & 0xe000u) == 0x2000u) { /* MOV/CMP/ADD/SUB immediate */
        const unsigned operation = (half >> 11) & 3u;
        const unsigned rd = (half >> 8) & 7u;
        const uint32_t immediate = half & 255u;
        const uint32_t old = cpu->r[rd];
        if (operation == 0) { cpu->r[rd] = immediate; set_nz(cpu, immediate); }
        else if (operation == 1) { const uint32_t value = old - immediate; set_sub_flags(cpu, old, immediate, 0, value); }
        else if (operation == 2) { cpu->r[rd] = old + immediate; set_add_flags(cpu, old, immediate, 0, cpu->r[rd]); }
        else { cpu->r[rd] = old - immediate; set_sub_flags(cpu, old, immediate, 0, cpu->r[rd]); }
    } else if ((half & 0xfc00u) == 0x4000u) { /* ALU operations */
        const unsigned operation = (half >> 6) & 15u;
        const unsigned rs = (half >> 3) & 7u;
        const unsigned rd = half & 7u;
        const uint32_t a = cpu->r[rd];
        const uint32_t b = cpu->r[rs];
        uint32_t value;
        switch (operation) {
        case 0: cpu->r[rd] = a & b; set_nz(cpu, cpu->r[rd]); break;
        case 1: cpu->r[rd] = a ^ b; set_nz(cpu, cpu->r[rd]); break;
        case 2: cpu->r[rd] = thumb_shift(cpu, a, 0, b & 255u, false); set_nz(cpu, cpu->r[rd]); break;
        case 3: cpu->r[rd] = thumb_shift(cpu, a, 1, b & 255u, false); set_nz(cpu, cpu->r[rd]); break;
        case 4: cpu->r[rd] = thumb_shift(cpu, a, 2, b & 255u, false); set_nz(cpu, cpu->r[rd]); break;
        case 5: { const uint32_t carry = (cpu->cpsr & FLAG_C) != 0; cpu->r[rd] = a + b + carry; set_add_flags(cpu, a, b, carry, cpu->r[rd]); break; }
        case 6: { const uint32_t borrow = (cpu->cpsr & FLAG_C) == 0; cpu->r[rd] = a - b - borrow; set_sub_flags(cpu, a, b, borrow, cpu->r[rd]); break; }
        case 7: cpu->r[rd] = thumb_shift(cpu, a, 3, b & 255u, false); set_nz(cpu, cpu->r[rd]); break;
        case 8: set_nz(cpu, a & b); break;
        case 9: value = 0u - b; cpu->r[rd] = value; set_sub_flags(cpu, 0, b, 0, value); break;
        case 10: value = a - b; set_sub_flags(cpu, a, b, 0, value); break;
        case 11: value = a + b; set_add_flags(cpu, a, b, 0, value); break;
        case 12: cpu->r[rd] = a | b; set_nz(cpu, cpu->r[rd]); break;
        case 13: cpu->r[rd] = a * b; set_nz(cpu, cpu->r[rd]); break;
        case 14: cpu->r[rd] = a & ~b; set_nz(cpu, cpu->r[rd]); break;
        default: cpu->r[rd] = ~b; set_nz(cpu, cpu->r[rd]); break;
        }
    } else if ((half & 0xfc00u) == 0x4400u) { /* High-register ops and BX/BLX */
        const unsigned operation = (half >> 8) & 3u;
        const unsigned rs = ((half >> 3) & 7u) | ((half >> 3) & 8u);
        const unsigned rd = (half & 7u) | ((half >> 4) & 8u);
        const uint32_t source = rs == 15 ? (pc + 4u) & ~1u : cpu->r[rs];
        if (operation == 0) {
            const uint32_t value = (rd == 15 ? (pc + 4u) & ~1u : cpu->r[rd]) + source;
            if (rd == 15) { cpu->r[15] = value & ~1u; return true; }
            cpu->r[rd] = value;
        } else if (operation == 1) {
            const uint32_t a = rd == 15 ? (pc + 4u) & ~1u : cpu->r[rd];
            set_sub_flags(cpu, a, source, 0, a - source);
        } else if (operation == 2) {
            if (rd == 15) { cpu->r[15] = source & ~1u; return true; }
            cpu->r[rd] = source;
        } else {
            if (half & 0x0080u) cpu->r[14] = next | 1u;
            thumb_exchange(cpu, source);
            return true;
        }
    } else if ((half & 0xf800u) == 0x4800u) { /* LDR literal */
        const unsigned rd = (half >> 8) & 7u;
        cpu->r[rd] = nds_read32(cpu, ((pc + 4u) & ~3u) + (half & 255u) * 4u);
    } else if ((half & 0xf000u) == 0x5000u) { /* Register-offset transfers */
        const unsigned operation = (half >> 9) & 7u;
        const unsigned ro = (half >> 6) & 7u;
        const unsigned rb = (half >> 3) & 7u;
        const unsigned rd = half & 7u;
        const uint32_t address = cpu->r[rb] + cpu->r[ro];
        if (operation == 0) nds_write32(cpu, address, cpu->r[rd]);
        else if (operation == 1) nds_write16(cpu, address, (uint16_t)cpu->r[rd]);
        else if (operation == 2) nds_write8(cpu, address, (uint8_t)cpu->r[rd]);
        else if (operation == 3) cpu->r[rd] = (uint32_t)(int32_t)(int8_t)nds_read8(cpu, address);
        else if (operation == 4) cpu->r[rd] = nds_read32(cpu, address);
        else if (operation == 5) cpu->r[rd] = nds_read16(cpu, address);
        else if (operation == 6) cpu->r[rd] = nds_read8(cpu, address);
        else cpu->r[rd] = (uint32_t)(int32_t)(int16_t)nds_read16(cpu, address);
    } else if ((half & 0xe000u) == 0x6000u) { /* Immediate word/byte transfers */
        const bool byte = (half & 0x1000u) != 0;
        const bool load = (half & 0x0800u) != 0;
        const unsigned rb = (half >> 3) & 7u;
        const unsigned rd = half & 7u;
        uint32_t offset = (half >> 6) & 31u;
        if (!byte) offset *= 4u;
        const uint32_t address = cpu->r[rb] + offset;
        if (load) cpu->r[rd] = byte ? nds_read8(cpu, address) : nds_read32(cpu, address);
        else if (byte) nds_write8(cpu, address, (uint8_t)cpu->r[rd]);
        else nds_write32(cpu, address, cpu->r[rd]);
    } else if ((half & 0xf000u) == 0x8000u) { /* Halfword transfer */
        const bool load = (half & 0x0800u) != 0;
        const unsigned rb = (half >> 3) & 7u;
        const unsigned rd = half & 7u;
        const uint32_t address = cpu->r[rb] + ((half >> 6) & 31u) * 2u;
        if (load) cpu->r[rd] = nds_read16(cpu, address);
        else nds_write16(cpu, address, (uint16_t)cpu->r[rd]);
    } else if ((half & 0xf000u) == 0x9000u) { /* SP-relative transfer */
        const bool load = (half & 0x0800u) != 0;
        const unsigned rd = (half >> 8) & 7u;
        const uint32_t address = cpu->r[13] + (half & 255u) * 4u;
        if (load) cpu->r[rd] = nds_read32(cpu, address);
        else nds_write32(cpu, address, cpu->r[rd]);
    } else if ((half & 0xf000u) == 0xa000u) { /* ADD Rd, PC/SP, #imm */
        const unsigned rd = (half >> 8) & 7u;
        const uint32_t base = half & 0x0800u ? cpu->r[13] : (pc + 4u) & ~3u;
        cpu->r[rd] = base + (half & 255u) * 4u;
    } else if ((half & 0xff00u) == 0xb000u) { /* ADD/SUB SP */
        const uint32_t amount = (half & 0x7fu) * 4u;
        cpu->r[13] = half & 0x80u ? cpu->r[13] - amount : cpu->r[13] + amount;
    } else if ((half & 0xf600u) == 0xb400u) { /* PUSH/POP */
        const bool load = (half & 0x0800u) != 0;
        const uint32_t list = half & 255u;
        unsigned count = (half & 0x0100u) != 0;
        for (unsigned reg = 0; reg < 8; ++reg) count += (list >> reg) & 1u;
        if (!load) {
            uint32_t address = cpu->r[13] - count * 4u;
            cpu->r[13] = address;
            for (unsigned reg = 0; reg < 8; ++reg) if (list & (1u << reg)) { nds_write32(cpu, address, cpu->r[reg]); address += 4u; }
            if (half & 0x0100u) nds_write32(cpu, address, cpu->r[14]);
        } else {
            uint32_t address = cpu->r[13];
            for (unsigned reg = 0; reg < 8; ++reg) if (list & (1u << reg)) { cpu->r[reg] = nds_read32(cpu, address); address += 4u; }
            if (half & 0x0100u) { const uint32_t target = nds_read32(cpu, address); address += 4u; cpu->r[13] = address; thumb_exchange(cpu, target); return true; }
            cpu->r[13] = address;
        }
    } else if ((half & 0xf000u) == 0xc000u) { /* STMIA/LDMIA */
        const bool load = (half & 0x0800u) != 0;
        const unsigned rb = (half >> 8) & 7u;
        const uint32_t list = half & 255u;
        uint32_t address = cpu->r[rb];
        for (unsigned reg = 0; reg < 8; ++reg) if (list & (1u << reg)) {
            if (load) cpu->r[reg] = nds_read32(cpu, address); else nds_write32(cpu, address, cpu->r[reg]);
            address += 4u;
        }
        cpu->r[rb] = address;
    } else if ((half & 0xf000u) == 0xd000u) { /* Conditional branch / SWI */
        const unsigned condition = (half >> 8) & 15u;
        if (condition == 0xfu)
            return nds_exec_swi(cpu, half & 255u, pc, next);
        if (condition == 0xeu) return false;
        if (nds_condition(cpu, condition)) {
            cpu->r[15] = pc + 4u + (uint32_t)((int32_t)(int8_t)half * 2);
            return true;
        }
    } else if ((half & 0xf800u) == 0xf000u) { /* BL prefix */
        cpu->r[14] = pc + 4u + (uint32_t)(sign_extend32(half & 0x7ffu, 11) * 4096);
    } else if ((half & 0xf800u) == 0xf800u || (half & 0xf800u) == 0xe800u) { /* BL/BLX suffix */
        const uint32_t target = cpu->r[14] + ((half & 0x7ffu) << 1);
        cpu->r[14] = next | 1u;
        if ((half & 0xf800u) == 0xe800u) thumb_exchange(cpu, target & ~3u);
        else thumb_exchange(cpu, target | 1u);
        return true;
    } else if ((half & 0xf800u) == 0xe000u) { /* Unconditional branch */
        cpu->r[15] = pc + 4u + (uint32_t)(sign_extend32(half & 0x7ffu, 11) * 2);
        return true;
    } else {
        return false;
    }
    cpu->r[15] = next;
    return true;
}

bool nds_branch_exchange(NdsCpu *cpu, unsigned rm, bool link, uint32_t pc) {
    const uint32_t target = register_value(cpu, rm, pc);
    if (link)
        cpu->r[14] = pc + 4u;
    thumb_exchange(cpu, target);
    return true;
}

void nds_branch_link_exchange_immediate(NdsCpu *cpu, uint32_t target, uint32_t return_address) {
    cpu->r[14] = return_address;
    thumb_exchange(cpu, target | 1u);
}
