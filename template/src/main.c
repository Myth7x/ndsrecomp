#include "arm9_recomp.h"
#include "easygl2d.h"
#include "nds_video.h"
#include "nds_gpu.h"
#include "rom_config.h"

#include <SDL3/SDL.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NDS_FRAME_NS UINT64_C(16714286)

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

static unsigned initialized_obj_count(const NdsCpu *cpu, unsigned engine) {
    unsigned count = 0u;
    for (unsigned object = 0; object < 128u; ++object) {
        const uint32_t address = UINT32_C(0x07000000) + engine * 0x400u +
                                 object * 8u;
        const uint16_t attr0 = nds_read16(cpu, address);
        const uint16_t attr1 = nds_read16(cpu, address + 2u);
        const uint16_t attr2 = nds_read16(cpu, address + 4u);
        if (attr0 != 0u || attr1 != 0u || attr2 != 0u)
            count++;
    }
    return count;
}

static bool run_steps(NdsCpu *cpu, NdsCpu *arm7, uint32_t count,
                      NdsRunResult *result, NdsRunResult *arm7_result) {
    /* Run long slices until a cross-CPU IPC write requires an early yield. */
    for (uint32_t step = 0; step < count;) {
        uint32_t quantum = count - step;
        if (quantum > 4096u)
            quantum = 4096u;
        if (cpu->wait_cycles != 0u && cpu->wait_cycles < quantum)
            quantum = cpu->wait_cycles;
        if (arm7->wait_cycles != 0u && arm7->wait_cycles < quantum)
            quantum = arm7->wait_cycles;
        const bool cpu_waiting = cpu->wait_cycles != 0u;
        const bool arm7_waiting = arm7->wait_cycles != 0u;
        const uint64_t cpu_instructions = cpu->instructions_executed;
        nds_poll_interrupts(cpu);
        if (!cpu_waiting && !cpu->halted) {
            cpu->reschedule = false;
            *result = nds_run_arm9(cpu, quantum);
            if (*result != NDS_RUN_BUDGET_EXHAUSTED)
                return false;
        }
        nds_poll_interrupts(arm7);
        if (!arm7_waiting && !arm7->halted) {
            arm7->reschedule = false;
            *arm7_result = nds_run_arm7(arm7, quantum);
            if (*arm7_result != NDS_RUN_BUDGET_EXHAUSTED)
                return false;
        }
        if (!cpu_waiting) {
            const uint64_t executed = cpu->instructions_executed - cpu_instructions;
            if (executed != 0u && executed < quantum)
                quantum = (uint32_t)executed;
        }
        nds_tick(cpu, quantum * 2u);
        nds_tick(arm7, quantum);
        if (cpu_waiting)
            cpu->wait_cycles = cpu->wait_cycles > quantum ?
                cpu->wait_cycles - quantum : 0u;
        if (arm7_waiting)
            arm7->wait_cycles = arm7->wait_cycles > quantum ?
                arm7->wait_cycles - quantum : 0u;
        nds_poll_interrupts(cpu);
        nds_poll_interrupts(arm7);
        step += quantum;
    }
    return true;
}

