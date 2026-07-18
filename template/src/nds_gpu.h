#pragma once

#include "nds_runtime.h"

NdsGpu *nds_gpu_create(void);
void nds_gpu_destroy(NdsGpu *gpu);
void nds_gpu_begin_frame(NdsGpu *gpu);
void nds_gpu_invalidate_textures(NdsGpu *gpu);
void nds_gpu_set_1dot_depth(NdsGpu *gpu, uint16_t depth);
void nds_gpu_vblank(NdsGpu *gpu);
void nds_gpu_write32(NdsGpu *gpu, uint32_t value);
void nds_gpu_write_port(NdsGpu *gpu, uint8_t command, uint32_t value);
void nds_gpu_render(const NdsCpu *cpu, int screen_y);
size_t nds_gpu_triangle_count(const NdsGpu *gpu);
uint64_t nds_gpu_command_count(const NdsGpu *gpu);
uint64_t nds_gpu_unsupported_count(const NdsGpu *gpu);
uint32_t nds_gpu_command_histogram(const NdsGpu *gpu, unsigned command);
unsigned nds_gpu_max_texture_pixels(const NdsGpu *gpu);
size_t nds_gpu_texture_format_count(const NdsGpu *gpu, unsigned format);
size_t nds_gpu_unique_texture_count(const NdsGpu *gpu, unsigned format);
uint64_t nds_gpu_texture_upload_count(const NdsGpu *gpu);
size_t nds_gpu_texture_cache_count(const NdsGpu *gpu);
uint32_t nds_gpu_read_status(const NdsGpu *gpu);
uint32_t nds_gpu_read_ram_count(const NdsGpu *gpu);
uint32_t nds_gpu_read_position_result(const NdsGpu *gpu, unsigned index);
uint16_t nds_gpu_read_vector_result(const NdsGpu *gpu, unsigned index);
uint32_t nds_gpu_read_clip_matrix(const NdsGpu *gpu, unsigned index);
uint32_t nds_gpu_read_vector_matrix(const NdsGpu *gpu, unsigned index);
void nds_gpu_write_status(NdsGpu *gpu, uint32_t value);
