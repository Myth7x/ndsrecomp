#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct NdsGpuTag NdsGpu;

typedef enum {
    NDS_RUN_BUDGET_EXHAUSTED,
    NDS_RUN_OUTSIDE_TRANSLATION,
    NDS_RUN_CODE_MISMATCH,
    NDS_RUN_UNSUPPORTED,
    NDS_RUN_MEMORY_ERROR
} NdsRunResult;

typedef struct NdsCpuTag {
    uint32_t r[16];
    uint32_t cpsr;
    uint32_t spsr;
    uint32_t banked_r13[32];
    uint32_t banked_r14[32];
    uint32_t banked_spsr[32];
    uint32_t dtcm_base;
    uint32_t dtcm_control;
    uint32_t cp15_control;
    uint32_t trap_pc;
    uint32_t trap_word;
    uint32_t last_pc;
    uint32_t previous_pc;
    uint32_t history[16];
    uint32_t history_index;
    uint64_t instructions_executed;
    uint32_t fifo_word;
    bool fifo_pending;
    uint32_t fifo_queue[16];
    unsigned fifo_head;
    unsigned fifo_tail;
    unsigned fifo_count;
    uint32_t fifo_sent;
    uint32_t fifo_received;
    uint32_t wait_cycles;
    uint32_t tick_count;
    bool reschedule;
    uint32_t timer_accum[4];
    uint16_t timer_reload[4];
    uint32_t last_irq_handler;
    uint32_t irq_count;
    uint32_t irq_completed;
    uint32_t irq_sources;
    void *rom_file;
    uint32_t card_address;
    uint32_t card_remaining;
    uint16_t touch_x;
    uint16_t touch_y;
    uint16_t touch_result;
    uint8_t touch_phase;
    uint8_t firmware_command;
    uint32_t firmware_position;
    uint32_t firmware_address;
    bool firmware_selected;
    uint8_t firmware_user[256];
    uint8_t backup_command;
    uint8_t backup_status;
    uint32_t backup_position;
    uint32_t backup_address;
    bool backup_selected;
    bool backup_dirty;
    unsigned cpu_id;
    bool halted;
    bool owns_shared_memory;
    bool owns_save_memory;
    struct NdsCpuTag *peer;
    NdsGpu *gpu;
    uint8_t *itcm;
    uint8_t *dtcm;
    uint8_t *main_ram;
    uint8_t *io;
    uint8_t *palette;
    uint8_t *vram;
    uint8_t *oam;
    uint16_t *display_fifo;
    size_t display_fifo_position;
    uint8_t *wram;
    uint8_t *save_data;
    size_t save_size;
    char *save_path;
} NdsCpu;

bool nds_cpu_init_arm9(NdsCpu *cpu, const char *rom_path);
bool nds_cpu_init_arm7(NdsCpu *cpu, NdsCpu *arm9);
void nds_cpu_link(NdsCpu *arm9, NdsCpu *arm7);
void nds_set_touch(NdsCpu *cpu, bool touching, uint16_t x, uint16_t y);
void nds_tick(NdsCpu *cpu, uint32_t cycles);
void nds_cpu_destroy(NdsCpu *cpu);
NdsRunResult nds_cpu_trap(NdsCpu *cpu, NdsRunResult result, uint32_t pc, uint32_t word);
bool nds_condition(const NdsCpu *cpu, unsigned condition);
bool nds_cpu_is_thumb(const NdsCpu *cpu);
void nds_poll_interrupts(NdsCpu *cpu);
bool nds_finish_interrupt(NdsCpu *cpu);
const uint8_t *nds_vram_bank_pointer(const NdsCpu *cpu, unsigned bank,
                                     uint32_t address);
uint8_t nds_read8(const NdsCpu *cpu, uint32_t address);
uint16_t nds_read16(const NdsCpu *cpu, uint32_t address);
uint32_t nds_read32(const NdsCpu *cpu, uint32_t address);
void nds_write8(NdsCpu *cpu, uint32_t address, uint8_t value);
void nds_write16(NdsCpu *cpu, uint32_t address, uint16_t value);
void nds_write32(NdsCpu *cpu, uint32_t address, uint32_t value);
bool nds_branch_exchange(NdsCpu *cpu, unsigned rm, bool link, uint32_t pc);
void nds_branch_link_exchange_immediate(NdsCpu *cpu, uint32_t target, uint32_t return_address);
bool nds_exec_arm(NdsCpu *cpu, uint32_t word, uint32_t pc);
bool nds_exec_status(NdsCpu *cpu, uint32_t word, uint32_t pc);
void nds_exec_clz(NdsCpu *cpu, uint32_t word, uint32_t pc);
bool nds_exec_data_processing(NdsCpu *cpu, uint32_t word, uint32_t pc);
bool nds_exec_single_transfer(NdsCpu *cpu, uint32_t word, uint32_t pc);
bool nds_exec_half_transfer(NdsCpu *cpu, uint32_t word, uint32_t pc);
bool nds_exec_block_transfer(NdsCpu *cpu, uint32_t word, uint32_t pc);
bool nds_exec_multiply(NdsCpu *cpu, uint32_t word, uint32_t pc);
bool nds_exec_long_multiply(NdsCpu *cpu, uint32_t word, uint32_t pc);
bool nds_exec_coprocessor(NdsCpu *cpu, uint32_t word, uint32_t pc);
bool nds_exec_swi(NdsCpu *cpu, unsigned immediate, uint32_t pc, uint32_t next_pc);
bool nds_exec_thumb(NdsCpu *cpu, uint16_t half, uint32_t pc);
