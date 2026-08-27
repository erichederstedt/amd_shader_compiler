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

subdir('example')