int main(int argc, char **argv) {
    bool self_test = false;
    bool one_frame = false;
    const char *rom_path = NULL;
    const char *dump_path = NULL;
    uint32_t step_limit = 0u;
    uint32_t frame_limit = 0u;
    bool scripted_touch = false;
    uint16_t scripted_touch_x = 0u;
    uint16_t scripted_touch_y = 0u;
    uint32_t scripted_tap_frame = UINT32_MAX;
    uint32_t scripted_tap_end_frame = UINT32_MAX;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--self-test") == 0) {
            self_test = true;
        } else if (strcmp(argv[index], "--once") == 0) {
            one_frame = true;
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
        } else if (strcmp(argv[index], "--frames") == 0 && index + 1 < argc) {
            char *end = NULL;
            const unsigned long parsed = strtoul(argv[++index], &end, 10);
            if (end == argv[index] || *end != '\0' || parsed == 0 ||
                parsed > UINT32_MAX) {
                fprintf(stderr, "invalid --frames value: %s\n", argv[index]);
                return 2;
            }
            frame_limit = (uint32_t)parsed;
        } else if (strcmp(argv[index], "--dump-frame") == 0 && index + 1 < argc) {
            dump_path = argv[++index];
        } else if (strcmp(argv[index], "--tap") == 0 && index + 2 < argc) {
            char *x_end = NULL;
            char *y_end = NULL;
            const unsigned long x = strtoul(argv[++index], &x_end, 10);
            const unsigned long y = strtoul(argv[++index], &y_end, 10);
            if (x_end == argv[index - 1] || *x_end != '\0' ||
                y_end == argv[index] || *y_end != '\0' ||
                x >= 256u || y >= 192u) {
                fprintf(stderr, "invalid --tap coordinates\n");
                return 2;
            }
            scripted_touch = true;
            scripted_touch_x = (uint16_t)x;
            scripted_touch_y = (uint16_t)y;
        } else if (strcmp(argv[index], "--tap-frame") == 0 && index + 3 < argc) {
            char *frame_end = NULL;
            char *x_end = NULL;
            char *y_end = NULL;
            const unsigned long frame = strtoul(argv[++index], &frame_end, 10);
            const unsigned long x = strtoul(argv[++index], &x_end, 10);
            const unsigned long y = strtoul(argv[++index], &y_end, 10);
            if (frame_end == argv[index - 2] || *frame_end != '\0' ||
                x_end == argv[index - 1] || *x_end != '\0' ||
                y_end == argv[index] || *y_end != '\0' ||
                frame >= UINT32_MAX || x >= 256u || y >= 192u) {
                fprintf(stderr, "invalid --tap-frame arguments\n");
                return 2;
            }
            scripted_touch = true;
            scripted_tap_frame = (uint32_t)frame;
            scripted_tap_end_frame = scripted_tap_frame + 120u;
            if (scripted_tap_end_frame < scripted_tap_frame)
                scripted_tap_end_frame = UINT32_MAX;
            scripted_touch_x = (uint16_t)x;
            scripted_touch_y = (uint16_t)y;
        } else {
            fprintf(stderr, "usage: %s [--self-test] [--once] [--frames count] [--rom path] [--steps count] [--dump-frame path] [--tap x y] [--tap-frame frame x y]\n", argv[0]);
            return 2;
        }
    }
    if (one_frame && frame_limit != 0u) {
        fputs("--once and --frames are mutually exclusive\n", stderr);
        return 2;
    }
    if (step_limit == 0u)
        step_limit = self_test ? 10000000u : 560190u;
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
    if (scripted_touch && scripted_tap_frame == UINT32_MAX) {
        nds_set_touch(&cpu, true, scripted_touch_x, scripted_touch_y);
        nds_set_touch(&arm7, true, scripted_touch_x, scripted_touch_y);
    }
    nds_gpu_begin_frame(cpu.gpu);
    bool running = run_steps(&cpu, &arm7, step_limit, &result, &arm7_result);
    nds_video_render(&cpu);
    EasyGL2DStats stats = {0};
    stats.running = running;
    stats.arm9_pc = cpu.r[15];
    stats.arm7_pc = arm7.r[15];
    stats.display_a = nds_read32(&cpu, UINT32_C(0x04000000));
    stats.display_b = nds_read32(&cpu, UINT32_C(0x04001000));
    stats.vcount = nds_read16(&cpu, UINT32_C(0x04000006));
    easygl2d_set_stats(&stats);
    easygl2d_present();
    if (dump_path != NULL && !easygl2d_dump_framebuffer(dump_path))
        fprintf(stderr, "failed to dump framebuffer to %s\n", dump_path);
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
    printf("vramcnt: %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8
           "  %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
           nds_read8(&cpu, UINT32_C(0x04000240)),
           nds_read8(&cpu, UINT32_C(0x04000241)),
           nds_read8(&cpu, UINT32_C(0x04000242)),
           nds_read8(&cpu, UINT32_C(0x04000243)),
           nds_read8(&cpu, UINT32_C(0x04000244)),
           nds_read8(&cpu, UINT32_C(0x04000245)),
           nds_read8(&cpu, UINT32_C(0x04000246)),
           nds_read8(&cpu, UINT32_C(0x04000248)));
    printf("gpu: commands=%" PRIu64 " triangles=%zu unsupported=%" PRIu64 "\n",
           nds_gpu_command_count(cpu.gpu), nds_gpu_triangle_count(cpu.gpu),
           nds_gpu_unsupported_count(cpu.gpu));
    printf("gpu texture max=%u pixels\n", nds_gpu_max_texture_pixels(cpu.gpu));
    printf("obj: initialized=%u/%u\n", initialized_obj_count(&cpu, 0u),
           initialized_obj_count(&cpu, 1u));
    fputs("gpu unsupported ids:", stdout);
    for (unsigned command = 0; command < 256u; ++command) {
        const uint32_t count = nds_gpu_command_histogram(cpu.gpu, command);
        if (count != 0u)
            printf(" %02x=%" PRIu32, command, count);
    }
    putchar('\n');

    if (self_test) {
        const uint64_t hash = easygl2d_framebuffer_hash();
        printf("easyGL2D framebuffer hash: %016" PRIx64 "\n", hash);
        nds_cpu_destroy(&arm7);
        nds_cpu_destroy(&cpu);
        easygl2d_shutdown();
        return hash == 0 ? 1 : 0;
    }

    if (one_frame) {
        nds_cpu_destroy(&arm7);
        nds_cpu_destroy(&cpu);
        easygl2d_shutdown();
        return running ? 0 : 1;
    }

    uint64_t previous_frame_start = SDL_GetTicksNS();
    uint64_t frame_deadline = previous_frame_start + NDS_FRAME_NS;
    double previous_present_ms = 0.0;
    while (running && easygl2d_poll()) {
        const uint64_t frame_start = SDL_GetTicksNS();
        const uint64_t arm9_before = cpu.instructions_executed;
        const uint64_t arm7_before = arm7.instructions_executed;
        const uint16_t keys = easygl2d_keyinput();
        const uint16_t extended_keys = easygl2d_extkeyin();
        const bool touching = scripted_touch ?
            (scripted_tap_frame == UINT32_MAX ||
             (stats.frame >= scripted_tap_frame &&
              stats.frame < scripted_tap_end_frame)) : easygl2d_touching();
        const uint16_t touch_x = scripted_touch ? scripted_touch_x : easygl2d_touch_x();
        const uint16_t touch_y = scripted_touch ? scripted_touch_y : easygl2d_touch_y();
        nds_write16(&cpu, UINT32_C(0x04000130), keys);
        nds_write16(&arm7, UINT32_C(0x04000130), keys);
        nds_write16(&cpu, UINT32_C(0x04000136), extended_keys);
        nds_write16(&arm7, UINT32_C(0x04000136), extended_keys);
        nds_set_touch(&cpu, touching, touch_x, touch_y);
        nds_set_touch(&arm7, touching, touch_x, touch_y);
        nds_gpu_begin_frame(cpu.gpu);
        running = run_steps(&cpu, &arm7, 560190u, &result, &arm7_result);
        const uint64_t emulation_end = SDL_GetTicksNS();
        nds_video_render(&cpu);
        const uint64_t video_end = SDL_GetTicksNS();

        const double frame_ms = (double)(frame_start - previous_frame_start) / 1000000.0;
        const double emulation_ms = (double)(emulation_end - frame_start) / 1000000.0;
        const double video_ms = (double)(video_end - emulation_end) / 1000000.0;
        const double instant_fps = frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0;
        stats.frame++;
        stats.fps = stats.fps == 0.0 ? instant_fps : stats.fps * 0.9 + instant_fps * 0.1;
        stats.frame_ms = frame_ms;
        stats.emulation_ms = emulation_ms;
        stats.video_ms = video_ms;
        stats.present_ms = previous_present_ms;
        stats.host_load = (emulation_ms + video_ms + previous_present_ms) *
                          100000000.0 / (double)NDS_FRAME_NS;
        stats.arm9_mips = emulation_ms > 0.0 ?
            (double)(cpu.instructions_executed - arm9_before) / emulation_ms / 1000.0 : 0.0;
        stats.arm7_mips = emulation_ms > 0.0 ?
            (double)(arm7.instructions_executed - arm7_before) / emulation_ms / 1000.0 : 0.0;
        stats.arm9_pc = cpu.r[15];
        stats.arm7_pc = arm7.r[15];
        stats.display_a = nds_read32(&cpu, UINT32_C(0x04000000));
        stats.display_b = nds_read32(&cpu, UINT32_C(0x04001000));
        stats.vcount = nds_read16(&cpu, UINT32_C(0x04000006));
        stats.running = running;
        easygl2d_set_stats(&stats);

        const uint64_t present_start = SDL_GetTicksNS();
        easygl2d_present();
        const uint64_t present_end = SDL_GetTicksNS();
        previous_present_ms = (double)(present_end - present_start) / 1000000.0;
        if (present_end < frame_deadline)
            SDL_DelayPrecise(frame_deadline - present_end);
        else if (present_end - frame_deadline > NDS_FRAME_NS)
            frame_deadline = present_end;
        frame_deadline += NDS_FRAME_NS;
        previous_frame_start = frame_start;
        if (frame_limit != 0u && stats.frame >= frame_limit)
            break;
    }
    if (dump_path != NULL && !easygl2d_dump_framebuffer(dump_path))
        fprintf(stderr, "failed to dump framebuffer to %s\n", dump_path);
    printf("final: run=%s frames=%" PRIu64 " pc=%08" PRIx32 "/%08" PRIx32
           " display=%08" PRIx32 "/%08" PRIx32 "\n",
           result_name(result), stats.frame, cpu.r[15], arm7.r[15],
           nds_read32(&cpu, UINT32_C(0x04000000)),
           nds_read32(&cpu, UINT32_C(0x04001000)));
    fputs("final texture formats (triangles/unique):", stdout);
    for (unsigned format = 0u; format < 8u; ++format)
        printf(" %u=%zu/%zu", format,
               nds_gpu_texture_format_count(cpu.gpu, format),
               nds_gpu_unique_texture_count(cpu.gpu, format));
    putchar('\n');
    printf("gpu texture cache: uploads=%" PRIu64 " entries=%zu\n",
           nds_gpu_texture_upload_count(cpu.gpu),
           nds_gpu_texture_cache_count(cpu.gpu));
    nds_cpu_destroy(&arm7);
    nds_cpu_destroy(&cpu);
    easygl2d_shutdown();
    return 0;
}
