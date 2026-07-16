#include "arm9_recomp.h"
#include "easygl2d.h"
#include "nds_video.h"
#include "rom_config.h"

#include <SDL3/SDL.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *result_name(NdsRunResult result) {
    switch (result) {
    case NDS_RUN_BUDGET_EXHAUSTED: return "budget exhausted";
    case NDS_RUN_OUTSIDE_TRANSLATION: return "outside translation";
    case NDS_RUN_CODE_MISMATCH: return "code mismatch";
    case NDS_RUN_UNSUPPORTED: return "unsupported instruction";
    case NDS_RUN_MEMORY_ERROR: return "memory error";
    }
    return "unknown";
}

static bool run_steps(NdsCpu *cpu, NdsCpu *arm7, uint32_t count,
                      NdsRunResult *result, NdsRunResult *arm7_result) {
    for (uint32_t step = 0; step < count; ++step) {
        nds_tick(cpu, 2u);
        nds_tick(arm7, 1u);
        if (cpu->wait_cycles != 0u) {
            cpu->wait_cycles--;
            nds_poll_interrupts(cpu);
        } else if (cpu->halted) {
            nds_poll_interrupts(cpu);
        } else {
            *result = nds_run_arm9(cpu, 1u);
            if (*result != NDS_RUN_BUDGET_EXHAUSTED)
                return false;
        }
        if (arm7->wait_cycles != 0u) {
            arm7->wait_cycles--;
            nds_poll_interrupts(arm7);
        } else if (arm7->halted) {
            nds_poll_interrupts(arm7);
        } else {
            *arm7_result = nds_run_arm7(arm7, 1u);
            if (*arm7_result != NDS_RUN_BUDGET_EXHAUSTED)
                return false;
        }
    }
    return true;
}

