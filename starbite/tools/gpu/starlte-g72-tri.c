/*
 * Project Starbite: the iOS kernel's first Mali-G72 draw, built offline with
 * Mesa 25.0.7 (MIT) so every descriptor is packed by Mesa's own v6 genxml
 * pack macros and the fragment shader comes from Mesa's Bifrost compiler.
 *
 * One tiler job (no vertex job: screen-space positions straight into the
 * tiler, as Mesa's fb preload does on Midgard) and one fragment job that
 * clears a 512x512 render target on the panel to dark grey and shades a
 * magenta triangle. Output: a C header with the descriptor/shader blob and
 * its GPU addresses, for xnu/src/starlte_gpu.c. Both job chains are run
 * through pandecode first.
 *
 * Phase 5 adds a second blob, the GPU present: iOS's 1242x2208 canvas
 * (sweetfb_lw/lh) as a texture on a quad scaled to the panel's full height,
 * bilinear, between black bars, as SweetFramebuffer's scaledCopy() lays it
 * out. Per frame the kernel patches only the surface pointer and strides.
 *
 * Built and run in the lima VM by tools/gpu/build-g72-tri.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir_builder.h"
#include "panfrost/lib/pan_shader.h"
#include "panfrost/lib/pan_encoder.h"
#include "panfrost/lib/pan_util.h"
#include "panfrost/lib/pan_format.h"
#include "panfrost/lib/wrap.h"
#include "panfrost/compiler/bifrost/disassemble.h"
#include "util/u_dynarray.h"
#include "genxml/gen_macros.h"

#define GPU_ID          0x6221u         /* Mali-G72 product ID (GPU_ID >> 16; r0p1 here) */

/* Must match xnu/src/starlte_gpu.c. */
#define BLOB_VA         0x02008000ull   /* SG_VA + data offset 0x8000 */
#define BLOB_BYTES      0x2000u
#define HEAP_VA         0x10000000ull
#define HEAP_BYTES      0x01000000u     /* 16 MiB, pre-mapped (no growable heap) */
#define PANEL_VA        0x04000000ull
#define PANEL_STRIDE    5760u
#define RT_X            464u            /* (1440 - 512) / 2 */
#define RT_Y            1216u           /* about (2960 - 512) / 2, 16-aligned */
#define FB_W            512u
#define FB_H            512u

/* Blob layout (offsets from BLOB_VA). */
#define O_TILER_JOB     0x000u
#define O_FRAG_JOB      0x100u
#define O_FBD           0x200u          /* Framebuffer 128 B + one RT 64 B */
#define O_TILER_CTX     0x300u
#define O_TILER_HEAP    0x3c0u
#define O_RSD           0x400u          /* Renderer State 64 B + Blend 16 B */
#define O_VIEWPORT      0x480u
#define O_TLS           0x4c0u
#define O_POS           0x500u
#define O_SAMPLES       0x540u          /* 256 B */
#define O_SHADER        0x800u

static uint8_t blob[BLOB_BYTES];

static void *
at(unsigned off)
{
   return blob + off;
}

static uint32_t
fbits(float f)
{
   uint32_t u;
   memcpy(&u, &f, 4);
   return u;
}

static void
compile_fs(struct util_dynarray *bin, struct pan_shader_info *info)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, GENX(pan_shader_get_compiler_options)(), "starbite_tri");
   nir_def *magenta = nir_imm_vec4(&b, 1.0, 0.0, 1.0, 1.0);
   nir_store_output(&b, magenta, nir_imm_int(&b, 0), .base = 0,
                    .src_type = nir_type_float32,
                    .io_semantics.location = FRAG_RESULT_DATA0,
                    .io_semantics.num_slots = 1, .write_mask = 0xf);

   struct panfrost_compile_inputs inputs = {
      .gpu_id = GPU_ID,
      .is_blit = true,          /* no ATEST: Z/S forced early, as Mesa's preload */
      .no_idvs = true,
   };
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   pan_shader_preprocess(b.shader, inputs.gpu_id);
   util_dynarray_init(bin, NULL);
   GENX(pan_shader_compile)(b.shader, &inputs, bin, info);
}

