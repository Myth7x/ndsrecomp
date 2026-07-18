#include "nds_gpu.h"

#include "easygl2d.h"

#include <SDL3/SDL_opengl.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define GPU_MAX_TRIANGLES 65536u
#define GPU_CMD_QUEUE 260u
#define GPU_STACK_DEPTH 32u

typedef struct {
    float x, y, z, w;
    float r, g, b, a;
    float u, v;
    float clip[4];
    float depth;
} GpuVertex;

typedef struct {
    GpuVertex vertex[3];
    uint32_t polygon;
    uint32_t texture;
    uint32_t palette;
    bool w_buffer;
    uint32_t sort_key;
} GpuTriangle;

typedef struct {
    float direction[3];
    float color[3];
} GpuLight;

typedef struct {
    uint32_t texture;
    uint32_t palette;
    GLuint name;
    uint64_t generation;
    bool ready;
} GpuTextureCacheEntry;

struct NdsGpuTag {
    float matrix[4][16];
    float stack[4][GPU_STACK_DEPTH][16];
    unsigned stack_top[4];
    unsigned matrix_mode;
    float vertex[3];
    float normal[3];
    float color[4];
    float diffuse[3];
    float ambient[3];
    float emissive[3];
    float specular[3];
    bool shininess_enabled;
    uint8_t shininess[128];
    GpuLight lights[4];
    float texcoord[2];
    float raw_texcoord[2];
    uint32_t polygon;
    uint32_t polygon_pending;
    bool polygon_pending_valid;
    uint32_t texture;
    uint32_t palette;
    uint32_t viewport;
    unsigned primitive;
    bool in_primitive;
    GpuVertex strip[4];
    unsigned strip_count;
    unsigned strip_parity;
    GpuTriangle *triangles;
    size_t triangle_count;
    size_t triangle_capacity;
    GpuTriangle *render_triangles;
    size_t render_triangle_count;
    size_t render_triangle_capacity;
    GpuTriangle *visible_triangles;
    size_t visible_triangle_count;
    size_t visible_triangle_capacity;
    GpuTriangle *pending_triangles;
    size_t pending_triangle_count;
    bool swap_pending;
    GpuTextureCacheEntry *texture_cache;
    size_t texture_cache_count;
    size_t texture_cache_capacity;
    uint64_t texture_generation;
    uint64_t texture_upload_count;
    uint16_t one_dot_depth;
    size_t vertex_count;
    uint8_t commands[GPU_CMD_QUEUE];
    unsigned command_head;
    unsigned command_count;
    uint8_t active_command;
    unsigned parameters_left;
    unsigned parameter_index;
    uint32_t parameters[32];
    uint8_t port_command;
    unsigned port_parameters_left;
    unsigned port_parameter_index;
    uint64_t command_count_total;
    uint64_t unsupported_commands;
    uint32_t command_histogram[256];
    uint32_t position_result[4];
    uint16_t vector_result[3];
    bool box_test_result;
    bool stack_error;
    unsigned fifo_irq_mode;
    uint32_t swap_attributes;
};

