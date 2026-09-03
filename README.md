# Standalone ACO

ACO (the AMD Compiler used by RADV) built and running outside of a full Mesa
checkout, for use as the shader backend of a from-scratch AMD userland
driver/graphics API.

## What this actually is

ACO is not a self-contained library inside Mesa -- it's a NIR-to-ISA backend
that assumes a NIR shader, a populated `ac_shader_args`, and some AMD chip
tables are handed to it. There is no upstream "extract ACO" mode, so this
repo takes the approach that carries the least risk of silently diverging
from upstream: **build the real Mesa source, unmodified, with everything
except the compiler stack disabled** (no GL/EGL/GLX/GBM, no gallium
drivers, no Vulkan WSI/loader integration, no LLVM). What's left over is
exactly ACO's real dependency closure:

- `src/amd/compiler` -- ACO itself (`idep_aco`)
- `src/compiler/nir` -- the IR ACO consumes (`idep_nir`)
- `src/compiler/spirv` -- SPIR-V -> NIR frontend (`idep_vtn`)
- `src/amd/common` -- chip tables, `ac_shader_args`, and the
  `ac_nir_lower_*` passes shared between RADV and radeonsi
  (`libamd_common`)
- `src/amd/addrlib`, `src/util` -- supporting libraries these need

Verified working: `libaco.a` + `libnir.a` + `libvtn.a` + `libamd_common.a`
compile from a clean checkout with no LLVM, no libGL/EGL, no Vulkan loader,
and the resulting code runs a NIR shader through instruction selection,
register allocation, scheduling, and the assembler to produce real GCN
(GFX9), RDNA2 (GFX10.3) and RDNA3 (GFX11) machine code. See `example/`.

## Layout

- `vendor/mesa-src/` -- pinned upstream Mesa checkout (`mesa-26.2.1`,
  shallow clone). Gitignored (600+ MB); fetched by `scripts/fetch-mesa.sh`
  rather than committed. Do not hand-edit; re-pin by bumping `MESA_TAG` in
  that script and re-running it against a clean `vendor/`.
- `subprojects/mesa` -- symlink to `vendor/mesa-src`, so Meson can treat it
  as a subproject. Created by `scripts/fetch-mesa.sh`.
- `meson.build` -- configures the Mesa subproject with everything but the
  compiler stack disabled, and re-exposes `idep_aco` + friends as one
  `aco_standalone_dep` for our own targets to depend on.
- `example/aco_hello.c` -- minimal end-to-end proof: hand-builds a NIR
  compute shader (no SPIR-V, no Vulkan) and runs it through
  `aco_compile_shader()` to real ISA bytes.

## Build

Toolchain needed: a C/C++17 compiler, Meson >= 1.3, Ninja, Python >= 3.10
with `mako` (Mesa's shader/opcode tables are code-generated), and
`glslangValidator` on `PATH` (only needed because RADV's build
unconditionally wants it for its built-in ray-tracing BVH shaders, even
though we don't build RADV's Vulkan entry points).

```sh
./scripts/fetch-mesa.sh            # clones vendor/mesa-src (pinned tag,
                                    # gitignored -- not committed) and
                                    # symlinks subprojects/mesa to it
meson setup build
ninja -C build example/aco_hello   # NOT a bare `ninja -C build` --
                                    # the mesa subproject's default target
                                    # set still includes all of RADV
                                    # (libvulkan_radeon.so) since nothing
                                    # marks it build_by_default:false: it
                                    # will build fine, just needlessly.
./build/example/aco_hello gfx10_3  # or gfx9 / gfx11
```

Output is the compiled shader's `ac_shader_config` (SGPR/VGPR usage) and
its machine code as a dword hex dump on stdout, with the NIR IR at each
stage on stderr.

## What's deliberately NOT here: the driver-specific ABI layer

`example/aco_hello.c` only uses hardware-native inputs (invocation IDs) and
LDS (`shared` memory) -- things ACO understands with zero extra help. Real
shaders also need:

- **Descriptor sets / buffer & image bindings.** In RADV these are lowered
  from Vulkan-style NIR intrinsics (`vulkan_resource_index`,
  `load_vulkan_descriptor`, ...) into concrete SGPR-backed buffer
  descriptor reads by driver-private passes in `src/amd/vulkan/` (see
  `radv_nir_lower_descriptors.c`, `radv_shader_args.c`). ACO itself has no
  code path for these intrinsics at all -- confirmed by grepping
  `src/amd/compiler/instruction_selection/`, they simply don't appear
  there. This lowering is inherently tied to whatever binding model your
  own driver defines (descriptor sets, bindless, buffer-device-address,
  ...), so it isn't something to vendor from RADV -- it's the actual
  design work of your driver's shader frontend.
- **Push constants.** Same story: `nir_intrinsic_load_push_constant` has no
  ACO isel case either; RADV resolves it before ACO ever sees the shader.
- **Buffer descriptors themselves** are just 4-dword V#/T# hardware
  resource descriptors (base address, stride, num_records, format) -- see
  `src/amd/common/ac_shader_args.h`'s `push_constants`/general arg
  mechanism and `ac_nir_lower_intrinsics_to_args.c` for the pattern used
  to turn "driver decided this SGPR/VGPR holds X" into NIR values ACO
  understands. This is the piece worth reusing directly; the *policy* of
  what goes in which slot is driver-specific and is what `radv_shader_args.c`
  encodes for RADV's own model.

`example/aco_hello.c`'s arg setup (`ac_add_arg` for
`local_invocation_id`/`local_invocation_ids_packed`, then
`ac_nir_lower_intrinsics_to_args()`) is the minimal, real pattern to follow
when you add your own inputs (a raw buffer pointer via a push-constant-like
SGPR pair is the simplest starting point for a from-scratch driver -- much
simpler than replicating Vulkan descriptor sets).

## Everything past "get ISA bytes" is your driver, not ACO

ACO's job ends at a dword array plus an `ac_shader_config` (SGPR/VGPR
counts, LDS size, scratch size). Getting that onto the GPU is entirely
outside ACO/Mesa's shader-compiler layer and is real driver work:
building the PM4 command stream for the relevant queue (`src/amd/common`'s
`ac_pm4.c`/`ac_cmdbuf*.c` are reusable helpers for packet building, but the
register programming sequence for a given pipeline state is driver logic),
allocating/mapping GPU memory and submitting via `libdrm_amdgpu`
(`amdgpu_bo_*`, `amdgpu_cs_*`) or directly via the `amdgpu`/`kfd` kernel
ioctls, and managing the user-data SGPR layout your own descriptor model
needs. None of that is addressed by this repo.