/* ---- phase 5: the present blob ---------------------------------------- */
#ifndef CANVAS_W                        /* boot-args sweetfb_lw / sweetfb_lh; -DCANVAS_W=828u -DCANVAS_H=1792u for iOS 26 */
#define CANVAS_W        1242u
#endif
#ifndef CANVAS_H
#define CANVAS_H        2208u
#endif
#define PANEL_W         1440u
#define PANEL_H         2960u
#define P_BLOB_VA       0x0200c000ull   /* SG_VA + data offset 0xc000 */
#define P_BLOB_BYTES    0x4000u
#define P_SURF_VA       0x20000000ull   /* first surface slot (pandecode only) */
#define P_TILER_JOB     0x000u
#define P_FRAG_JOB      0x100u
#define P_FBD           0x200u
#define P_TILER_CTX     0x300u
#define P_TILER_HEAP    0x3c0u
#define P_RSD           0x400u
#define P_VIEWPORT      0x480u
#define P_TLS           0x4c0u
#define P_POS           0x500u          /* 4 x vec4 */
#define P_VARY_DATA     0x540u          /* 4 x vec4 texel coordinates */
#define P_VARY          0x580u          /* Attribute */
#define P_VARY_BUF      0x5c0u          /* 2 Attribute Buffers (+ padding one) */
#define P_SAMPLER       0x600u
#define P_TEXTURE       0x640u
#define P_SURFACE       0x680u          /* Surface With Stride: patched per frame */
#define P_SAMPLES       0x700u
#define P_SHADER        0x800u

static uint8_t pblob[P_BLOB_BYTES];

static void *
pat(unsigned off)
{
   return pblob + off;
}

static void
compile_tex_fs(struct util_dynarray *bin, struct pan_shader_info *info)
{
   /* As Mesa's pan_preload shader: texel coordinates from varying 0, one
    * explicit-LOD texture fetch, written to RT 0. */
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, GENX(pan_shader_get_compiler_options)(), "starbite_present");
   nir_def *bary = nir_load_barycentric(&b, nir_intrinsic_load_barycentric_pixel,
                                        INTERP_MODE_SMOOTH);
   nir_def *coord = nir_load_interpolated_input(
      &b, 2, 32, bary, nir_imm_int(&b, 0), .base = 0, .dest_type = nir_type_float32,
      .io_semantics.location = VARYING_SLOT_VAR0, .io_semantics.num_slots = 1);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 2);
   tex->op = nir_texop_txl;
   tex->dest_type = nir_type_float32;
   tex->texture_index = 0;
   tex->sampler_index = 0;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_float(&b, 0.0));
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_store_output(&b, &tex->def, nir_imm_int(&b, 0), .base = 0,
                    .src_type = nir_type_float32,
                    .io_semantics.location = FRAG_RESULT_DATA0,
                    .io_semantics.num_slots = 1, .write_mask = 0xf);

   struct panfrost_compile_inputs inputs = {
      .gpu_id = GPU_ID,
      .is_blit = true,
      .no_idvs = true,
   };
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   BITSET_SET(b.shader->info.textures_used, 0);
   pan_shader_preprocess(b.shader, inputs.gpu_id);
   util_dynarray_init(bin, NULL);
   GENX(pan_shader_compile)(b.shader, &inputs, bin, info);
}