static void matrix_identity(float *matrix) {
    memset(matrix, 0, 16u * sizeof(*matrix));
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

static void matrix_copy(float *destination, const float *source) {
    memcpy(destination, source, 16u * sizeof(*destination));
}

static void matrix_multiply(float *destination, const float *left,
                            const float *right) {
    float result[16];
    for (unsigned column = 0; column < 4u; ++column) {
        for (unsigned row = 0; row < 4u; ++row) {
            float value = 0.0f;
            for (unsigned index = 0; index < 4u; ++index)
                value += left[index * 4u + row] * right[column * 4u + index];
            result[column * 4u + row] = value;
        }
    }
    matrix_copy(destination, result);
}

static float fixed20(uint32_t value) {
    return (float)(int32_t)value / 4096.0f;
}

static float fixed16(uint32_t value) {
    return (float)(int16_t)value / 4096.0f;
}

static float fixed10(uint32_t value) {
    int32_t result = (int32_t)(value & 0x3ffu);
    if (result & 0x200)
        result |= ~0x3ff;
    return (float)result / 64.0f;
}

static float fixed9(uint32_t value) {
    int32_t result = (int32_t)(value & 0x3ffu);
    if (result & 0x200)
        result |= ~0x3ff;
    return (float)result / 512.0f;
}

static float fixed10_relative(uint32_t value) {
    int32_t result = (int32_t)(value & 0x3ffu);
    if (result & 0x200)
        result |= ~0x3ff;
    return (float)result / 4096.0f;
}

static float color5(uint32_t value, unsigned shift) {
    const unsigned component = (value >> shift) & 31u;
    const unsigned expanded = component == 0u ? 0u : component * 2u + 1u;
    return (float)expanded / 63.0f;
}

static void update_lighting(NdsGpu *gpu) {
    float result[3] = {
        gpu->emissive[0], gpu->emissive[1], gpu->emissive[2]
    };
    for (unsigned light = 0; light < 4u; ++light) {
        if ((gpu->polygon & (1u << light)) == 0u)
            continue;
        float dot = -(gpu->normal[0] * gpu->lights[light].direction[0] +
                      gpu->normal[1] * gpu->lights[light].direction[1] +
                      gpu->normal[2] * gpu->lights[light].direction[2]);
        if (dot < 0.0f)
            dot = 0.0f;
        for (unsigned channel = 0; channel < 3u; ++channel)
            result[channel] += gpu->lights[light].color[channel] *
                               (gpu->ambient[channel] +
                                gpu->diffuse[channel] * dot);
        if (gpu->shininess_enabled && dot > 0.0f) {
            const float reflection_z = 2.0f * dot * gpu->normal[2] -
                                       gpu->lights[light].direction[2];
            if (reflection_z > 0.0f) {
                unsigned table_index = (unsigned)(reflection_z * 127.0f);
                if (table_index > 127u)
                    table_index = 127u;
                const float specular = (float)gpu->shininess[table_index] / 255.0f;
                for (unsigned channel = 0; channel < 3u; ++channel)
                    result[channel] += gpu->specular[channel] *
                                       gpu->lights[light].color[channel] * specular;
            }
        }
    }
    for (unsigned channel = 0; channel < 3u; ++channel) {
        if (result[channel] > 1.0f)
            result[channel] = 1.0f;
        gpu->color[channel] = result[channel];
    }
}

static const float *position_matrix(const NdsGpu *gpu) {
    return gpu->matrix[1];
}

static bool dual_matrix_mode(const NdsGpu *gpu) {
    return (gpu->matrix_mode & 3u) == 2u;
}

static bool dual_stack_mode(const NdsGpu *gpu) {
    return (gpu->matrix_mode & 3u) == 1u || dual_matrix_mode(gpu);
}

static unsigned matrix_stack_index(const NdsGpu *gpu) {
    const unsigned mode = gpu->matrix_mode & 3u;
    return mode == 2u ? 1u : mode;
}

static void matrix_set_current(NdsGpu *gpu, const float *matrix) {
    const unsigned mode = gpu->matrix_mode & 3u;
    if (mode == 2u) {
        matrix_copy(gpu->matrix[1], matrix);
        matrix_copy(gpu->matrix[2], matrix);
    } else {
        matrix_copy(gpu->matrix[mode], matrix);
    }
}

static void matrix_apply_current(NdsGpu *gpu, const float *transform,
                                 bool position_only) {
    const unsigned mode = gpu->matrix_mode & 3u;
    if (mode == 2u) {
        /* The DS fixed-point implementation computes current * transform
         * (its source calls this s*m because it uses row-vector notation). */
        matrix_multiply(gpu->matrix[1], gpu->matrix[1], transform);
        if (!position_only)
            matrix_multiply(gpu->matrix[2], gpu->matrix[2], transform);
    } else {
        matrix_multiply(gpu->matrix[mode], gpu->matrix[mode], transform);
    }
}

static void matrix_stack_push(NdsGpu *gpu) {
    const unsigned mode = gpu->matrix_mode & 3u;
    const unsigned stack = matrix_stack_index(gpu);
    unsigned top = gpu->stack_top[stack];
    if (mode == 0u || mode == 3u)
        top = 0u;
    const unsigned limit = mode == 0u || mode == 3u ? 1u : GPU_STACK_DEPTH - 1u;
    if (top >= limit) {
        gpu->stack_error = true;
        return;
    }
    matrix_copy(gpu->stack[stack][top], gpu->matrix[mode == 2u ? 1u : mode]);
    if (dual_stack_mode(gpu))
        matrix_copy(gpu->stack[2][top], gpu->matrix[2]);
    gpu->stack_top[stack] = top + 1u;
}

static void matrix_stack_pop(NdsGpu *gpu, uint32_t parameter) {
    const unsigned mode = gpu->matrix_mode & 3u;
    const unsigned stack = matrix_stack_index(gpu);
    int offset = (int)(parameter & 0x3fu);
    if ((offset & 0x20) != 0)
        offset -= 0x40;
    if (mode == 0u || mode == 3u)
        offset = 1;
    int top = (int)gpu->stack_top[stack] - offset;
    if (top < 0 || top >= (int)GPU_STACK_DEPTH) {
        gpu->stack_error = true;
        top = top < 0 ? 0 : (int)GPU_STACK_DEPTH - 1;
    }
    gpu->stack_top[stack] = (unsigned)top;
    matrix_copy(gpu->matrix[mode == 2u ? 1u : mode], gpu->stack[stack][top]);
    if (dual_stack_mode(gpu))
        matrix_copy(gpu->matrix[2], gpu->stack[2][top]);
}

static void matrix_stack_store(NdsGpu *gpu, uint32_t parameter,
                               bool restore) {
    const unsigned mode = gpu->matrix_mode & 3u;
    const unsigned stack = matrix_stack_index(gpu);
    unsigned index = mode == 0u || mode == 3u ? 0u : parameter & 31u;
    const unsigned limit = mode == 0u || mode == 3u ? 1u : GPU_STACK_DEPTH - 1u;
    if (index >= limit) {
        gpu->stack_error = true;
        return;
    }
    if (restore) {
        matrix_copy(gpu->matrix[mode == 2u ? 1u : mode], gpu->stack[stack][index]);
        if (dual_stack_mode(gpu))
            matrix_copy(gpu->matrix[2], gpu->stack[2][index]);
    } else {
        matrix_copy(gpu->stack[stack][index], gpu->matrix[mode == 2u ? 1u : mode]);
        if (dual_stack_mode(gpu))
            matrix_copy(gpu->stack[2][index], gpu->matrix[2]);
    }
}

static void transform_position(const NdsGpu *gpu, float x, float y, float z,
                               float output[4]) {
    const float *projection = gpu->matrix[0];
    const float *position = position_matrix(gpu);
    float combined[16];
    const float input[4] = {x, y, z, 1.0f};
    matrix_multiply(combined, projection, position);
    for (unsigned row = 0; row < 4u; ++row) {
        output[row] = 0.0f;
        for (unsigned column = 0; column < 4u; ++column)
            output[row] += combined[column * 4u + row] * input[column];
    }
}

static void transform_direction(const NdsGpu *gpu, float input_x,
                                float input_y, float input_z, float output[3]) {
    const float *matrix = gpu->matrix[2];
    const float input[4] = {input_x, input_y, input_z, 0.0f};
    for (unsigned row = 0; row < 3u; ++row) {
        output[row] = 0.0f;
        for (unsigned column = 0; column < 3u; ++column)
            output[row] += matrix[column * 4u + row] * input[column];
    }
}

static void update_texcoord_from_texcoord(NdsGpu *gpu) {
    if (((gpu->texture >> 30) & 3u) != 1u) {
        gpu->texcoord[0] = gpu->raw_texcoord[0];
        gpu->texcoord[1] = gpu->raw_texcoord[1];
        return;
    }
    const float *matrix = gpu->matrix[3];
    gpu->texcoord[0] = gpu->raw_texcoord[0] * matrix[0] +
                       gpu->raw_texcoord[1] * matrix[4] +
                       (matrix[8] + matrix[12]) / 16.0f;
    gpu->texcoord[1] = gpu->raw_texcoord[0] * matrix[1] +
                       gpu->raw_texcoord[1] * matrix[5] +
                       (matrix[9] + matrix[13]) / 16.0f;
}

static void update_texcoord_from_normal(NdsGpu *gpu) {
    if (((gpu->texture >> 30) & 3u) != 2u)
        return;
    const float *matrix = gpu->matrix[3];
    gpu->texcoord[0] = gpu->raw_texcoord[0] +
                       gpu->normal[0] * matrix[0] +
                       gpu->normal[1] * matrix[4] +
                       gpu->normal[2] * matrix[8];
    gpu->texcoord[1] = gpu->raw_texcoord[1] +
                       gpu->normal[0] * matrix[1] +
                       gpu->normal[1] * matrix[5] +
                       gpu->normal[2] * matrix[9];
}

static uint32_t fixed_result(float value) {
    const double scaled = (double)value * 4096.0;
    if (scaled > 2147483647.0)
        return UINT32_MAX;
    if (scaled < -2147483648.0)
        return UINT32_C(0x80000000);
    return (uint32_t)(int32_t)lrint(scaled);
}

static uint16_t vector_result(float value) {
    return (uint16_t)(int16_t)lrint((double)value * 4096.0);
}

static unsigned command_parameters(uint8_t command) {
    switch (command) {
    case 0x10: case 0x13: case 0x14: case 0x20:
    case 0x21:
    case 0x22: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28:
    case 0x29: case 0x2a: case 0x2b: case 0x30: case 0x31: case 0x32:
    case 0x33: case 0x40:
    case 0x50: case 0x60: case 0x72:
        return 1u;
    case 0x12: return 1u;
    case 0x70: return 3u;
    case 0x71: return 2u;
    case 0x16: case 0x18: return 16u;
    case 0x17: case 0x19: return 12u;
    case 0x1a: return 9u;
    case 0x1b: case 0x1c: return 3u;
    case 0x23: return 2u;
    case 0x34: return 32u;
    default: return 0u;
    }
}

static void matrix_load_4x3(float *matrix, const uint32_t *parameters) {
    /* GX payloads are row-oriented and the DS composes them as row-vector
     * transforms.  The renderer uses column-vector math, so store the
     * transpose in its column-major representation. */
    matrix_identity(matrix);
    for (unsigned row = 0; row < 3u; ++row)
        for (unsigned column = 0; column < 3u; ++column)
            matrix[column * 4u + row] =
                fixed20(parameters[column * 3u + row]);
    matrix[12] = fixed20(parameters[9]);
    matrix[13] = fixed20(parameters[10]);
    matrix[14] = fixed20(parameters[11]);
}

static void matrix_load_4x4(float *matrix, const uint32_t *parameters) {
    for (unsigned row = 0; row < 4u; ++row)
        for (unsigned column = 0; column < 4u; ++column)
            matrix[column * 4u + row] =
                fixed20(parameters[column * 4u + row]);
}

static void transform_vertex(const NdsGpu *gpu, GpuVertex *vertex) {
    const float *projection = gpu->matrix[0];
    const float *position = position_matrix(gpu);
    float combined[16];
    float input[4] = {gpu->vertex[0], gpu->vertex[1], gpu->vertex[2], 1.0f};
    float output[4];
    matrix_multiply(combined, projection, position);
    for (unsigned row = 0; row < 4u; ++row) {
        output[row] = 0.0f;
        for (unsigned column = 0; column < 4u; ++column)
            output[row] += combined[column * 4u + row] * input[column];
    }
    for (unsigned index = 0; index < 4u; ++index)
        vertex->clip[index] = output[index];
    const float reciprocal_w = output[3] == 0.0f ? 1.0f : 1.0f / output[3];
    vertex->w = output[3] == 0.0f ? 1.0f : output[3];
    const unsigned viewport_x = gpu->viewport & 0xffu;
    const unsigned viewport_y = 191u - ((gpu->viewport >> 24) & 0xffu);
    const unsigned viewport_w = ((gpu->viewport >> 16) & 0xffu) -
                                viewport_x + 1u;
    const unsigned viewport_h = 191u - ((gpu->viewport >> 8) & 0xffu) -
                                viewport_y + 1u;
    const float x = output[0] * reciprocal_w * 0.5f + 0.5f;
    const float y = output[1] * reciprocal_w * 0.5f + 0.5f;
    vertex->x = ((float)viewport_x + x * (float)viewport_w) / 128.0f - 1.0f;
    /* easygl2d_gl_end flips OpenGL's bottom-up readback into DS top-down
       scanlines, so invert the DS viewport Y when handing it to GL. */
    vertex->y = 1.0f - ((float)viewport_y +
                        (1.0f - y) * (float)viewport_h) / 96.0f;
    vertex->z = output[2] * reciprocal_w;
    vertex->r = gpu->color[0];
    vertex->g = gpu->color[1];
    vertex->b = gpu->color[2];
    vertex->a = gpu->color[3] * (float)((gpu->polygon >> 16) & 31u) / 31.0f;
    float texture_input[4] = {
        gpu->vertex[0], gpu->vertex[1], gpu->vertex[2], 1.0f
    };
    const unsigned texture_mode = (gpu->texture >> 30) & 3u;
    if (texture_mode == 3u) {
        const float *matrix = gpu->matrix[3];
        vertex->u = gpu->raw_texcoord[0] +
                    (texture_input[0] * matrix[0] +
                     texture_input[1] * matrix[4] +
                     texture_input[2] * matrix[8]);
        vertex->v = gpu->raw_texcoord[1] +
                    (texture_input[0] * matrix[1] +
                     texture_input[1] * matrix[5] +
                     texture_input[2] * matrix[9]);
    } else {
        vertex->u = gpu->texcoord[0];
        vertex->v = gpu->texcoord[1];
    }
}

static bool store_triangle(NdsGpu *gpu, const GpuVertex *a,
                           const GpuVertex *b, const GpuVertex *c) {
    if (gpu->triangle_count == gpu->triangle_capacity) {
        size_t capacity = gpu->triangle_capacity == 0u ? 4096u :
                          gpu->triangle_capacity * 2u;
        if (capacity > GPU_MAX_TRIANGLES)
            capacity = GPU_MAX_TRIANGLES;
        if (capacity == gpu->triangle_capacity)
            return false;
        GpuTriangle *triangles = realloc(gpu->triangles,
                                         capacity * sizeof(*triangles));
        if (triangles == NULL)
            return false;
        gpu->triangles = triangles;
        gpu->triangle_capacity = capacity;
    }
    GpuTriangle *triangle = &gpu->triangles[gpu->triangle_count++];
    triangle->vertex[0] = *a;
    triangle->vertex[1] = *b;
    triangle->vertex[2] = *c;
    triangle->polygon = gpu->polygon;
    triangle->texture = gpu->texture;
    triangle->palette = gpu->palette;
    triangle->w_buffer = (gpu->swap_attributes & 2u) != 0u;
    float top = (1.0f - a->y) * 96.0f;
    float bottom = top;
    const GpuVertex *vertices[2] = {b, c};
    for (unsigned index = 0; index < 2u; ++index) {
        const float y = (1.0f - vertices[index]->y) * 96.0f;
        if (y < top) top = y;
        if (y > bottom) bottom = y;
    }
    if (top < 0.0f) top = 0.0f;
    if (bottom < 0.0f) bottom = 0.0f;
    if (top > 255.0f) top = 255.0f;
    if (bottom > 255.0f) bottom = 255.0f;
    const unsigned format = (triangle->texture >> 26) & 7u;
    const unsigned alpha = (triangle->polygon >> 16) & 31u;
    const bool translucent = format == 1u || format == 6u ||
                             (alpha > 0u && alpha < 31u);
    triangle->sort_key = (translucent ? 0x10000u : 0u) |
                         ((unsigned)bottom << 8) | (unsigned)top;
    return true;
}

static bool triangle_is_translucent(const GpuTriangle *triangle) {
    const unsigned format = (triangle->texture >> 26) & 7u;
    const unsigned alpha = (triangle->polygon >> 16) & 31u;
    return format == 1u || format == 6u || (alpha > 0u && alpha < 31u);
}

static int compare_triangle_sort_key(const void *left, const void *right) {
    const GpuTriangle *first = left;
    const GpuTriangle *second = right;
    if (first->sort_key < second->sort_key)
        return -1;
    if (first->sort_key > second->sort_key)
        return 1;
    return 0;
}

static void sort_triangles(GpuTriangle *triangles, size_t count,
                           size_t opaque_count) {
    if (triangles != NULL && opaque_count > 1u)
        qsort(triangles, opaque_count, sizeof(*triangles),
              compare_triangle_sort_key);
    (void)count;
}

static float clip_distance(const GpuVertex *vertex, unsigned plane) {
    const float x = vertex->clip[0];
    const float y = vertex->clip[1];
    const float z = vertex->clip[2];
    const float w = vertex->clip[3];
    switch (plane) {
    case 0: return x + w;
    case 1: return w - x;
    case 2: return y + w;
    case 3: return w - y;
    /* DS clips Z symmetrically, -W <= Z <= W.  Its clip volume is not the
     * OpenGL-style 0 <= Z near plane. */
    case 4: return z + w;
    default: return w - z;
    }
}

static GpuVertex clip_intersection(const GpuVertex *first,
                                   const GpuVertex *second, float t) {
    GpuVertex result = *first;
    const float inverse = 1.0f - t;
    result.r = first->r * inverse + second->r * t;
    result.g = first->g * inverse + second->g * t;
    result.b = first->b * inverse + second->b * t;
    result.a = first->a * inverse + second->a * t;
    result.u = first->u * inverse + second->u * t;
    result.v = first->v * inverse + second->v * t;
    for (unsigned index = 0; index < 4u; ++index)
        result.clip[index] = first->clip[index] * inverse +
                             second->clip[index] * t;
    return result;
}

static void update_viewport_vertex(const NdsGpu *gpu, GpuVertex *vertex) {
    const float reciprocal_w = vertex->clip[3] == 0.0f ? 1.0f :
                               1.0f / vertex->clip[3];
    vertex->w = vertex->clip[3] == 0.0f ? 1.0f : vertex->clip[3];
    const unsigned viewport_x = gpu->viewport & 0xffu;
    const unsigned viewport_y = 191u - ((gpu->viewport >> 24) & 0xffu);
    const unsigned viewport_w = ((gpu->viewport >> 16) & 0xffu) -
                                viewport_x + 1u;
    const unsigned viewport_h = 191u - ((gpu->viewport >> 8) & 0xffu) -
                                viewport_y + 1u;
    const float x = vertex->clip[0] * reciprocal_w * 0.5f + 0.5f;
    const float y = vertex->clip[1] * reciprocal_w * 0.5f + 0.5f;
    vertex->x = ((float)viewport_x + x * (float)viewport_w) / 128.0f - 1.0f;
    vertex->y = 1.0f - ((float)viewport_y +
                        (1.0f - y) * (float)viewport_h) / 96.0f;
    vertex->z = vertex->clip[2] * reciprocal_w;
}

static bool polygon_is_visible(const NdsGpu *gpu, const GpuVertex *a,
                               const GpuVertex *b, const GpuVertex *c) {
    const float normal_x = (a->clip[1] - b->clip[1]) *
                           (c->clip[3] - b->clip[3]) -
                           (a->clip[3] - b->clip[3]) *
                           (c->clip[1] - b->clip[1]);
    const float normal_y = (a->clip[3] - b->clip[3]) *
                           (c->clip[0] - b->clip[0]) -
                           (a->clip[0] - b->clip[0]) *
                           (c->clip[3] - b->clip[3]);
    const float normal_z = (a->clip[0] - b->clip[0]) *
                           (c->clip[1] - b->clip[1]) -
                           (a->clip[1] - b->clip[1]) *
                           (c->clip[0] - b->clip[0]);
    const float dot = b->clip[0] * normal_x + b->clip[1] * normal_y +
                      b->clip[3] * normal_z;
    if (dot < 0.0f)
        return (gpu->polygon & (1u << 7)) != 0u;
    if (dot > 0.0f)
        return (gpu->polygon & (1u << 6)) != 0u;
    return true;
}

static bool append_triangle(NdsGpu *gpu, const GpuVertex *a,
                            const GpuVertex *b, const GpuVertex *c) {
    const bool visible = polygon_is_visible(gpu, a, b, c);
    if (!visible)
        return true;
    if ((gpu->polygon & (1u << 12)) == 0u &&
        (a->clip[2] > a->clip[3] || b->clip[2] > b->clip[3] ||
         c->clip[2] > c->clip[3]))
        return true;
    GpuVertex input[16];
    GpuVertex output[16];
    unsigned count = 3u;
    input[0] = *a;
    input[1] = *b;
    input[2] = *c;
    /* The DS pipeline clips Z first, then Y, then X. */
    static const unsigned planes[6] = {4u, 5u, 2u, 3u, 0u, 1u};
    for (unsigned plane_index = 0; plane_index < 6u && count != 0u;
         ++plane_index) {
        const unsigned plane = planes[plane_index];
        unsigned output_count = 0u;
        GpuVertex previous = input[count - 1u];
        float previous_distance = clip_distance(&previous, plane);
        bool previous_inside = previous_distance >= 0.0f;
        for (unsigned index = 0; index < count; ++index) {
            const GpuVertex current = input[index];
            const float current_distance = clip_distance(&current, plane);
            const bool current_inside = current_distance >= 0.0f;
            if (current_inside != previous_inside) {
                const float denominator = previous_distance - current_distance;
                const float t = denominator == 0.0f ? 0.0f :
                    previous_distance / denominator;
                if (output_count < 16u)
                    output[output_count++] = clip_intersection(&previous,
                                                               &current, t);
            }
            if (current_inside && output_count < 16u)
                output[output_count++] = current;
            previous = current;
            previous_distance = current_distance;
            previous_inside = current_inside;
        }
        memcpy(input, output, output_count * sizeof(*input));
        count = output_count;
    }
    if (count < 3u)
        return true;
    if ((gpu->polygon & (1u << 13)) == 0u) {
        const float threshold = (float)gpu->one_dot_depth / 8.0f;
        const bool too_far = a->clip[3] > threshold &&
                             b->clip[3] > threshold &&
                             c->clip[3] > threshold;
        GpuVertex projected[3] = {*a, *b, *c};
        for (unsigned vertex = 0; vertex < 3u; ++vertex)
            update_viewport_vertex(gpu, &projected[vertex]);
        float min_x = projected[0].x;
        float max_x = min_x;
        float min_y = projected[0].y;
        float max_y = min_y;
        for (unsigned vertex = 1u; vertex < 3u; ++vertex) {
            if (projected[vertex].x < min_x) min_x = projected[vertex].x;
            if (projected[vertex].x > max_x) max_x = projected[vertex].x;
            if (projected[vertex].y < min_y) min_y = projected[vertex].y;
            if (projected[vertex].y > max_y) max_y = projected[vertex].y;
        }
        const bool zero_width = floorf((min_x + 1.0f) * 128.0f) ==
                                floorf((max_x + 1.0f) * 128.0f);
        const bool zero_height = floorf((1.0f - max_y) * 96.0f) ==
                                 floorf((1.0f - min_y) * 96.0f);
        if (too_far && zero_width && zero_height)
            return true;
    }
    uint64_t maximum_w = 0u;
    for (unsigned vertex = 0; vertex < count; ++vertex) {
        int64_t raw_w = llround((double)input[vertex].clip[3] * 4096.0);
        if (raw_w < 0) raw_w = 0;
        if ((uint64_t)raw_w > maximum_w)
            maximum_w = (uint64_t)raw_w;
    }
    unsigned wsize = 0u;
    while ((maximum_w >> wsize) != 0u && wsize < 32u)
        wsize += 4u;
    for (unsigned vertex = 0; vertex < count; ++vertex) {
        int64_t raw_w = llround((double)input[vertex].clip[3] * 4096.0);
        if (raw_w < 0) raw_w = 0;
        /* DS W-buffering normalizes W independently for each polygon to a
         * 16-bit value.  Preserve that normalized value for the host depth
         * buffer; using the absolute W makes every polygon compare at the
         * wrong depth and hides otherwise valid geometry. */
        const int64_t normalized_w = wsize < 16u ?
            raw_w << (16u - wsize) : raw_w >> (wsize - 16u);
        input[vertex].depth = (float)((double)normalized_w / 65535.0);
    }
    for (unsigned index = 1u; index + 1u < count; ++index) {
        update_viewport_vertex(gpu, &input[0]);
        update_viewport_vertex(gpu, &input[index]);
        update_viewport_vertex(gpu, &input[index + 1u]);
        if (!store_triangle(gpu, &input[0], &input[index],
                            &input[index + 1u]))
            return false;
    }
    return true;
}

static uint32_t rgba15(uint16_t value) {
    const unsigned r = value & 31u;
    const unsigned g = (value >> 5) & 31u;
    const unsigned b = (value >> 10) & 31u;
    const unsigned r6 = r == 0u ? 0u : r * 2u + 1u;
    const unsigned g6 = g == 0u ? 0u : g * 2u + 1u;
    const unsigned b6 = b == 0u ? 0u : b * 2u + 1u;
    return (r6 * 4u + r6 / 16u) |
           ((g6 * 4u + g6 / 16u) << 8) |
           ((b6 * 4u + b6 / 16u) << 16) | UINT32_C(0xff000000);
}

static uint32_t rgba15_direct(uint16_t value) {
    uint32_t color = rgba15(value);
    if ((value & 0x8000u) == 0u)
        color &= UINT32_C(0x00ffffff);
    return color;
}

static uint16_t mix555(uint16_t first, uint16_t second,
                       unsigned first_weight, unsigned second_weight,
                       unsigned divisor) {
    const unsigned r = (((first & 31u) * first_weight) +
                        ((second & 31u) * second_weight)) / divisor;
    const unsigned g = ((((first >> 5) & 31u) * first_weight) +
                        (((second >> 5) & 31u) * second_weight)) / divisor;
    const unsigned b = ((((first >> 10) & 31u) * first_weight) +
                        (((second >> 10) & 31u) * second_weight)) / divisor;
    return (uint16_t)(r | (g << 5) | (b << 10));
}

static uint32_t compressed_texel(const NdsCpu *cpu, uint32_t texture,
                                 uint32_t palette, unsigned width,
                                 unsigned x, unsigned y) {
    uint32_t address = (texture & 0xffffu) * 8u;
    address += ((y & 0x3fcu) * (width >> 2)) + (x & 0x3fcu) + (y & 3u);
    address &= 0x7ffffu;
    const uint32_t palette_index_address =
        0x20000u + ((address & 0x1fffcu) >> 1) +
        (address >= 0x40000u ? 0x10000u : 0u);
    const uint16_t palette_info = nds_read16(cpu,
        UINT32_C(0x06800000) + palette_index_address);
    const unsigned index = address >= 0x20000u && address < 0x40000u ? 0u :
        (nds_read8(cpu, UINT32_C(0x06800000) + address) >>
         ((x & 3u) * 2u)) & 3u;
    const uint32_t palette_address = UINT32_C(0x06880000) +
        (palette & 0x1fffu) * 16u + (palette_info & 0x3fffu) * 4u;
    const uint16_t color0 = nds_read16(cpu, palette_address);
    const uint16_t color1 = nds_read16(cpu, palette_address + 2u);
    const unsigned mode = palette_info >> 14;
    uint16_t color = color0;
    bool transparent = false;
    if (index == 1u) {
        color = color1;
    } else if (index == 2u) {
        if (mode == 1u)
            color = mix555(color0, color1, 1u, 1u, 2u);
        else if (mode == 3u)
            color = mix555(color0, color1, 5u, 3u, 8u);
        else
            color = nds_read16(cpu, palette_address + 4u);
    } else if (index == 3u) {
        if (mode == 0u || mode == 1u) {
            transparent = true;
        } else if (mode == 2u) {
            color = nds_read16(cpu, palette_address + 6u);
        } else {
            color = mix555(color0, color1, 3u, 5u, 8u);
        }
    }
    return transparent ? rgba15_direct(0u) : rgba15(color);
}

static uint32_t texture_texel(const NdsCpu *cpu, uint32_t texture,
                              uint32_t palette, unsigned format,
                              unsigned width, unsigned x, unsigned y) {
    const uint32_t base = UINT32_C(0x06800000) + (texture & 0xffffu) * 8u;
    const unsigned pixel = y * width + x;
    if (format == 7u)
        return rgba15_direct(nds_read16(cpu, base + pixel * 2u));
    if (format == 1u || format == 6u) {
        const uint8_t packed = nds_read8(cpu, base + pixel);
        const unsigned alpha_bits = format == 1u ? packed >> 5 : packed >> 3;
        const unsigned index = format == 1u ? packed & 31u : packed & 7u;
        uint32_t color = rgba15(nds_read16(cpu, UINT32_C(0x06880000) +
                                           (palette & 0x1fffu) * 16u + index * 2u));
        const unsigned alpha = format == 1u ?
            (alpha_bits * 4u + alpha_bits / 2u) : alpha_bits;
        color = (color & UINT32_C(0x00ffffff)) |
                ((alpha * 255u / 31u) << 24);
        return color;
    }
    unsigned index;
    if (format == 2u) {
        const uint8_t packed = nds_read8(cpu, base + pixel / 4u);
        index = (packed >> ((pixel & 3u) * 2u)) & 3u;
    } else if (format == 3u) {
        const uint8_t packed = nds_read8(cpu, base + pixel / 2u);
        index = (pixel & 1u) ? packed >> 4 : packed & 15u;
    } else if (format == 4u) {
        index = nds_read8(cpu, base + pixel);
    } else {
        return 0u;
    }
    const uint32_t palette_step = format == 2u ? 8u : 16u;
    uint32_t color = rgba15(nds_read16(cpu, UINT32_C(0x06880000) +
                                       (palette & 0x1fffu) * palette_step + index * 2u));
    if (index == 0u && (texture & (1u << 29)) != 0u)
        color &= UINT32_C(0x00ffffff);
    return color;
}

static bool upload_texture(const NdsCpu *cpu, const GpuTriangle *triangle,
                           GLuint texture_name) {
    const unsigned format = (triangle->texture >> 26) & 7u;
    if (format == 0u)
        return false;
    const unsigned width = 8u << ((triangle->texture >> 20) & 7u);
    const unsigned height = 8u << ((triangle->texture >> 23) & 7u);
    if (width > 1024u || height > 1024u)
        return false;
    uint32_t *pixels = malloc((size_t)width * height * sizeof(*pixels));
    if (pixels == NULL)
        return false;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            pixels[y * width + x] = format == 5u ?
                compressed_texel(cpu, triangle->texture, triangle->palette,
                                 width, x, y) :
                texture_texel(cpu, triangle->texture, triangle->palette,
                              format, width, x, y);
    static bool debug_texture_dumped;
    if (!debug_texture_dumped && format == 5u) {
        FILE *file = fopen("/tmp/sm64-texture.ppm", "wb");
        if (file != NULL) {
            fprintf(file, "P6\n%u %u\n255\n", width, height);
            for (size_t index = 0; index < (size_t)width * height; ++index) {
                const uint32_t color = pixels[index];
                fputc((int)(color >> 24), file);
                fputc((int)(color >> 16), file);
                fputc((int)(color >> 8), file);
            }
            fclose(file);
        }
        debug_texture_dumped = true;
    }
    glBindTexture(GL_TEXTURE_2D, texture_name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                    (triangle->texture & (1u << 16)) ?
                    ((triangle->texture & (1u << 18)) ? GL_MIRRORED_REPEAT : GL_REPEAT) :
                    GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                    (triangle->texture & (1u << 17)) ?
                    ((triangle->texture & (1u << 19)) ? GL_MIRRORED_REPEAT : GL_REPEAT) :
                    GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    free(pixels);
    return true;
}

static void toon_color(const NdsCpu *cpu, const GpuVertex *vertex,
                       float color[3]) {
    /* The red component selects all 32 entries of the toon table. */
    const unsigned index = (unsigned)(vertex->r * 31.0f) > 31u ?
        31u : (unsigned)(vertex->r * 31.0f);
    const uint16_t value = nds_read16(cpu, UINT32_C(0x04000380) + index * 2u);
    const unsigned red = value & 31u;
    const unsigned green = (value >> 5) & 31u;
    const unsigned blue = (value >> 10) & 31u;
    color[0] = (float)(red == 0u ? 0u : red * 2u + 1u) / 63.0f;
    color[1] = (float)(green == 0u ? 0u : green * 2u + 1u) / 63.0f;
    color[2] = (float)(blue == 0u ? 0u : blue * 2u + 1u) / 63.0f;
}

static void edge_color(const NdsCpu *cpu, const GpuTriangle *triangle,
                       float color[3]) {
    const unsigned polygon_id = (triangle->polygon >> 24) & 63u;
    const uint16_t value = nds_read16(cpu, UINT32_C(0x04000330) +
                                      (polygon_id >> 3) * 2u);
    color[0] = color5(value, 0u);
    color[1] = color5(value, 5u);
    color[2] = color5(value, 10u);
}

static bool fullscreen_composite(const GpuTriangle *triangle) {
    float min_x = triangle->vertex[0].x;
    float max_x = min_x;
    float min_y = triangle->vertex[0].y;
    float max_y = min_y;
    for (unsigned vertex = 1u; vertex < 3u; ++vertex) {
        if (triangle->vertex[vertex].x < min_x) min_x = triangle->vertex[vertex].x;
        if (triangle->vertex[vertex].x > max_x) max_x = triangle->vertex[vertex].x;
        if (triangle->vertex[vertex].y < min_y) min_y = triangle->vertex[vertex].y;
        if (triangle->vertex[vertex].y > max_y) max_y = triangle->vertex[vertex].y;
    }
    return max_x - min_x > 1.5f && max_y - min_y > 1.5f;
}

static void emit_vertex(NdsGpu *gpu) {
    GpuVertex vertex;
    transform_vertex(gpu, &vertex);
    if (!gpu->in_primitive)
        return;
    gpu->vertex_count++;
    if (gpu->primitive == 0u) {
        gpu->strip[gpu->strip_count++] = vertex;
        if (gpu->strip_count == 3u) {
            append_triangle(gpu, &gpu->strip[0], &gpu->strip[1], &gpu->strip[2]);
            gpu->strip_count = 0u;
        }
    } else if (gpu->primitive == 1u) {
        gpu->strip[gpu->strip_count++] = vertex;
        if (gpu->strip_count == 4u) {
            append_triangle(gpu, &gpu->strip[0], &gpu->strip[1], &gpu->strip[2]);
            append_triangle(gpu, &gpu->strip[0], &gpu->strip[2], &gpu->strip[3]);
            gpu->strip_count = 0u;
        }
    } else if (gpu->primitive == 2u) {
        if (gpu->strip_count < 3u) {
            gpu->strip[gpu->strip_count++] = vertex;
            if (gpu->strip_count == 3u)
                append_triangle(gpu, &gpu->strip[0], &gpu->strip[1], &gpu->strip[2]);
        } else {
            if (gpu->strip_parity == 0u)
                append_triangle(gpu, &gpu->strip[2], &gpu->strip[1], &vertex);
            else
                append_triangle(gpu, &gpu->strip[1], &gpu->strip[2], &vertex);
            gpu->strip[1] = gpu->strip[2];
            gpu->strip[2] = vertex;
            gpu->strip_parity ^= 1u;
        }
    } else if (gpu->primitive == 3u) {
        gpu->strip[gpu->strip_count++] = vertex;
        if (gpu->strip_count == 4u) {
            append_triangle(gpu, &gpu->strip[0], &gpu->strip[1], &gpu->strip[2]);
            append_triangle(gpu, &gpu->strip[1], &gpu->strip[3], &gpu->strip[2]);
            gpu->strip[0] = gpu->strip[2];
            gpu->strip[1] = gpu->strip[3];
            gpu->strip_count = 2u;
        }
    } else if (gpu->primitive == 4u) {
        if (gpu->strip_count == 0u)
            gpu->strip[0] = vertex;
        else if (gpu->strip_count == 1u)
            gpu->strip[1] = vertex;
        else {
            append_triangle(gpu, &gpu->strip[0], &gpu->strip[1], &vertex);
            gpu->strip[1] = vertex;
        }
        if (gpu->strip_count < 2u)
            gpu->strip_count++;
    }
}

static void execute_box_test(NdsGpu *gpu, const uint32_t *p) {
    const float x = fixed16(p[0]);
    const float y = fixed16(p[0] >> 16);
    const float z = fixed16(p[1]);
    const float width = fixed16(p[1] >> 16);
    const float height = fixed16(p[2]);
    const float depth = fixed16(p[2] >> 16);
    gpu->box_test_result = false;
    for (unsigned corner = 0; corner < 8u; ++corner) {
        float clip[4];
        transform_position(gpu,
                           x + ((corner & 1u) ? width : 0.0f),
                           y + ((corner & 2u) ? height : 0.0f),
                           z + ((corner & 4u) ? depth : 0.0f), clip);
        if (clip[3] > 0.0f && clip[0] >= -clip[3] && clip[0] <= clip[3] &&
            clip[1] >= -clip[3] && clip[1] <= clip[3] &&
            clip[2] >= -clip[3] && clip[2] <= clip[3]) {
            gpu->box_test_result = true;
            break;
        }
    }
}

static void execute_command(NdsGpu *gpu) {
    const uint32_t *p = gpu->parameters;
    switch (gpu->active_command) {
    case 0x10: gpu->matrix_mode = p[0] & 3u; break;
    case 0x11: matrix_stack_push(gpu); break;
    case 0x12: matrix_stack_pop(gpu, p[0]); break;
    case 0x13: matrix_stack_store(gpu, p[0], false); break;
    case 0x14: matrix_stack_store(gpu, p[0], true); break;
    case 0x15: {
        float identity[16];
        matrix_identity(identity);
        matrix_set_current(gpu, identity);
        break;
    }
    case 0x16: {
        float matrix[16];
        matrix_load_4x4(matrix, p);
        matrix_set_current(gpu, matrix);
        break;
    }
    case 0x17: {
        float matrix[16];
        matrix_load_4x3(matrix, p);
        matrix_set_current(gpu, matrix);
        break;
    }
    case 0x18: {
        float transform[16];
        matrix_load_4x4(transform, p);
        matrix_apply_current(gpu, transform, false);
        break;
    }
    case 0x19: {
        float transform[16];
        matrix_load_4x3(transform, p);
        matrix_apply_current(gpu, transform, false);
        break;
    }
    case 0x1a: {
        float transform[16]; matrix_identity(transform);
        for (unsigned row = 0; row < 3u; ++row)
            for (unsigned column = 0; column < 3u; ++column)
                transform[column * 4u + row] =
                    fixed20(p[column * 3u + row]);
        matrix_apply_current(gpu, transform, false);
        break;
    }
    case 0x1b: {
        float transform[16];
        matrix_identity(transform);
        transform[0] = fixed20(p[0]);
        transform[5] = fixed20(p[1]);
        transform[10] = fixed20(p[2]);
        matrix_apply_current(gpu, transform, true);
        break;
    }
    case 0x1c: {
        float transform[16];
        matrix_identity(transform);
        transform[12] = fixed20(p[0]);
        transform[13] = fixed20(p[1]);
        transform[14] = fixed20(p[2]);
        matrix_apply_current(gpu, transform, false);
        break;
    }
    case 0x20:
        gpu->color[0] = color5(p[0], 0u);
        gpu->color[1] = color5(p[0], 5u);
        gpu->color[2] = color5(p[0], 10u);
        break;
    case 0x21:
        transform_direction(gpu, fixed9(p[0]), fixed9(p[0] >> 10),
                            fixed9(p[0] >> 20), gpu->normal);
        update_lighting(gpu);
        update_texcoord_from_normal(gpu);
        break;
    case 0x22:
        gpu->raw_texcoord[0] = (float)(int16_t)(p[0] & 0xffffu) / 16.0f;
        gpu->raw_texcoord[1] = (float)(int16_t)(p[0] >> 16) / 16.0f;
        update_texcoord_from_texcoord(gpu);
        break;
    case 0x23:
        gpu->vertex[0] = fixed16(p[0]);
        gpu->vertex[1] = fixed16(p[0] >> 16);
        gpu->vertex[2] = fixed16(p[1]);
        emit_vertex(gpu);
        break;
    case 0x24:
        gpu->vertex[0] = fixed10(p[0]);
        gpu->vertex[1] = fixed10(p[0] >> 10);
        gpu->vertex[2] = fixed10(p[0] >> 20);
        emit_vertex(gpu);
        break;
    case 0x25: gpu->vertex[0] = fixed16(p[0]); gpu->vertex[1] = fixed16(p[0] >> 16); emit_vertex(gpu); break;
    case 0x26: gpu->vertex[0] = fixed16(p[0]); gpu->vertex[2] = fixed16(p[0] >> 16); emit_vertex(gpu); break;
    case 0x27: gpu->vertex[1] = fixed16(p[0]); gpu->vertex[2] = fixed16(p[0] >> 16); emit_vertex(gpu); break;
    case 0x28:
        gpu->vertex[0] += fixed10_relative(p[0]);
        gpu->vertex[1] += fixed10_relative(p[0] >> 10);
        gpu->vertex[2] += fixed10_relative(p[0] >> 20);
        emit_vertex(gpu);
        break;
    case 0x29:
        gpu->polygon_pending = p[0];
        gpu->polygon_pending_valid = true;
        break;
    case 0x2a: gpu->texture = p[0]; break;
    case 0x2b: gpu->palette = p[0]; break;
    case 0x30:
        gpu->diffuse[0] = color5(p[0], 0u);
        gpu->diffuse[1] = color5(p[0], 5u);
        gpu->diffuse[2] = color5(p[0], 10u);
        gpu->ambient[0] = color5(p[0], 16u);
        gpu->ambient[1] = color5(p[0], 21u);
        gpu->ambient[2] = color5(p[0], 26u);
        /* Bit 15 selects the diffuse material as the current vertex color;
         * it does not execute lighting immediately.  Lighting is evaluated
         * by NORMAL, matching the DS command pipeline. */
        if (p[0] & (1u << 15)) {
            gpu->color[0] = gpu->diffuse[0];
            gpu->color[1] = gpu->diffuse[1];
            gpu->color[2] = gpu->diffuse[2];
        }
        break;
    case 0x31:
        gpu->specular[0] = color5(p[0], 0u);
        gpu->specular[1] = color5(p[0], 5u);
        gpu->specular[2] = color5(p[0], 10u);
        gpu->shininess_enabled = (p[0] & (1u << 15)) != 0u;
        gpu->emissive[0] = color5(p[0], 16u);
        gpu->emissive[1] = color5(p[0], 21u);
        gpu->emissive[2] = color5(p[0], 26u);
        break;
    case 0x32: {
        const unsigned light = (p[0] >> 30) & 3u;
        transform_direction(gpu, fixed9(p[0]), fixed9(p[0] >> 10),
                            fixed9(p[0] >> 20), gpu->lights[light].direction);
        break;
    }
    case 0x33: {
        const unsigned light = (p[0] >> 30) & 3u;
        gpu->lights[light].color[0] = color5(p[0], 0u);
        gpu->lights[light].color[1] = color5(p[0], 5u);
        gpu->lights[light].color[2] = color5(p[0], 10u);
        break;
    }
    case 0x34:
        for (unsigned word = 0; word < 32u; ++word)
            for (unsigned byte = 0; byte < 4u; ++byte)
                gpu->shininess[word * 4u + byte] =
                    (uint8_t)(p[word] >> (byte * 8u));
        break;
    case 0x40:
        if (gpu->polygon_pending_valid) {
            gpu->polygon = gpu->polygon_pending;
            gpu->polygon_pending_valid = false;
        }
        gpu->primitive = p[0] & 3u;
        gpu->in_primitive = true;
        gpu->strip_count = 0u;
        gpu->strip_parity = 0u;
        break;
    case 0x41: gpu->in_primitive = false; gpu->strip_count = 0u; break;
    case 0x50:
        gpu->in_primitive = false;
        gpu->strip_count = 0u;
        /* SWAP_BUFFERS latches the completed geometry for the rendering
         * engine and starts a fresh geometry list. */
        {
            gpu->swap_attributes = p[0];
            for (size_t index = 0; index < gpu->triangle_count; ++index)
                gpu->triangles[index].w_buffer = (p[0] & 2u) != 0u;
            GpuTriangle *triangles = gpu->triangles;
            gpu->triangles = gpu->render_triangles;
            gpu->render_triangles = triangles;
            const size_t count = gpu->triangle_count;
            gpu->triangle_count = gpu->render_triangle_count;
            gpu->render_triangle_count = count;
            const size_t capacity = gpu->triangle_capacity;
            gpu->triangle_capacity = gpu->render_triangle_capacity;
            gpu->render_triangle_capacity = capacity;
            gpu->pending_triangles = gpu->render_triangles;
            gpu->pending_triangle_count = count;
            gpu->swap_pending = true;
            gpu->triangle_count = 0u;
            gpu->vertex_count = 0u;
        }
        break;
    case 0x70:
        execute_box_test(gpu, p);
        break;
    case 0x71: {
        gpu->vertex[0] = fixed16(p[0]);
        gpu->vertex[1] = fixed16(p[0] >> 16);
        gpu->vertex[2] = fixed16(p[1]);
        float clip[4];
        transform_position(gpu, gpu->vertex[0], gpu->vertex[1],
                           gpu->vertex[2], clip);
        for (unsigned index = 0; index < 4u; ++index)
            gpu->position_result[index] = fixed_result(clip[index]);
        break;
    }
    case 0x72: {
        float result[3];
        transform_direction(gpu, fixed9(p[0]), fixed9(p[0] >> 10),
                            fixed9(p[0] >> 20), result);
        for (unsigned index = 0; index < 3u; ++index)
            gpu->vector_result[index] = vector_result(result[index]);
        break;
    }
    case 0x60: gpu->viewport = p[0]; break;
    default:
        gpu->unsupported_commands++;
        break;
    }
}

static void pump_commands(NdsGpu *gpu) {
    while (gpu->parameters_left == 0u && gpu->command_count != 0u) {
        gpu->active_command = gpu->commands[gpu->command_head];
        gpu->command_head = (gpu->command_head + 1u) % GPU_CMD_QUEUE;
        gpu->command_count--;
        if (gpu->active_command == 0u)
            continue;
        gpu->parameters_left = command_parameters(gpu->active_command);
        gpu->parameter_index = 0u;
        gpu->command_count_total++;
        gpu->command_histogram[gpu->active_command]++;
        if (gpu->parameters_left == 0u)
            execute_command(gpu);
    }
}

NdsGpu *nds_gpu_create(void) {
    NdsGpu *gpu = calloc(1, sizeof(*gpu));
    if (gpu == NULL)
        return NULL;
    for (unsigned index = 0; index < 4u; ++index) {
        matrix_identity(gpu->matrix[index]);
        matrix_identity(gpu->stack[index][0]);
    }
    gpu->color[0] = gpu->color[1] = gpu->color[2] = gpu->color[3] = 1.0f;
    gpu->diffuse[0] = gpu->diffuse[1] = gpu->diffuse[2] = 1.0f;
    gpu->ambient[0] = gpu->ambient[1] = gpu->ambient[2] = 1.0f;
    gpu->viewport = 0xbfff00ffu;
    gpu->texture_generation = 1u;
    return gpu;
}

void nds_gpu_destroy(NdsGpu *gpu) {
    if (gpu == NULL)
        return;
    free(gpu->triangles);
    free(gpu->render_triangles);
    free(gpu->visible_triangles);
    free(gpu->texture_cache);
    free(gpu);
}

void nds_gpu_begin_frame(NdsGpu *gpu) {
    (void)gpu;
}

void nds_gpu_invalidate_textures(NdsGpu *gpu) {
    if (gpu == NULL)
        return;
    ++gpu->texture_generation;
    if (gpu->texture_generation == 0u)
        gpu->texture_generation = 1u;
}

void nds_gpu_set_1dot_depth(NdsGpu *gpu, uint16_t depth) {
    if (gpu != NULL)
        gpu->one_dot_depth = depth & 0x7fffu;
}

static void prune_texture_cache(NdsGpu *gpu) {
    size_t write = 0u;
    for (size_t index = 0; index < gpu->texture_cache_count; ++index) {
        GpuTextureCacheEntry *entry = &gpu->texture_cache[index];
        if (entry->generation != gpu->texture_generation) {
            if (entry->name != 0u)
                glDeleteTextures(1, &entry->name);
            continue;
        }
        if (write != index)
            gpu->texture_cache[write] = *entry;
        ++write;
    }
    gpu->texture_cache_count = write;
}

void nds_gpu_vblank(NdsGpu *gpu) {
    if (gpu == NULL || !gpu->swap_pending)
        return;
    size_t opaque_count = 0u;
    GpuTriangle *ordered = gpu->pending_triangle_count == 0u ? NULL :
        malloc(gpu->pending_triangle_count * sizeof(*ordered));
    if (ordered != NULL) {
        for (size_t index = 0; index < gpu->pending_triangle_count; ++index)
            if (!triangle_is_translucent(&gpu->pending_triangles[index]))
                ordered[opaque_count++] = gpu->pending_triangles[index];
        size_t translucent_count = opaque_count;
        for (size_t index = 0; index < gpu->pending_triangle_count; ++index)
            if (triangle_is_translucent(&gpu->pending_triangles[index]))
                ordered[translucent_count++] = gpu->pending_triangles[index];
        memcpy(gpu->pending_triangles, ordered,
               gpu->pending_triangle_count * sizeof(*ordered));
        free(ordered);
    }
    if ((gpu->swap_attributes & 1u) != 0u)
        sort_triangles(gpu->pending_triangles, gpu->pending_triangle_count,
                       opaque_count);
    else
        sort_triangles(gpu->pending_triangles, gpu->pending_triangle_count,
                       gpu->pending_triangle_count);
    if (gpu->pending_triangle_count > gpu->visible_triangle_capacity) {
        GpuTriangle *triangles = realloc(
            gpu->visible_triangles,
            gpu->pending_triangle_count * sizeof(*triangles));
        if (triangles == NULL)
            return;
        gpu->visible_triangles = triangles;
        gpu->visible_triangle_capacity = gpu->pending_triangle_count;
    }
    if (gpu->pending_triangle_count != 0u)
        memcpy(gpu->visible_triangles, gpu->pending_triangles,
               gpu->pending_triangle_count * sizeof(*gpu->visible_triangles));
    gpu->visible_triangle_count = gpu->pending_triangle_count;
    gpu->swap_pending = false;
}

void nds_gpu_write32(NdsGpu *gpu, uint32_t value) {
    if (gpu == NULL)
        return;
    if (gpu->parameters_left != 0u) {
        gpu->parameters[gpu->parameter_index++] = value;
        gpu->parameters_left--;
        if (gpu->parameters_left == 0u)
            execute_command(gpu);
        pump_commands(gpu);
        return;
    }
    for (unsigned index = 0; index < 4u; ++index) {
        if (gpu->command_count < GPU_CMD_QUEUE) {
            const unsigned tail = (gpu->command_head + gpu->command_count) % GPU_CMD_QUEUE;
            gpu->commands[tail] = (uint8_t)(value >> (index * 8u));
            gpu->command_count++;
        }
    }
    pump_commands(gpu);
}

void nds_gpu_write_port(NdsGpu *gpu, uint8_t command, uint32_t value) {
    if (gpu == NULL)
        return;
    const unsigned count = command_parameters(command);
    if (count == 0u) {
        gpu->active_command = command;
        gpu->command_count_total++;
        gpu->command_histogram[command]++;
        execute_command(gpu);
        return;
    }
    if (gpu->port_parameters_left == 0u || gpu->port_command != command) {
        gpu->port_command = command;
        gpu->port_parameters_left = count;
        gpu->port_parameter_index = 0u;
    }
    gpu->parameters[gpu->port_parameter_index++] = value;
    gpu->port_parameters_left--;
    if (gpu->port_parameters_left == 0u) {
        gpu->active_command = command;
        gpu->command_count_total++;
        gpu->command_histogram[command]++;
        execute_command(gpu);
    }
}

void nds_gpu_render(const NdsCpu *cpu, int screen_y) {
    const NdsGpu *gpu = cpu == NULL ? NULL : cpu->gpu;
    static bool debug_triangles_logged;
    if (!debug_triangles_logged && gpu != NULL &&
        gpu->visible_triangle_count != 0u) {
        for (size_t index = 0; index < gpu->visible_triangle_count; ++index) {
            const GpuTriangle *triangle = &gpu->visible_triangles[index];
            float min_z = 2.0f, max_z = -2.0f;
            float min_depth = 2.0f, max_depth = -2.0f;
            float min_x = 2.0f, max_x = -2.0f;
            float min_y = 2.0f, max_y = -2.0f;
            for (unsigned vertex = 0u; vertex < 3u; ++vertex) {
                const GpuVertex *point = &triangle->vertex[vertex];
                if (point->z < min_z) min_z = point->z;
                if (point->z > max_z) max_z = point->z;
                if (point->depth < min_depth) min_depth = point->depth;
                if (point->depth > max_depth) max_depth = point->depth;
                if (point->x < min_x) min_x = point->x;
                if (point->x > max_x) max_x = point->x;
                if (point->y < min_y) min_y = point->y;
                if (point->y > max_y) max_y = point->y;
            }
            fprintf(stderr, "tri %zu poly=%08x tex=%08x fmt=%u alpha=%u "
                            "wb=%u z=%g..%g d=%g..%g xy=%g,%g..%g,%g\n",
                    index, triangle->polygon, triangle->texture,
                    (triangle->texture >> 26) & 7u,
                    (triangle->polygon >> 16) & 31u,
                    triangle->w_buffer ? 1u : 0u, min_z, max_z,
                    min_depth, max_depth, min_x, min_y, max_x, max_y);
        }
        debug_triangles_logged = true;
    }
    if (gpu == NULL || !easygl2d_gl_begin())
        return;
    NdsGpu *cache_gpu = (NdsGpu *)gpu;
    prune_texture_cache(cache_gpu);
    glViewport(0, 0, 256, 192);
    const uint16_t display3d = nds_read16(cpu, UINT32_C(0x04000060));
    const uint32_t clear_color = nds_read32(cpu, UINT32_C(0x04000350));
    const uint16_t clear_depth = nds_read16(cpu, UINT32_C(0x04000354));
    const bool fog_enabled = (display3d & (1u << 7)) != 0u &&
                             (display3d & (1u << 6)) == 0u;
    if (fog_enabled) {
        const uint32_t fog_color = nds_read32(cpu, UINT32_C(0x04000358));
        const unsigned shift = (display3d >> 8) & 15u;
        const unsigned offset = nds_read16(cpu, UINT32_C(0x0400035c)) & 0x7fffu;
        unsigned end_index = 32u;
        for (unsigned index = 0; index < 32u; ++index) {
            if (nds_read8(cpu, UINT32_C(0x04000360) + index) >= 127u) {
                end_index = index;
                break;
            }
        }
        /* ponytail: linear fixed-function fog; use a fragment shader for the
         * exact 32-entry DS density curve when fog-heavy scenes need it. */
        const double step = (double)(UINT32_C(0x80000) >> shift);
        const double start = (double)offset * 0x200 / 16777215.0;
        const double end = ((double)offset * 0x200 + end_index * step) /
                          16777215.0;
        const GLfloat color[4] = {
            (GLfloat)(fog_color & 31u) / 31.0f,
            (GLfloat)((fog_color >> 5) & 31u) / 31.0f,
            (GLfloat)((fog_color >> 10) & 31u) / 31.0f,
            (GLfloat)((fog_color >> 16) & 31u) / 31.0f
        };
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogi(GL_FOG_COORDINATE_SOURCE, GL_FRAGMENT_DEPTH);
        glFogfv(GL_FOG_COLOR, color);
        glFogf(GL_FOG_START, (GLfloat)start);
        glFogf(GL_FOG_END, (GLfloat)(end > start ? end : 1.0));
    }
    bool has_shadow_mask = false;
    for (size_t index = 0; index < gpu->visible_triangle_count; ++index)
        if ((gpu->visible_triangles[index].polygon & 0x3f000030u) == 0x30u)
            has_shadow_mask = true;
    GLint stencil_bits = 0;
    glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);
    has_shadow_mask = has_shadow_mask && stencil_bits != 0;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const unsigned clear_r = (clear_color & 31u) == 0u ? 0u :
                              (clear_color & 31u) * 2u + 1u;
    const unsigned clear_g = ((clear_color >> 5) & 31u) == 0u ? 0u :
                              ((clear_color >> 5) & 31u) * 2u + 1u;
    const unsigned clear_b = ((clear_color >> 10) & 31u) == 0u ? 0u :
                              ((clear_color >> 10) & 31u) * 2u + 1u;
    glClearColor((float)clear_r / 63.0f,
                 (float)clear_g / 63.0f,
                 (float)clear_b / 63.0f,
                 (float)((clear_color >> 16) & 31u) / 31.0f);
    /* CLEAR_DEPTH expands to X*0x200 plus the final 0x1ff only for X=0x7fff. */
    const uint32_t clear_depth24 =
        ((uint32_t)(clear_depth & 0x7fffu) << 9) |
        (clear_depth == 0x7fffu ? 0x1ffu : 0u);
    glClearDepth((double)clear_depth24 / 16777215.0);
    if (has_shadow_mask) {
        glClearStencil(0);
        glStencilMask(0xff);
        glEnable(GL_STENCIL_TEST);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
            (has_shadow_mask ? GL_STENCIL_BUFFER_BIT : 0));
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    if ((display3d & (1u << 4)) != 0u) {
        glEnable(GL_POLYGON_SMOOTH);
        glHint(GL_POLYGON_SMOOTH_HINT, GL_NICEST);
    } else {
        glDisable(GL_POLYGON_SMOOTH);
    }
    if (display3d & (1u << 2)) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER,
                    (float)nds_read8(cpu, UINT32_C(0x04000340)) / 31.0f);
    } else {
        glDisable(GL_ALPHA_TEST);
    }
    for (unsigned draw_pass = 0; draw_pass < 4u; ++draw_pass)
    for (size_t index = 0; index < gpu->visible_triangle_count; ++index) {
        const GpuTriangle *triangle = &gpu->visible_triangles[index];
        const bool shadow_mask =
            (triangle->polygon & 0x3f000030u) == 0x30u;
        const bool shadow = (triangle->polygon & 0x30u) == 0x30u &&
                            !shadow_mask;
        /* SM64 uses large unlit quads as same-depth compositor layers.  Keep
         * that narrow case in the rear pass; ordinary unlit geometry must
         * remain depth-tested. */
        const bool rear_plane = !shadow_mask && !shadow &&
                                (triangle->polygon & 0x0fu) == 0u &&
                                fullscreen_composite(triangle);
        if (!has_shadow_mask && (shadow_mask || shadow))
            continue;
        const unsigned triangle_pass = shadow_mask ? 2u :
                                       (shadow ? 3u : (rear_plane ? 0u : 1u));
        if (draw_pass != triangle_pass)
            continue;
        const unsigned polygon_alpha = (triangle->polygon >> 16) & 31u;
        const unsigned polygon_mode = (triangle->polygon >> 4) & 3u;
        const unsigned format = (triangle->texture >> 26) & 7u;
        bool textured = (display3d & 1u) != 0u && format != 0u &&
                        polygon_mode != 3u;
        const bool texture_alpha = textured &&
            (format == 1u || format == 5u || format == 6u || format == 7u ||
             (triangle->texture & (1u << 29)) != 0u);
        GpuTextureCacheEntry *texture_entry = NULL;
        if (textured) {
            size_t cache_index = 0u;
            while (cache_index < cache_gpu->texture_cache_count &&
                   (cache_gpu->texture_cache[cache_index].texture != triangle->texture ||
                    cache_gpu->texture_cache[cache_index].palette != triangle->palette))
                ++cache_index;
            if (cache_index == cache_gpu->texture_cache_count &&
                cache_gpu->texture_cache_count == cache_gpu->texture_cache_capacity) {
                size_t capacity = cache_gpu->texture_cache_capacity == 0u ? 32u :
                                   cache_gpu->texture_cache_capacity * 2u;
                GpuTextureCacheEntry *entries = realloc(
                    cache_gpu->texture_cache, capacity * sizeof(*entries));
                if (entries != NULL) {
                    cache_gpu->texture_cache = entries;
                    cache_gpu->texture_cache_capacity = capacity;
                }
            }
            if (cache_index == cache_gpu->texture_cache_count &&
                cache_gpu->texture_cache_count < cache_gpu->texture_cache_capacity) {
                GpuTextureCacheEntry *entry =
                    &cache_gpu->texture_cache[cache_gpu->texture_cache_count++];
                entry->texture = triangle->texture;
                entry->palette = triangle->palette;
                entry->generation = cache_gpu->texture_generation;
                glGenTextures(1, &entry->name);
                entry->ready = upload_texture(cpu, triangle, entry->name);
                cache_gpu->texture_upload_count++;
            }
            if (cache_index < cache_gpu->texture_cache_count)
                texture_entry = &cache_gpu->texture_cache[cache_index];
        }
        textured = textured && texture_entry != NULL && texture_entry->ready;
        if (textured)
            glBindTexture(GL_TEXTURE_2D, texture_entry->name);
        if (textured)
            glEnable(GL_TEXTURE_2D);
        else
            glDisable(GL_TEXTURE_2D);
        if (polygon_mode == 1u)
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
        else
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        /* The DS accepts front-facing opaque pixels at the same Z as the
         * existing pixel.  GL_LEQUAL is the closest fixed-function match;
         * GL_LESS drops SM64's same-depth fullscreen compositing passes. */
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilMask(0);
        glStencilFunc(GL_ALWAYS, 0, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        if (shadow_mask) {
            /* ponytail: one stencil bit models both DS top/bottom shadow
             * buffers; split stencil bits when shadow-heavy scenes require
             * exact receiver selection. */
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glStencilMask(0xff);
            glStencilFunc(GL_ALWAYS, 0, 0xff);
            glStencilOp(GL_KEEP, GL_INCR, GL_KEEP);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_FALSE);
        } else if (shadow) {
            glStencilFunc(GL_NOTEQUAL, 0, 0xff);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_FALSE);
        } else {
            const bool equal_depth = (triangle->polygon & (1u << 14)) != 0u;
            glDepthFunc(rear_plane ? GL_ALWAYS :
                        (equal_depth ? GL_EQUAL : GL_LEQUAL));
            glDepthMask(!rear_plane && (polygon_alpha == 31u ||
                        (triangle->polygon & (1u << 11)) != 0u));
        }
        if (fog_enabled && (triangle->polygon & (1u << 15)) != 0u)
            glEnable(GL_FOG);
        else
            glDisable(GL_FOG);
        if (shadow || texture_alpha || ((display3d & (1u << 3)) != 0u &&
            (polygon_alpha != 31u || textured))
            )
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
        if (polygon_alpha == 0u) {
            glDisable(GL_TEXTURE_2D);
            glBegin(GL_LINE_LOOP);
        } else {
            glBegin(GL_TRIANGLES);
        }
        for (unsigned vertex = 0; vertex < 3u; ++vertex) {
            const GpuVertex *point = &triangle->vertex[vertex];
            float color[3] = {point->r, point->g, point->b};
            if (polygon_mode == 2u) {
                float toon[3];
                toon_color(cpu, point, toon);
                if ((display3d & (1u << 1)) == 0u) {
                    color[0] = toon[0];
                    color[1] = toon[1];
                    color[2] = toon[2];
                } else {
                    color[1] = color[2] = color[0];
                    color[0] += toon[0];
                    color[1] += toon[1];
                    color[2] += toon[2];
                    if (color[0] > 1.0f) color[0] = 1.0f;
                    if (color[1] > 1.0f) color[1] = 1.0f;
                    if (color[2] > 1.0f) color[2] = 1.0f;
                }
            }
            if (polygon_mode == 3u) {
                color[0] = color[1] = color[2] = 0.0f;
            }
            glColor4f(color[0], color[1], color[2],
                      polygon_alpha == 0u ? 1.0f : point->a);
            if (textured && polygon_alpha != 0u)
                glTexCoord2f(point->u / (float)(8u << ((triangle->texture >> 20) & 7u)),
                             1.0f - point->v / (float)(8u << ((triangle->texture >> 23) & 7u)));
            /* Reconstruct clip-space coordinates with their original W.
             * This preserves perspective-correct texture interpolation; the
             * prior glVertex3f path forced W=1 after DS division. */
            const float depth = triangle->w_buffer ? point->depth * 2.0f - 1.0f :
                                point->z;
            glVertex4f(point->x * point->w, point->y * point->w,
                       depth * point->w, point->w);
        }
        glEnd();
    }
    if ((display3d & (1u << 5)) != 0u) {
        /* OpenGL 2.1 has no DS polygon-ID buffer.  Drawing the submitted
           opaque triangle boundaries gives the closest fixed-function edge
           marking while preserving depth ordering and the DS edge table. */
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_FOG);
        glDisable(GL_BLEND);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glLineWidth(1.0f);
        for (size_t index = 0; index < gpu->visible_triangle_count; ++index) {
            const GpuTriangle *triangle = &gpu->visible_triangles[index];
            const unsigned alpha = (triangle->polygon >> 16) & 31u;
            if (alpha != 31u || (triangle->polygon & 0x30u) == 0x30u)
                continue;
            float color[3];
            edge_color(cpu, triangle, color);
            glColor4f(color[0], color[1], color[2], 1.0f);
            glBegin(GL_LINE_LOOP);
            for (unsigned vertex = 0; vertex < 3u; ++vertex) {
                const GpuVertex *point = &triangle->vertex[vertex];
                const float depth = triangle->w_buffer ?
                    point->depth * 2.0f - 1.0f : point->z;
                glVertex4f(point->x * point->w, point->y * point->w,
                           depth * point->w, point->w);
            }
            glEnd();
        }
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_POLYGON_SMOOTH);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    easygl2d_gl_end(screen_y);
}

