project('amd-shader-compiler', 'c', 'cpp',
  version : '0.1',
  default_options : ['c_std=c11', 'cpp_std=c++17', 'buildtype=release'],
)

# ACO ("aco_standalone") is not extracted/copied out of Mesa -- it is built
# directly from a pinned upstream Mesa checkout (subprojects/mesa, see
# vendor/mesa-src) via Mesa's own Meson build, with every unrelated frontend
# (GL, EGL/GLX/GBM, all gallium drivers, the Vulkan WSI/loader integration,
# LLVM) disabled. This is the same approach RADV itself uses to build ACO;
# we are just not building the rest of RADV.
mesa_opts = {
  'platforms': '',
  'vulkan-drivers': 'amd',
  'gallium-drivers': '',
  'glx': 'disabled',
  'egl': 'disabled',
  'gbm': 'disabled',
  'opengl': false,
  'gles1': 'disabled',
  'gles2': 'disabled',
  'shared-glapi': 'disabled',
  'llvm': 'disabled',
  'vulkan-layers': '',
  'valgrind': 'disabled',
  'libunwind': 'disabled',
  'selinux': false,
  'zstd': 'disabled',
  'build-tests': false,
  'tools': '',
  'allow-fallback-for': 'libdrm',
}

mesa = subproject('mesa', default_options : mesa_opts)

# Pieces of the standalone shader-compiler stack, straight out of the Mesa
# subproject:
#   idep_aco                  -> src/amd/compiler   (ACO itself)
#   libamd_common / libaddrlib -> src/amd/common, src/amd/addrlib
#                                 (chip tables, ac_shader_args, the shared
#                                 ac_nir_lower_* passes, surface/addr math)
#   idep_nir                  -> src/compiler/nir    (the IR ACO consumes)
#   idep_vtn                  -> src/compiler/spirv  (SPIR-V -> NIR frontend)
#   idep_mesautil / idep_amd_generated_headers -> src/util and generated
#                                 chip/opcode headers shared by all of the above
aco_standalone_dep = declare_dependency(
  dependencies : [
    mesa.get_variable('idep_aco'),
    mesa.get_variable('idep_vtn'),
    mesa.get_variable('idep_nir'),
    mesa.get_variable('idep_mesautil'),
    mesa.get_variable('idep_amd_generated_headers'),
  ],
  link_with : [
    mesa.get_variable('libamd_common'),
    mesa.get_variable('libamdgpu_addrlib'),
  ],
  include_directories : [
    mesa.get_variable('inc_amd'),
    mesa.get_variable('inc_amd_common'),
  ],
  # Mesa's own feature-detection defines (HAVE_ENDIAN_H, HAVE_PTHREAD, ...)
  # are added via add_project_arguments() inside the mesa subproject, so
  # they don't automatically apply to us as a separate Meson project --
  # pull the same computed flags in explicitly.
  compile_args : [
    mesa.get_variable('pre_args'),
    mesa.get_variable('c_cpp_args'),
  ],
)

# plans/vertex_pixel_draw_plan.md Batch 0: everything vk_reference_trace.c
# (third_party/shader_bridge/) needs to reach RADV's internal
# struct radv_cmd_buffer/struct ac_cmdbuf directly (see that file's own top
# comment for why -- recording a real draw through the real driver without
# ever submitting it, then decoding the recorded PM4 bytes on the CPU).
# Mirrors aco_standalone_dep's own approach: pull already-configured pieces
# straight out of the vendored mesa subproject, add nothing new to it.
# radv_cmd_buffer.h itself isn't exposed via any inc_* variable (it's only
# ever included via a bare include_directories('.') inline in
# src/amd/vulkan/meson.build), so it's reached with a plain relative path
# into the pristine vendored checkout instead.
radv_internal_dep = declare_dependency(
  dependencies : [
    mesa.get_variable('idep_vulkan_util'),
    mesa.get_variable('idep_vulkan_runtime'),
    mesa.get_variable('idep_vulkan_wsi'),
    mesa.get_variable('idep_xmlconfig'),
    mesa.get_variable('idep_radv_tracepoints'),
  ],
  include_directories : [
    mesa.get_variable('inc_include'),
    mesa.get_variable('inc_src'),
    mesa.get_variable('inc_amd'),
    mesa.get_variable('inc_amd_common'),
    mesa.get_variable('inc_amd_common_llvm'),
    mesa.get_variable('inc_util'),
    mesa.get_variable('inc_virtio_gpu'),
    include_directories('vendor/mesa-src/src/amd/vulkan'),
  ],
  # The generated radv_entrypoints.h -- listed as a source (not linked) so
  # Meson builds it and puts it on the include path before compiling
  # anything that depends on this; Meson only compiles recognized .c/.cpp
  # suffixes from a sources list, so the paired generated .c is inert here.
  sources : [
    mesa.get_variable('radv_entrypoints')[0],
  ],
)

subdir('example')