int main(int argc, char **argv) {
    bool self_test = false;
    const char *rom_path = NULL;
    uint32_t step_limit = 10000000u;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--self-test") == 0) {
            self_test = true;
        } else if (strcmp(argv[index], "--rom") == 0 && index + 1 < argc) {
            rom_path = argv[++index];
        } else if (strcmp(argv[index], "--steps") == 0 && index + 1 < argc) {
            char *end = NULL;
            const unsigned long parsed = strtoul(argv[++index], &end, 10);
            if (end == argv[index] || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
                fprintf(stderr, "invalid --steps value: %s\n", argv[index]);
                return 2;
            }
            step_limit = (uint32_t)parsed;
        } else {
            fprintf(stderr, "usage: %s [--self-test] [--rom path] [--steps count]\n", argv[0]);
            return 2;
        }
    }
    char default_rom[4096];
    if (rom_path == NULL) {
        const char *slash = strrchr(argv[0], '/');
        const char *backslash = strrchr(argv[0], '\\');
        if (backslash != NULL && (slash == NULL || backslash > slash))
            slash = backslash;
        const int directory_length = slash == NULL ? 1 : (int)(slash - argv[0]);
        const char *directory = slash == NULL ? "." : argv[0];
        snprintf(default_rom, sizeof(default_rom), "%.*s/../rom/game.nds",
                 directory_length, directory);
        rom_path = default_rom;
    }
    char title[96];
    snprintf(title, sizeof(title), "ndsrecomp %s - %s", NDS_GAME_CODE, NDS_ROM_TITLE);
    if (!easygl2d_init(title, 3, self_test)) {
        fprintf(stderr, "easyGL2D initialization failed: %s\n", SDL_GetError());
        return 1;
    }

    NdsCpu cpu;
    if (!nds_cpu_init_arm9(&cpu, rom_path)) {
        fprintf(stderr, "failed to initialize ARM9 memory or open %s\n", rom_path);
        easygl2d_shutdown();
        return 1;
    }
    NdsCpu arm7;
    if (!nds_cpu_init_arm7(&arm7, &cpu)) {
        fputs("failed to initialize ARM7 memory\n", stderr);
        nds_cpu_destroy(&cpu);
        easygl2d_shutdown();
        return 1;
    }
    nds_cpu_link(&cpu, &arm7);

    NdsRunResult result = NDS_RUN_BUDGET_EXHAUSTED;
    NdsRunResult arm7_result = NDS_RUN_BUDGET_EXHAUSTED;
    bool running = run_steps(&cpu, &arm7, step_limit, &result, &arm7_result);
    nds_video_render(&cpu);
    easygl2d_present();
    printf("%s (%s): %u ARM9 instructions translated\n", NDS_GAME_CODE,
           NDS_ROM_TITLE, nds_arm9_translated_instruction_count);
    printf("ARM7: %u instructions translated; run: %s, pc=%08" PRIx32
           ", opcode=%08" PRIx32 "\n", nds_arm7_translated_instruction_count,
           result_name(arm7_result), arm7.trap_pc, arm7.trap_word);
    printf("ARM7 flow: previous=%08" PRIx32 " last=%08" PRIx32
           " r0=%08" PRIx32 " r1=%08" PRIx32 " sp=%08" PRIx32
           " lr=%08" PRIx32 "\n", arm7.previous_pc, arm7.last_pc,
           arm7.r[0], arm7.r[1], arm7.r[13], arm7.r[14]);
    fputs("ARM7 history:", stdout);
    const uint32_t arm7_history_start = arm7.history_index > 16u ? arm7.history_index - 16u : 0u;
    for (uint32_t index = arm7_history_start; index < arm7.history_index; ++index)
        printf(" %08" PRIx32, arm7.history[index & 15u]);
    putchar('\n');
    printf("run: %s, pc=%08" PRIx32 ", opcode=%08" PRIx32 "\n",
           result_name(result), cpu.trap_pc, cpu.trap_word);
    printf("flow: previous=%08" PRIx32 " last=%08" PRIx32 "\n",
           cpu.previous_pc, cpu.last_pc);
    fputs("history:", stdout);
    const uint32_t history_start = cpu.history_index > 16u ? cpu.history_index - 16u : 0u;
    for (uint32_t index = history_start; index < cpu.history_index; ++index)
        printf(" %08" PRIx32, cpu.history[index & 15u]);
    putchar('\n');
    printf("regs: r0=%08" PRIx32 " r1=%08" PRIx32 " r2=%08" PRIx32
           " r3=%08" PRIx32 " r4=%08" PRIx32 " r5=%08" PRIx32
           " r6=%08" PRIx32 " r7=%08" PRIx32 " sp=%08" PRIx32 " lr=%08" PRIx32 "\n",
           cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[4], cpu.r[5],
           cpu.r[6], cpu.r[7], cpu.r[13], cpu.r[14]);
    printf("ipc: arm9=%08" PRIx32 " arm7=%08" PRIx32 " sync=%04" PRIx16 "\n",
           nds_read32(&cpu, 0x02ffff88u), nds_read32(&cpu, 0x02ffff8cu),
           nds_read16(&cpu, 0x04000180u));
    printf("fifo-words: ARM9=%08" PRIx32 " ARM7=%08" PRIx32 "\n",
           cpu.fifo_word, arm7.fifo_word);
    printf("save: size=%zu ARM9=%02x/%" PRIu32 " ARM7=%02x/%" PRIu32
           " dirty=%u\n", cpu.save_size, cpu.backup_command, cpu.backup_position,
           arm7.backup_command, arm7.backup_position,
           (cpu.backup_dirty || arm7.backup_dirty) ? 1u : 0u);
    printf("irq=%08" PRIx32 " last-handler=%08" PRIx32
           " card: command=%08" PRIx32 "%08" PRIx32
           " control=%08" PRIx32 "\n",
           nds_read32(&cpu, 0x02fe3ffcu), cpu.last_irq_handler,
           nds_read32(&cpu, 0x040001a8u),
           nds_read32(&cpu, 0x040001acu), nds_read32(&cpu, 0x040001a4u));
    printf("sdk: globals=%08" PRIx32 " lock=%08" PRIx32 "/%08" PRIx32
           " stage=%08" PRIx32 "\n",
           nds_read32(&cpu, 0x02152ee0u), nds_read32(&cpu, 0x02152f40u),
           nds_read32(&cpu, 0x02152f70u),
           nds_read32(&cpu, 0x02fffc28u));
    printf("irq-state: ARM9 ime=%08" PRIx32 " ie=%08" PRIx32 " if=%08" PRIx32
           " fifo=%04" PRIx16 "; ARM7 ime=%08" PRIx32 " ie=%08" PRIx32
           " if=%08" PRIx32 " fifo=%04" PRIx16 " pending=%u\n",
           nds_read32(&cpu, 0x04000208u), nds_read32(&cpu, 0x04000210u),
           nds_read32(&cpu, 0x04000214u), nds_read16(&cpu, 0x04000184u),
           nds_read32(&arm7, 0x04000208u), nds_read32(&arm7, 0x04000210u),
           nds_read32(&arm7, 0x04000214u), nds_read16(&arm7, 0x04000184u),
           arm7.fifo_pending ? 1u : 0u);
    printf("activity: ARM9 instructions=%" PRIu64 " cpsr=%08" PRIx32
           " spsr=%08" PRIx32 " irq=%" PRIu32 "/%" PRIu32
           " sources=%08" PRIx32 " fifo=%" PRIu32 "/%" PRIu32
           "; ARM7 instructions=%" PRIu64 " cpsr=%08" PRIx32
           " spsr=%08" PRIx32 " irq=%" PRIu32 "/%" PRIu32
           " sources=%08" PRIx32 " fifo=%" PRIu32 "/%" PRIu32 "\n",
           cpu.instructions_executed, cpu.cpsr, cpu.spsr, cpu.irq_completed,
           cpu.irq_count, cpu.irq_sources, cpu.fifo_sent, cpu.fifo_received,
           arm7.instructions_executed, arm7.cpsr, arm7.spsr, arm7.irq_completed,
           arm7.irq_count, arm7.irq_sources, arm7.fifo_sent, arm7.fifo_received);
    printf("video: display=%08" PRIx32 "/%08" PRIx32
           " status=%04" PRIx16 " vcount=%" PRIu16
           " bg2=%04" PRIx16 "/%04" PRIx16
           " 3d=%04" PRIx16 " gx=%08" PRIx32
           " brightness=%04" PRIx16 "/%04" PRIx16 "\n",
           nds_read32(&cpu, UINT32_C(0x04000000)),
           nds_read32(&cpu, UINT32_C(0x04001000)),
           nds_read16(&cpu, UINT32_C(0x04000004)),
           nds_read16(&cpu, UINT32_C(0x04000006)),
           nds_read16(&cpu, UINT32_C(0x0400000c)),
           nds_read16(&cpu, UINT32_C(0x0400100c)),
           nds_read16(&cpu, UINT32_C(0x04000060)),
           nds_read32(&cpu, UINT32_C(0x04000600)),
           nds_read16(&cpu, UINT32_C(0x0400006c)),
           nds_read16(&cpu, UINT32_C(0x0400106c)));

    if (self_test) {
        const uint64_t hash = easygl2d_framebuffer_hash();
        printf("easyGL2D framebuffer hash: %016" PRIx64 "\n", hash);
        nds_cpu_destroy(&arm7);
        nds_cpu_destroy(&cpu);
        easygl2d_shutdown();
        return hash == 0 ? 1 : 0;
    }

    while (running && easygl2d_poll()) {
        const uint16_t keys = easygl2d_keyinput();
        const uint16_t extended_keys = easygl2d_extkeyin();
        nds_set_touch(&cpu, easygl2d_touching(), easygl2d_touch_x(), easygl2d_touch_y());
        nds_set_touch(&arm7, easygl2d_touching(), easygl2d_touch_x(), easygl2d_touch_y());
        nds_write16(&cpu, UINT32_C(0x04000130), keys);
        nds_write16(&arm7, UINT32_C(0x04000130), keys);
        nds_write16(&cpu, UINT32_C(0x04000136), extended_keys);
        nds_write16(&arm7, UINT32_C(0x04000136), extended_keys);
        running = run_steps(&cpu, &arm7, 560190u, &result, &arm7_result);
        nds_video_render(&cpu);
        easygl2d_present();
    }
    nds_cpu_destroy(&arm7);
    nds_cpu_destroy(&cpu);
    easygl2d_shutdown();
    return 0;
}