size_t nds_gpu_triangle_count(const NdsGpu *gpu) {
    return gpu == NULL ? 0u : gpu->visible_triangle_count;
}

uint64_t nds_gpu_command_count(const NdsGpu *gpu) {
    return gpu == NULL ? 0u : gpu->command_count_total;
}

uint64_t nds_gpu_unsupported_count(const NdsGpu *gpu) {
    return gpu == NULL ? 0u : gpu->unsupported_commands;
}

uint32_t nds_gpu_command_histogram(const NdsGpu *gpu, unsigned command) {
    return gpu == NULL || command >= 256u ? 0u : gpu->command_histogram[command];
}

unsigned nds_gpu_max_texture_pixels(const NdsGpu *gpu) {
    unsigned maximum = 0u;
    if (gpu == NULL)
        return 0u;
    for (size_t index = 0; index < gpu->visible_triangle_count; ++index) {
        const GpuTriangle *triangle = &gpu->visible_triangles[index];
        const unsigned format = (triangle->texture >> 26) & 7u;
        if (format == 0u)
            continue;
        const unsigned width = 8u << ((triangle->texture >> 20) & 7u);
        const unsigned height = 8u << ((triangle->texture >> 23) & 7u);
        if (width * height > maximum)
            maximum = width * height;
    }
    return maximum;
}

