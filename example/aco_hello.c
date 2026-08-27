/*
 * Minimal standalone proof-of-concept: build a NIR compute shader by hand
 * (no SPIR-V, no Vulkan, no descriptor sets) and run it all the way through
 * ACO to real AMD ISA bytes, using only:
 *   - libnir.a       (src/compiler/nir)
 *   - libamd_common.a (src/amd/common)
 *   - libaco.a        (src/amd/compiler)
 *   - libmesa_util.a  (src/util)
 *
 * No RADV, no libdrm ioctls, no GPU required to run this -- ac_fill_compiler_info()
 * derives everything from the gfx_level/family enum, and ACO compiles shaders
 * entirely on the CPU.
 *
 * Shader semantics (workgroup_size = 64,1,1):
 *   shared uint lds[64];
 *   void main() { lds[local_invocation_index] = local_invocation_index * 2 + 42; }
 *
 * This deliberately avoids buffer/image descriptors and push constants: those
 * are wired into ac_shader_args by *driver-specific* NIR lowering passes
 * (RADV has its own; a from-scratch driver needs its own too). Shared-memory
 * and invocation IDs are plain hardware inputs ACO understands natively, so
 * this is the smallest shader that proves the whole NIR -> ACO -> ISA path
 * works with zero driver glue.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"

#include "aco_interface.h"
#include "aco_shader_info.h"
#include "ac_shader_args.h"
#include "ac_gpu_info.h"
#include "common/amd_family.h"
#include "common/nir/ac_nir.h"

struct capture {
   struct ac_shader_config config;
   uint32_t *code;
   uint32_t code_dw;
};

static void
capture_binary(void **priv_ptr, const struct ac_shader_config *config, const char *llvm_ir_str,
               unsigned llvm_ir_size, const char *disasm_str, unsigned disasm_size,
               struct amd_stats *stats, uint32_t exec_size, const uint32_t *code,
               uint32_t code_dw, const struct aco_symbol *symbols, unsigned num_symbols,
               const struct ac_shader_debug_info *debug_info, unsigned debug_info_count)
{
   struct capture *cap = *(struct capture **)priv_ptr;
   cap->config = *config;
   cap->code_dw = code_dw;
   cap->code = malloc(code_dw * 4);
   memcpy(cap->code, code, code_dw * 4);
}

int
main(int argc, char **argv)
{
   enum amd_gfx_level gfx_level = GFX10_3;
   enum radeon_family family = CHIP_NAVI21;

   if (argc > 1) {
      if (!strcmp(argv[1], "gfx9")) { gfx_level = GFX9; family = CHIP_VEGA10; }
      else if (!strcmp(argv[1], "gfx10_3")) { gfx_level = GFX10_3; family = CHIP_NAVI21; }
      else if (!strcmp(argv[1], "gfx11")) { gfx_level = GFX11; family = CHIP_NAVI31; }
      else { fprintf(stderr, "usage: %s [gfx9|gfx10_3|gfx11]\n", argv[0]); return 1; }
   }

   struct radeon_info rad_info;
   memset(&rad_info, 0, sizeof(rad_info));
   rad_info.gfx_level = gfx_level;
   rad_info.family = family;
   ac_fill_compiler_info(&rad_info, NULL, false);

   struct nir_shader_compiler_options nir_options;
   memset(&nir_options, 0, sizeof(nir_options));
   ac_nir_set_options(&rad_info.compiler_info, false, &nir_options);

   glsl_type_singleton_init_or_ref();

   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &nir_options, "aco_hello");

   b.shader->info.workgroup_size[0] = 64;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.shared_size = 64 * 4;

   nir_def *lid = nir_load_local_invocation_index(&b);
   nir_def *val = nir_iadd_imm(&b, nir_imul_imm(&b, lid, 2), 42);
   nir_def *addr = nir_imul_imm(&b, lid, 4);
   nir_store_shared(&b, val, addr, .align_mul = 4);

   /* Turn the hardware-agnostic sysval intrinsic (load_local_invocation_index)
    * into the hardware-specific one(s) ACO actually understands. */
   nir_lower_compute_system_values(b.shader, NULL);

   /* Declare which SGPR/VGPR user-data ACO gets to work with. This is the
    * generic (driver-agnostic) hardware ABI layer -- every AMD compute
    * shader needs its local-invocation-id delivered this way, regardless
    * of descriptor-set/push-constant design, which is what a real driver
    * still has to add on top for buffers/textures/etc. */
   struct ac_shader_args args;
   memset(&args, 0, sizeof(args));
   if (rad_info.compiler_info.local_invocation_ids_packed) {
      ac_add_arg(&args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args.local_invocation_ids_packed);
   } else {
      ac_add_arg(&args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args.local_invocation_id_x);
      ac_add_arg(&args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args.local_invocation_id_y);
      ac_add_arg(&args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args.local_invocation_id_z);
   }

   ac_nir_lower_intrinsics_to_args_options lower_opts = {
      .gfx_level = gfx_level,
      .hw_stage = AC_HW_COMPUTE_SHADER,
      .wave_size = 64,
      .workgroup_size = 64,
   };
   ac_nir_lower_intrinsics_to_args(b.shader, &args, &lower_opts);

   nir_validate_shader(b.shader, "aco_hello");

   fprintf(stderr, "==== NIR (after lowering to args) ====\n");
   nir_print_shader(b.shader, stderr);

   struct aco_compiler_options options;
   memset(&options, 0, sizeof(options));
   options.compiler_info = &rad_info.compiler_info;
   options.family = family;
   options.gfx_level = gfx_level;

   struct aco_shader_info info;
   memset(&info, 0, sizeof(info));
   info.hw_stage = AC_HW_COMPUTE_SHADER;
   info.wave_size = 64;
   info.workgroup_size = 64;

   struct capture cap;
   memset(&cap, 0, sizeof(cap));
   struct capture *cap_ptr = &cap;

   nir_shader *shaders[1] = { b.shader };
   aco_compile_shader(&options, &info, 1, shaders, &args, capture_binary, (void **)&cap_ptr);

   fprintf(stderr, "\n==== Result ====\n");
   fprintf(stderr, "gfx_level=%d family=%d\n", gfx_level, family);
   fprintf(stderr, "num_sgprs=%u num_vgprs=%u lds_size(chunks)=%u scratch_bytes_per_wave=%u\n",
           cap.config.num_sgprs, cap.config.num_vgprs, cap.config.lds_size,
           cap.config.scratch_bytes_per_wave);
   fprintf(stderr, "code_dw=%u\n", cap.code_dw);

   printf("/* %u dwords of AMD ISA, gfx_level=%d family=%d */\n", cap.code_dw, gfx_level, family);
   for (uint32_t i = 0; i < cap.code_dw; i++) {
      printf("0x%08x,%s", cap.code[i], (i % 4 == 3) ? "\n" : " ");
   }
   printf("\n");

   free(cap.code);
   ralloc_free(b.shader);
   glsl_type_singleton_decref();

   return cap.code_dw > 0 ? 0 : 1;
}