static unsigned
gen_present(void)
{
   struct util_dynarray bin;
   struct pan_shader_info info;
   memset(&info, 0, sizeof(info));
   compile_tex_fs(&bin, &info);
   if (P_SHADER + bin.size > P_BLOB_BYTES) {
      fprintf(stderr, "present shader too big: %u\n", bin.size);
      exit(1);
   }
   memcpy(pat(P_SHADER), bin.data, bin.size);
   fprintf(stderr, "present shader: %u bytes, work regs %u, preload %#llx, textures %u "
           "samplers %u varyings %u\n", bin.size, info.work_reg_count,
           (unsigned long long)info.preload, info.texture_count, info.sampler_count,
           info.varyings.input_count);

   /* The canvas keeps its aspect, centred, as scaledCopy() lays it out: at full panel height
    * between bars at the sides when it is narrower than the panel (828x1792), at full panel
    * width between bars above and below when it is wider (1242x2208: 1440x2560, 200 rows each). */
   unsigned x0 = 0, x1 = PANEL_W, y0 = 0, y1 = PANEL_H;
   if (CANVAS_W * PANEL_H <= PANEL_W * CANVAS_H) {
      unsigned out_w = CANVAS_W * PANEL_H / CANVAS_H;
      x0 = (PANEL_W - out_w) / 2, x1 = x0 + out_w;
   } else {
      unsigned out_h = CANVAS_H * PANEL_W / CANVAS_W;
      y0 = (PANEL_H - out_h) / 2, y1 = y0 + out_h;
   }
   float pos[] = {
      x0, y0, 0, 1,
      x1, y0, 0, 1,
      x0, y1, 0, 1,
      x1, y1, 0, 1,
   };
   memcpy(pat(P_POS), pos, sizeof(pos));
   float tc[] = {
      0, 0, 0, 1,
      CANVAS_W, 0, 0, 1,
      0, CANVAS_H, 0, 1,
      CANVAS_W, CANVAS_H, 0, 1,
   };
   memcpy(pat(P_VARY_DATA), tc, sizeof(tc));
   uint16_t *sp = pat(P_SAMPLES);
   sp[0] = sp[1] = 128;
   sp[64] = sp[65] = 128;

   pan_cast_and_pack(pat(P_VARY), ATTRIBUTE, cfg) {
      cfg.buffer_index = 0;
      cfg.offset_enable = false;
      cfg.format = GENX(panfrost_format_from_pipe_format)(PIPE_FORMAT_R32G32B32_FLOAT)->hw;
   }
   pan_cast_and_pack(pat(P_VARY_BUF), ATTRIBUTE_BUFFER, cfg) {
      cfg.pointer = P_BLOB_VA + P_VARY_DATA;
      cfg.stride = 16;
      cfg.size = 16 * 4;
   }
   /* Bifrost needs an empty descriptor to end prefetching (Mesa). */
   pan_cast_and_pack(pat(P_VARY_BUF + pan_size(ATTRIBUTE_BUFFER)), ATTRIBUTE_BUFFER, cfg)
      ;
   pan_cast_and_pack(pat(P_SAMPLER), SAMPLER, cfg) {
      cfg.seamless_cube_map = false;
      cfg.normalized_coordinates = false;
      cfg.minify_nearest = false;       /* bilinear */
      cfg.magnify_nearest = false;
   }
   /* Linear RGBA8: bytes pass through unchanged (iOS's BGRA stays BGRA on
    * the BGRA panel), as the render target below. */
   pan_cast_and_pack(pat(P_TEXTURE), TEXTURE, cfg) {
      cfg.dimension = MALI_TEXTURE_DIMENSION_2D;
      cfg.format = GENX(panfrost_format_from_pipe_format)(PIPE_FORMAT_R8G8B8A8_UNORM)->hw;
      cfg.width = CANVAS_W;
      cfg.height = CANVAS_H;
      cfg.swizzle = 0x688;
      cfg.texel_ordering = MALI_TEXTURE_LAYOUT_LINEAR;
      cfg.levels = 1;
      cfg.array_size = 1;
      cfg.surfaces = P_BLOB_VA + P_SURFACE;
      cfg.minimum_lod = 0;
      cfg.maximum_lod = 0;
   }
   pan_cast_and_pack(pat(P_SURFACE), SURFACE_WITH_STRIDE, cfg) {
      cfg.pointer = P_SURF_VA;
      cfg.row_stride = CANVAS_W * 4;
      cfg.surface_stride = CANVAS_W * 4 * CANVAS_H;
   }

   pan_cast_and_pack(pat(P_TLS), LOCAL_STORAGE, cfg) {
      cfg.tls_size = 0;
   }
   pan_cast_and_pack(pat(P_VIEWPORT), VIEWPORT, cfg) {
      cfg.scissor_maximum_x = PANEL_W - 1;
      cfg.scissor_maximum_y = PANEL_H - 1;
   }
   pan_cast_and_pack(pat(P_RSD), RENDERER_STATE, cfg) {
      pan_shader_prepare_rsd(&info, P_BLOB_VA + P_SHADER, &cfg);
      cfg.multisample_misc.sample_mask = 0xFFFF;
      cfg.multisample_misc.depth_function = MALI_FUNC_ALWAYS;
      cfg.stencil_mask_misc.stencil_mask_front = 0xFF;
      cfg.stencil_mask_misc.stencil_mask_back = 0xFF;
      cfg.stencil_front.compare_function = MALI_FUNC_ALWAYS;
      cfg.stencil_front.stencil_fail = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.depth_fail = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.depth_pass = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.mask = 0xFF;
      cfg.stencil_back = cfg.stencil_front;
      cfg.properties.zs_update_operation = MALI_PIXEL_KILL_FORCE_EARLY;
      cfg.properties.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_EARLY;
      cfg.properties.allow_forward_pixel_to_kill = true;
      cfg.properties.allow_forward_pixel_to_be_killed = true;
   }
   pan_cast_and_pack(pat(P_RSD + pan_size(RENDERER_STATE)), BLEND, cfg) {
      cfg.round_to_fb_precision = true;
      cfg.internal.mode = MALI_BLEND_MODE_OPAQUE;
      cfg.equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
      cfg.equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
      cfg.equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
      cfg.equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
      cfg.equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
      cfg.equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
      cfg.equation.color_mask = 0xf;
      cfg.internal.fixed_function.num_comps = 4;
      cfg.internal.fixed_function.conversion.memory_format =
         GENX(panfrost_dithered_format_from_pipe_format)(PIPE_FORMAT_R8G8B8A8_UNORM, false);
      cfg.internal.fixed_function.conversion.register_format = MALI_REGISTER_FILE_FORMAT_F32;
      cfg.internal.fixed_function.rt = 0;
   }

   pan_cast_and_pack(pat(P_TILER_HEAP), TILER_HEAP, heap) {
      heap.size = HEAP_BYTES;
      heap.base = HEAP_VA;
      heap.bottom = HEAP_VA;
      heap.top = HEAP_VA + HEAP_BYTES;
   }
   pan_cast_and_pack(pat(P_TILER_CTX), TILER_CONTEXT, tiler) {
      tiler.hierarchy_mask = pan_select_tiler_hierarchy_mask(PANEL_W, PANEL_H, 8);
      tiler.fb_width = PANEL_W;
      tiler.fb_height = PANEL_H;
      tiler.heap = P_BLOB_VA + P_TILER_HEAP;
      tiler.sample_pattern = MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
   }

   void *tj = pat(P_TILER_JOB);
   pan_section_pack(tj, TILER_JOB, HEADER, cfg) {
      cfg.type = MALI_JOB_TYPE_TILER;
      cfg.index = 1;
   }
   panfrost_pack_work_groups_compute(pan_section_ptr(tj, TILER_JOB, INVOCATION), 1, 4, 1, 1,
                                     1, 1, true, false);
   pan_section_pack(tj, TILER_JOB, PRIMITIVE, cfg) {
      cfg.draw_mode = MALI_DRAW_MODE_TRIANGLE_STRIP;
      cfg.index_count = 4;
      cfg.job_task_split = 6;
   }
   pan_section_pack(tj, TILER_JOB, PRIMITIVE_SIZE, cfg) {
      cfg.constant = 1.0f;
   }
   pan_section_pack(tj, TILER_JOB, TILER, cfg) {
      cfg.address = P_BLOB_VA + P_TILER_CTX;
   }
   pan_section_pack(tj, TILER_JOB, PADDING, cfg)
      ;
   pan_section_pack(tj, TILER_JOB, DRAW, cfg) {
      cfg.position = P_BLOB_VA + P_POS;
      cfg.state = P_BLOB_VA + P_RSD;
      cfg.viewport = P_BLOB_VA + P_VIEWPORT;
      cfg.thread_storage = P_BLOB_VA + P_TLS;
      cfg.varyings = P_BLOB_VA + P_VARY;
      cfg.varying_buffers = P_BLOB_VA + P_VARY_BUF;
      cfg.textures = P_BLOB_VA + P_TEXTURE;
      cfg.samplers = P_BLOB_VA + P_SAMPLER;
   }

   void *fbd = pat(P_FBD);
   pan_section_pack(fbd, FRAMEBUFFER, PARAMETERS, cfg) {
      cfg.sample_locations = P_BLOB_VA + P_SAMPLES;
      cfg.tiler = P_BLOB_VA + P_TILER_CTX;
      cfg.width = PANEL_W;
      cfg.height = PANEL_H;
      cfg.bound_max_x = PANEL_W - 1;
      cfg.bound_max_y = PANEL_H - 1;
      cfg.effective_tile_size = 16 * 16;
      cfg.tie_break_rule = MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT;
      cfg.render_target_count = 1;
      cfg.z_internal_format = MALI_Z_INTERNAL_FORMAT_D24;
      cfg.color_buffer_allocation = 1024;
      cfg.sample_count = 1;
      cfg.sample_pattern = MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
   }
   pan_section_pack(fbd, FRAMEBUFFER, PADDING, cfg)
      ;
   pan_cast_and_pack(pat(P_FBD + pan_size(FRAMEBUFFER)), RENDER_TARGET, cfg) {
      cfg.clean_pixel_write_enable = true;
      cfg.internal_format = MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A8;
      cfg.write_enable = true;
      cfg.writeback_format = MALI_COLOR_FORMAT_R8G8B8A8;
      cfg.writeback_block_format = MALI_BLOCK_FORMAT_LINEAR;
      cfg.dithering_enable = true;
      cfg.swizzle = 0x688;
      cfg.rgb.base = PANEL_VA;
      cfg.rgb.row_stride = PANEL_STRIDE;
      cfg.clear.color_0 = 0xff000000;   /* black bars */
   }
   void *fj = pat(P_FRAG_JOB);
   pan_section_pack(fj, FRAGMENT_JOB, HEADER, cfg) {
      cfg.type = MALI_JOB_TYPE_FRAGMENT;
      cfg.index = 1;
   }
   pan_section_pack(fj, FRAGMENT_JOB, PAYLOAD, cfg) {
      cfg.bound_max_x = (PANEL_W - 1) >> MALI_TILE_SHIFT;
      cfg.bound_max_y = (PANEL_H - 1) >> MALI_TILE_SHIFT;
      cfg.framebuffer = (P_BLOB_VA + P_FBD) | 1;
   }

   struct pandecode_context *ctx = pandecode_create_context(true);
   static uint8_t heap_dummy[0x10000], panel_dummy[0x10000], surf_dummy[0x10000];
   pandecode_inject_mmap(ctx, P_BLOB_VA, pblob, P_BLOB_BYTES, "present blob");
   pandecode_inject_mmap(ctx, HEAP_VA, heap_dummy, sizeof(heap_dummy), "heap");
   pandecode_inject_mmap(ctx, PANEL_VA, panel_dummy, sizeof(panel_dummy), "panel");
   pandecode_inject_mmap(ctx, P_SURF_VA, surf_dummy, sizeof(surf_dummy), "surface");
   fprintf(stderr, "==== present tiler chain ====\n");
   pandecode_jc(ctx, P_BLOB_VA + P_TILER_JOB, GPU_ID);
   fprintf(stderr, "==== present fragment chain ====\n");
   pandecode_jc(ctx, P_BLOB_VA + P_FRAG_JOB, GPU_ID);
   fprintf(stderr, "present quad x %u..%u y %u..%u of %ux%u\n", x0, x1, y0, y1, PANEL_W, PANEL_H);
   return (P_SHADER + bin.size + 3) / 4;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s OUT.h\n", argv[0]);
      return 1;
   }
   glsl_type_singleton_init_or_ref();

   struct util_dynarray bin;
   struct pan_shader_info info;
   memset(&info, 0, sizeof(info));
   compile_fs(&bin, &info);
   if (O_SHADER + bin.size > BLOB_BYTES) {
      fprintf(stderr, "shader too big: %u\n", bin.size);
      return 1;
   }
   memcpy(at(O_SHADER), bin.data, bin.size);
   fprintf(stderr, "fragment shader: %u bytes, work regs %u, preload %#llx\n", bin.size,
           info.work_reg_count, (unsigned long long)info.preload);
   /* The listing: BIFROST_MESA_DEBUG=shaders (the disassembler trips on the
    * prefetch padding after the last clause). */

   /* Positions: screen space within the 512x512 render target, w = 1. */
   float pos[] = {
      256, 48, 0, 1,
      464, 464, 0, 1,
      48, 464, 0, 1,
   };
   memcpy(at(O_POS), pos, sizeof(pos));

   /* Single-sampled positions (pan_samples.c): sample 0 and origin. */
   uint16_t *sp = at(O_SAMPLES);
   sp[0] = sp[1] = 128;
   sp[64] = sp[65] = 128;

   pan_cast_and_pack(at(O_TLS), LOCAL_STORAGE, cfg) {
      cfg.tls_size = 0;
   }

   pan_cast_and_pack(at(O_VIEWPORT), VIEWPORT, cfg) {
      cfg.scissor_minimum_x = 0;
      cfg.scissor_minimum_y = 0;
      cfg.scissor_maximum_x = FB_W - 1;
      cfg.scissor_maximum_y = FB_H - 1;
   }

   /* Renderer State + Blend, as pan_preload_emit_rsd / _blend (colour only). */
   pan_cast_and_pack(at(O_RSD), RENDERER_STATE, cfg) {
      pan_shader_prepare_rsd(&info, BLOB_VA + O_SHADER, &cfg);
      cfg.multisample_misc.sample_mask = 0xFFFF;
      cfg.multisample_misc.depth_function = MALI_FUNC_ALWAYS;
      cfg.stencil_mask_misc.stencil_mask_front = 0xFF;
      cfg.stencil_mask_misc.stencil_mask_back = 0xFF;
      cfg.stencil_front.compare_function = MALI_FUNC_ALWAYS;
      cfg.stencil_front.stencil_fail = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.depth_fail = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.depth_pass = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.mask = 0xFF;
      cfg.stencil_back = cfg.stencil_front;
      cfg.properties.zs_update_operation = MALI_PIXEL_KILL_FORCE_EARLY;
      cfg.properties.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_EARLY;
      cfg.properties.allow_forward_pixel_to_kill = true;
      cfg.properties.allow_forward_pixel_to_be_killed = true;
   }
   pan_cast_and_pack(at(O_RSD + pan_size(RENDERER_STATE)), BLEND, cfg) {
      cfg.round_to_fb_precision = true;
      cfg.srgb = false;
      cfg.internal.mode = MALI_BLEND_MODE_OPAQUE;
      cfg.equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
      cfg.equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
      cfg.equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
      cfg.equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
      cfg.equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
      cfg.equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
      cfg.equation.color_mask = 0xf;
      cfg.internal.fixed_function.num_comps = 4;
      cfg.internal.fixed_function.conversion.memory_format =
         GENX(panfrost_dithered_format_from_pipe_format)(PIPE_FORMAT_R8G8B8A8_UNORM, false);
      cfg.internal.fixed_function.conversion.register_format =
         MALI_REGISTER_FILE_FORMAT_F32;
      cfg.internal.fixed_function.rt = 0;
   }

   /* Tiler heap and context, as jm_emit_tiler_desc(). */
   pan_cast_and_pack(at(O_TILER_HEAP), TILER_HEAP, heap) {
      heap.size = HEAP_BYTES;
      heap.base = HEAP_VA;
      heap.bottom = HEAP_VA;
      heap.top = HEAP_VA + HEAP_BYTES;
   }
   unsigned max_levels = (0x809 >> 8) & 0xf;   /* TILER_FEATURES */
   pan_cast_and_pack(at(O_TILER_CTX), TILER_CONTEXT, tiler) {
      tiler.hierarchy_mask = pan_select_tiler_hierarchy_mask(FB_W, FB_H, max_levels);
      tiler.fb_width = FB_W;
      tiler.fb_height = FB_H;
      tiler.heap = BLOB_VA + O_TILER_HEAP;
      tiler.sample_pattern = MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
   }

   /* Tiler job: header, invocation (3 vertices), primitive, size, tiler, draw. */
   void *tj = at(O_TILER_JOB);
   pan_section_pack(tj, TILER_JOB, HEADER, cfg) {
      cfg.type = MALI_JOB_TYPE_TILER;
      cfg.index = 1;
   }
   panfrost_pack_work_groups_compute(pan_section_ptr(tj, TILER_JOB, INVOCATION), 1, 3, 1, 1,
                                     1, 1, true, false);
   pan_section_pack(tj, TILER_JOB, PRIMITIVE, cfg) {
      cfg.draw_mode = MALI_DRAW_MODE_TRIANGLES;
      cfg.index_count = 3;
      cfg.job_task_split = 6;
   }
   pan_section_pack(tj, TILER_JOB, PRIMITIVE_SIZE, cfg) {
      cfg.constant = 1.0f;
   }
   pan_section_pack(tj, TILER_JOB, TILER, cfg) {
      cfg.address = BLOB_VA + O_TILER_CTX;
   }
   pan_section_pack(tj, TILER_JOB, PADDING, cfg)
      ;
   pan_section_pack(tj, TILER_JOB, DRAW, cfg) {
      cfg.position = BLOB_VA + O_POS;
      cfg.state = BLOB_VA + O_RSD;
      cfg.viewport = BLOB_VA + O_VIEWPORT;
      cfg.thread_storage = BLOB_VA + O_TLS;
   }

   /* Framebuffer: as GENX(pan_emit_fbd) for one RGBA8 linear target with a
    * clear, 16x16 tiles, 1 KiB colour buffer, the tiler context above. */
   void *fbd = at(O_FBD);
   pan_section_pack(fbd, FRAMEBUFFER, PARAMETERS, cfg) {
      cfg.sample_locations = BLOB_VA + O_SAMPLES;
      cfg.tiler = BLOB_VA + O_TILER_CTX;
      cfg.width = FB_W;
      cfg.height = FB_H;
      cfg.bound_max_x = FB_W - 1;
      cfg.bound_max_y = FB_H - 1;
      cfg.effective_tile_size = 16 * 16;
      cfg.tie_break_rule = MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT;
      cfg.render_target_count = 1;
      cfg.z_internal_format = MALI_Z_INTERNAL_FORMAT_D24;
      cfg.color_buffer_allocation = 1024;
      cfg.sample_count = 1;
      cfg.sample_pattern = MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
   }
   pan_section_pack(fbd, FRAMEBUFFER, PADDING, cfg)
      ;
   pan_cast_and_pack(at(O_FBD + pan_size(FRAMEBUFFER)), RENDER_TARGET, cfg) {
      cfg.clean_pixel_write_enable = true;
      cfg.internal_format = MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A8;
      cfg.write_enable = true;
      cfg.writeback_format = MALI_COLOR_FORMAT_R8G8B8A8;
      cfg.writeback_block_format = MALI_BLOCK_FORMAT_LINEAR;
      cfg.dithering_enable = true;
      cfg.swizzle = 0x688;      /* R G B A */
      cfg.rgb.base = PANEL_VA + (uint64_t)RT_Y * PANEL_STRIDE + RT_X * 4u;
      cfg.rgb.row_stride = PANEL_STRIDE;
      cfg.clear.color_0 = 0xff262626;   /* dark grey */
   }

   void *fj = at(O_FRAG_JOB);
   pan_section_pack(fj, FRAGMENT_JOB, HEADER, cfg) {
      cfg.type = MALI_JOB_TYPE_FRAGMENT;
      cfg.index = 1;
   }
   pan_section_pack(fj, FRAGMENT_JOB, PAYLOAD, cfg) {
      cfg.bound_max_x = (FB_W - 1) >> MALI_TILE_SHIFT;
      cfg.bound_max_y = (FB_H - 1) >> MALI_TILE_SHIFT;
      cfg.framebuffer = (BLOB_VA + O_FBD) | 1;  /* MFBD, 1 RT, no ZS/CRC */
   }

   /* Check both chains with pandecode (blob, heap and panel as dummies). */
   struct pandecode_context *ctx = pandecode_create_context(true);
   static uint8_t heap_dummy[0x10000], panel_dummy[0x10000];
   pandecode_inject_mmap(ctx, BLOB_VA, blob, BLOB_BYTES, "starbite blob");
   pandecode_inject_mmap(ctx, HEAP_VA, heap_dummy, sizeof(heap_dummy), "heap");
   pandecode_inject_mmap(ctx, PANEL_VA, panel_dummy, sizeof(panel_dummy), "panel");
   fprintf(stderr, "==== tiler chain ====\n");
   pandecode_jc(ctx, BLOB_VA + O_TILER_JOB, GPU_ID);
   fprintf(stderr, "==== fragment chain ====\n");
   pandecode_jc(ctx, BLOB_VA + O_FRAG_JOB, GPU_ID);

   FILE *f = fopen(argv[1], "w");
   if (!f) {
      perror(argv[1]);
      return 1;
   }
   fprintf(f, "/* Generated by tools/gpu/starlte-g72-tri.c (Mesa 25.0.7, MIT). Do not edit. */\n");
   fprintf(f, "#define SG_TRI_BLOB_VA     0x%llxull\n", (unsigned long long)BLOB_VA);
   fprintf(f, "#define SG_TRI_HEAP_VA     0x%llxull\n", (unsigned long long)HEAP_VA);
   fprintf(f, "#define SG_TRI_HEAP_BYTES  0x%xu\n", HEAP_BYTES);
   fprintf(f, "#define SG_TRI_TILER_JOB   0x%xu\n", O_TILER_JOB);
   fprintf(f, "#define SG_TRI_FRAG_JOB    0x%xu\n", O_FRAG_JOB);
   fprintf(f, "#define SG_TRI_RT_X        %uu\n", RT_X);
   fprintf(f, "#define SG_TRI_RT_Y        %uu\n", RT_Y);
   fprintf(f, "#define SG_TRI_FB_W        %uu\n", FB_W);
   fprintf(f, "#define SG_TRI_FB_H        %uu\n", FB_H);
   fprintf(f, "#define SG_TRI_COLOR       0xff%02x%02x%02xu\n", 255, 0, 255);
   fprintf(f, "#define SG_TRI_CLEAR       0xff262626u\n");
   unsigned words = (O_SHADER + bin.size + 3) / 4;
   fprintf(f, "#define SG_TRI_BLOB_WORDS  %uu\n", words);
   fprintf(f, "static const uint32_t sg_tri_blob[SG_TRI_BLOB_WORDS] = {");
   for (unsigned i = 0; i < words; i++) {
      uint32_t w;
      memcpy(&w, blob + i * 4, 4);
      fprintf(f, "%s0x%08x,", i % 6 ? " " : "\n\t", w);
   }
   fprintf(f, "\n};\n");

   unsigned pwords = gen_present();
   fprintf(f, "\n/* Phase 5: the GPU present (iOS canvas scaled to the panel). */\n");
   fprintf(f, "#define SG_PRES_BLOB_VA    0x%llxull\n", (unsigned long long)P_BLOB_VA);
   fprintf(f, "#define SG_PRES_TILER_JOB  0x%xu\n", P_TILER_JOB);
   fprintf(f, "#define SG_PRES_FRAG_JOB   0x%xu\n", P_FRAG_JOB);
   fprintf(f, "#define SG_PRES_SURFACE    0x%xu   /* Surface With Stride: pointer, row, surface */\n",
           P_SURFACE);
   fprintf(f, "#define SG_PRES_CANVAS_W   %uu\n", CANVAS_W);
   fprintf(f, "#define SG_PRES_CANVAS_H   %uu\n", CANVAS_H);
   fprintf(f, "#define SG_PRES_BLOB_WORDS %uu\n", pwords);
   fprintf(f, "static const uint32_t sg_pres_blob[SG_PRES_BLOB_WORDS] = {");
   for (unsigned i = 0; i < pwords; i++) {
      uint32_t w;
      memcpy(&w, pblob + i * 4, 4);
      fprintf(f, "%s0x%08x,", i % 6 ? " " : "\n\t", w);
   }
   fprintf(f, "\n};\n");
   fclose(f);
   fprintf(stderr, "wrote %s (%u words)\n", argv[1], words);
   (void)fbits;
   return 0;
}