size_t nds_gpu_texture_format_count(const NdsGpu *gpu, unsigned format) {
    size_t count = 0u;
    if (gpu == NULL || format >= 8u)
        return 0u;
    for (size_t index = 0; index < gpu->visible_triangle_count; ++index)
        count += ((gpu->visible_triangles[index].texture >> 26) & 7u) == format;
    return count;
}

size_t nds_gpu_unique_texture_count(const NdsGpu *gpu, unsigned format) {
    size_t count = 0u;
    if (gpu == NULL || format >= 8u)
        return 0u;
    for (size_t index = 0; index < gpu->visible_triangle_count; ++index) {
        const GpuTriangle *triangle = &gpu->visible_triangles[index];
        if (((triangle->texture >> 26) & 7u) != format)
            continue;
        size_t previous = 0u;
        while (previous < index &&
               (gpu->visible_triangles[previous].texture != triangle->texture ||
                gpu->visible_triangles[previous].palette != triangle->palette))
            ++previous;
        count += previous == index;
    }
    return count;
}

uint64_t nds_gpu_texture_upload_count(const NdsGpu *gpu) {
    return gpu == NULL ? 0u : gpu->texture_upload_count;
}

size_t nds_gpu_texture_cache_count(const NdsGpu *gpu) {
    return gpu == NULL ? 0u : gpu->texture_cache_count;
}

uint32_t nds_gpu_read_status(const NdsGpu *gpu) {
    if (gpu == NULL)
        return 0u;
    uint32_t status = 1u | (gpu->box_test_result ? 2u : 0u);
    status |= (gpu->stack_top[1] & 31u) << 8;
    status |= (gpu->stack_top[0] & 1u) << 13;
    if (gpu->stack_error)
        status |= 1u << 15;
    const unsigned fifo = gpu->command_count > 0x1ffu ? 0x1ffu : gpu->command_count;
    status |= fifo << 16;
    if (fifo >= 0x100u)
        status |= 1u << 24;
    if (fifo < 0x80u)
        status |= 1u << 25;
    if (fifo == 0u)
        status |= 1u << 26;
    if (fifo != 0u || gpu->parameters_left != 0u)
        status |= 1u << 27;
    status |= (gpu->fifo_irq_mode & 3u) << 30;
    return status;
}

uint32_t nds_gpu_read_ram_count(const NdsGpu *gpu) {
    if (gpu == NULL)
        return 0u;
    const size_t polygons = gpu->triangle_count > 2048u ? 2048u :
                            gpu->triangle_count;
    const size_t vertices = gpu->vertex_count > 6144u ? 6144u :
                            gpu->vertex_count;
    return (uint32_t)polygons | ((uint32_t)vertices << 16);
}

uint32_t nds_gpu_read_position_result(const NdsGpu *gpu, unsigned index) {
    return gpu == NULL || index >= 4u ? 0u : gpu->position_result[index];
}

uint16_t nds_gpu_read_vector_result(const NdsGpu *gpu, unsigned index) {
    return gpu == NULL || index >= 3u ? 0u : gpu->vector_result[index];
}

uint32_t nds_gpu_read_clip_matrix(const NdsGpu *gpu, unsigned index) {
    if (gpu == NULL || index >= 16u)
        return 0u;
    float matrix[16];
    matrix_multiply(matrix, gpu->matrix[0], gpu->matrix[1]);
    return fixed_result(matrix[index]);
}

uint32_t nds_gpu_read_vector_matrix(const NdsGpu *gpu, unsigned index) {
    if (gpu == NULL || index >= 9u)
        return 0u;
    const unsigned row = index / 3u;
    const unsigned column = index % 3u;
    return fixed_result(gpu->matrix[2][column * 4u + row]);
}

void nds_gpu_write_status(NdsGpu *gpu, uint32_t value) {
    if (gpu == NULL)
        return;
    if (value & (1u << 15))
        gpu->stack_error = false;
    gpu->fifo_irq_mode = (value >> 30) & 3u;
}
