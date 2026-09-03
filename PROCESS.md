# How ACO was made to build and run standalone

This documents the actual steps taken to get Mesa's ACO (AMD Compiler)
building and producing real AMD ISA outside of a full Mesa/RADV tree,
including the dead ends, so the reasoning is reproducible and not just the
end result. See `README.md` for the resulting architecture and usage;
this file is the "how we got there."

## 1. Why this isn't a simple file-copy job

ACO is not a self-contained library inside Mesa. It's a NIR-to-ISA backend:
it expects a NIR shader, a populated `ac_shader_args` describing which
SGPRs/VGPRs the driver has decided to feed it, and some AMD chip tables,
and produces machine code. There's no upstream "standalone ACO" build
target, so the first job was figuring out ACO's *actual* dependency
surface rather than guessing.

Grepping every `#include` across `src/amd/compiler/` and sorting by origin
showed the real closure was small and well-defined:

- `src/compiler/nir` -- the IR itself
- `src/amd/common` -- chip tables (`amd_family.h`, `amdgfxregs.h`),
  `ac_shader_args.h`, `ac_gpu_info.h`, and the shared `ac_nir_lower_*`
  passes
- `src/util` -- Mesa's base utility library (allocators, bitsets, etc.)
- LLVM headers, but only inside `aco_print_asm.cpp` for optional
  disassembly printing -- not needed for codegen itself
- `vulkan/vulkan.h` -- for a couple of enum types, not for linking against
  a real Vulkan loader

This ruled out the riskiest option early: hand-copying/rewriting ACO's
frontend to eat a custom IR instead of NIR. The dependency surface was
narrow enough that vendoring real, unmodified NIR + a slice of Mesa was
clearly less work and less risky than reinventing instruction selection's
NIR-shaped assumptions.

A decision was made up front, with the user, on three axes before writing
any code:

1. **Frontend**: keep NIR + SPIR-V→NIR (`spirv_to_nir`) as the input
   boundary, rather than stripping NIR out or hand-building a custom IR.
2. **Target GPUs**: all generations ACO supports (GCN through RDNA3+).
3. **Build system**: keep Meson, rather than porting to CMake or dropping
   a bare source tree with no build system at all.

## 2. Environment: this container had almost nothing

The devcontainer (`rocm/amdgpu-driver` base image, RHEL 9.8 UBI) shipped
with `bash`, `curl`, `tar` -- no `git`, no compiler, no Meson/Ninja, no
Python. `dnf`/`yum` weren't present either, only `rpm` and `microdnf`
(and no active Red Hat subscription/entitlement certs). What actually
worked:

- The public, unauthenticated UBI CDN (`cdn-ubi.redhat.com`) serves
  baseos/appstream repos without an entitlement cert, so `microdnf
  install` worked for `git`, `gcc`, `gcc-c++`, `cmake`, `python3`,
  `python3-pip`, `ninja-build`, `pkgconf-pkg-config`, etc.
- Meson from `dnf`'s Python 3.9 build was too old for Mesa's
  `meson.options` requirements; installed a current Meson via `pip3
  install meson` instead.
- Mesa's generated-header build (opcode tables, NIR intrinsics, etc.) uses
  Mako templates and requires **Python >= 3.10** — UBI9's default `python3`
  is 3.9. Installed `python3.12` + `python3.12-pip` from the same repo
  alongside it, and `pip install mako pyyaml` under 3.12.
- `glslangValidator` wasn't packaged in the enabled repos at all (no
  EPEL). Downloaded a prebuilt release binary directly from the
  `KhronosGroup/glslang` GitHub releases instead of building it from
  source.

## 3. Getting Mesa source

No local Mesa checkout existed. Rather than pull `main` (which churns),
queried the GitLab API for tags and pinned to the latest stable release at
the time, **`mesa-26.2.1`**, via a shallow clone (`git clone --depth 1
--branch mesa-26.2.1`) into `vendor/mesa-src/`.

## 4. Finding the smallest Meson configuration that still builds ACO

Mesa's own `src/amd/meson.build` only descends into `src/amd/compiler`
(building `libaco.a`) when `with_amd_vk` (i.e. `-Dvulkan-drivers=amd`) or
`with_gallium_radeonsi` is true. Enabling `gallium-drivers=radeonsi` would
have dragged in the entire Gallium state-tracker framework just to reach
the compiler subdirectory, so `-Dvulkan-drivers=amd` was the cheaper path
-- even though it also configures (but doesn't have to *build*) the rest
of RADV.

Iterating on `meson setup` against real error messages (not guesswork)
produced this option set:

```
-Dplatforms=              # no X11/Wayland WSI code at all
-Dvulkan-drivers=amd      # the only thing that reaches src/amd/compiler
-Dgallium-drivers=        # no Gallium framework
-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false
-Dgles1=disabled -Dgles2=disabled -Dshared-glapi=disabled
-Dllvm=disabled           # ACO doesn't need LLVM to generate code
-Dvulkan-layers=
-Dvalgrind=disabled -Dlibunwind=disabled -Dselinux=false -Dzstd=disabled
-Dbuild-tests=false -Dtools=
-Dallow-fallback-for=libdrm
```

Each of those came from a real configure failure and a specific fix:

- **`selinux` option**: Meson rejected `disabled`; it's a boolean option,
  not a feature. Changed to `-Dselinux=false`.
- **`glslangValidator` not found**: RADV unconditionally requires it at
  configure time to build its built-in ray-tracing BVH-construction
  shaders (`with_bvh = with_amd_vk or ...`), regardless of whether ray
  tracing is ever used or whether RADV itself gets built. Fixed by
  installing the prebuilt binary (§2), not by trying to disable this --
  there's no option to.
- **Python >= 3.10 required**: fixed via the `python3.12` install (§2);
  Meson auto-detects the newest suitable interpreter it can find on
  `PATH`.
- **`libdrm` not found, version >= 2.4.133 needed**: UBI's repos had no
  `libdrm-devel` package at all. Mesa vendors a `subprojects/libdrm.wrap`
  fallback, but it's only used when explicitly allowed
  (`allow-fallback-for` defaults to just `perfetto`). Adding `libdrm` to
  `-Dallow-fallback-for` let Meson build libdrm itself from its wrap
  subproject — Meson also auto-fetched `zlib` and `expat` the same way
  without any extra flag, since those already had unconditional
  fallbacks.

With that configuration, `ninja -C build src/amd/compiler/libaco.a`
succeeded standalone: no LLVM, no X11/Wayland, no Vulkan loader linkage.
Verified it wasn't a fluke by checking `aco_compile_shader` was actually a
defined (not just declared) symbol in the resulting archive.

Building `libnir.a`, `libvtn.a` (SPIR-V→NIR), and `libamd_common.a`
(explicitly requested as extra ninja targets) succeeded the same way, and
running the existing `spirv2nir` tool Mesa ships (`src/compiler/spirv/
spirv2nir.c`) against a real compiled GLSL compute shader confirmed the
SPIR-V→NIR frontend worked fully standalone too, dumping correct NIR text
for a shader with an SSBO binding.

## 5. Finding out what ACO actually needs from a driver

Before writing any example code, the goal was to find the *real* minimal
input ACO expects, rather than guess at API usage from headers alone.
Mesa ships its own ACO unit test harness
(`src/amd/compiler/tests/helpers.cpp`, gated behind a `build-aco-tests`
Meson option), which turned out to be a mix of two very different things:

- Low-level IR/pass tests (register allocation, scheduling, the
  assembler, ...) build `aco::Program` directly through the internal
  `aco::Builder` API, bypassing NIR entirely.
- Instruction-selection tests (`test_isel.cpp`) go through actual SPIR-V,
  but only because they call real `vkCreateComputePipelines`/
  `vkCreateGraphicsPipelines` against **RADV itself** (`libvulkan_radeon.so`,
  loaded directly via its ICD entry point, running against a fake
  `amdgpu` device via `drm-shim` so no real GPU is needed) -- i.e. this
  test path exercises RADV's own driver-specific NIR lowering, not a
  "standalone ACO" input path.

That distinction mattered: it meant the test suite couldn't just be
reused as-is for a driver-agnostic example, and it surfaced the real
question -- how much of what a shader needs is ACO's job versus the
driver's.

Answered that by grepping, not assuming:

- `nir_intrinsic_load_push_constant` and every `vulkan_resource_index`/
  `load_vulkan_descriptor`-style intrinsic: **zero matches** anywhere in
  `src/amd/compiler/instruction_selection/`. ACO has no code path for
  descriptor sets or push constants at all -- RADV resolves those into
  concrete SGPR-backed reads in its own driver-private passes
  (`src/amd/vulkan/radv_nir_lower_descriptors.c`,
  `radv_shader_args.c`) before ACO ever sees the shader.
- `nir_intrinsic_load_local_invocation_id`/`load_local_invocation_index`:
  also **zero matches** in ACO's isel. Confirmed those are expected to
  already be lowered away by the time ACO runs, this time into something
  ACO does handle.
- Traced that lowering to `src/amd/common/nir/
  ac_nir_lower_intrinsics_to_args.c` -- a *driver-agnostic* pass (shared
  between RADV and radeonsi, already part of `libamd_common.a`) that
  turns hardware system-value intrinsics into reads of whatever SGPR/VGPR
  slots the driver registered via `ac_add_arg()` on an `ac_shader_args`
  struct. This is the real, reusable seam between "generic NIR shader"
  and "ACO-compilable shader" -- the *mechanism* is shared infrastructure;
  only the *policy* of which slot holds which descriptor/push-constant is
  driver-specific (and is exactly the part left for the user's own driver
  to design, documented as such in `README.md`).

## 6. Building and debugging the end-to-end example

Wrote `example/aco_hello.c`: a hand-built NIR compute shader (workgroup
size 64, no SPIR-V, no descriptor sets) that computes a value from the
invocation index and writes it to LDS (`shared` memory) -- deliberately
the smallest shader that needs zero driver-specific lowering, to isolate
"does the standalone build actually work" from "has a driver ABI been
designed."

First compiled it directly with `gcc`/`g++` against the ninja-built `.a`
files (not yet through the top-level Meson project, which didn't exist
yet) to iterate quickly. That surfaced several real problems in order:

1. **Missing feature-detection macros** (`UTIL_ARCH_LITTLE_ENDIAN`
   unset, `struct timespec` redefinition, unknown `once_flag`/`mtx_t`/
   `thrd_t`...): Mesa's headers assume a long list of `-DHAVE_*` macros
   that Meson computes from real compiler/platform checks and injects via
   `add_project_arguments()`. Fixed by extracting the exact compiler
   invocation Ninja used for a real Mesa translation unit (parsing
   `build.ninja` for one target's `ARGS` line) and reusing that same flag
   set, rather than trying to reconstruct the macro list by hand.
2. **Linker errors for `Addr*`, `blake3_hasher_*`, `c23_timespec_get`**
   despite the providing static libraries being on the link line: plain
   static-archive link order matters -- `ld` only pulls in archive
   members to satisfy *already-pending* undefined symbols, so an archive
   listed before the object that needs it gets skipped. Fixed by wrapping
   the whole set of Mesa static libraries in `-Wl,--start-group ...
   -Wl,--end-group` so circular/order-independent resolution works.
3. **Two libraries (`libaddrlib.a`, `libdrm_amdgpu.so`) referenced by the
   link line but not yet built**: they weren't pulled in by the
   `libaco.a`/`libamd_common.a` ninja targets built earlier, since Ninja
   only builds what's requested plus its *declared* dependencies for that
   specific target, not everything a full link might eventually need.
   Built them explicitly (`ninja -C build src/amd/addrlib/libaddrlib.a`,
   and the actual versioned `.so` targets, not the unversioned symlink
   names, which aren't ninja targets themselves).
4. **`ACO ERROR: Unimplemented intrinsic instr: load_local_invocation_index`**:
   the first real confirmation, at runtime, of the §5 finding -- ACO
   genuinely does not handle this intrinsic. Fixed in two steps:
   - Ran `nir_lower_compute_system_values()` first, which (for a
     workgroup that fits in one wave) NIR is able to algebraically
     rewrite into `load_local_invocation_id`-shaped math -- this alone
     wasn't enough.
   - Realized `load_local_invocation_id` isn't handled by ACO either
     (confirmed by the same §5 grep). Added the actual missing step:
     declare an `ac_shader_args` with `ac_add_arg()` for
     `local_invocation_id_x/y/z` (or the packed single-VGPR form on
     hardware that supports it, branching on
     `compiler_info.local_invocation_ids_packed` the same way
     `radv_shader_args.c` does), then run
     `ac_nir_lower_intrinsics_to_args()` before instruction selection.
     After that, `aco_compile_shader()` succeeded and returned real ISA.
   - Interesting side effect visible in the resulting NIR: since the
     workgroup size (64) equals the wave size, NIR/ACO's own
     optimizations replaced the VGPR-arg read with `v_mbcnt_lo/hi`
     (subgroup lane index) instead, which is legitimate and cheaper --
     a small sign the pipeline was doing real optimization work, not
     just passing data through.

Verified the resulting machine code by hand at a basic level (ends in
`0xbf810000` = `s_endpgm`, padded with `0xbf9f0000` = `s_nop 0`, with
plausible VOP3/DS-format encodings before that) and by running the same
shader against `gfx9`, `gfx10_3`, and `gfx11` targets and confirming each
produced a different, plausible instruction count/register footprint
appropriate to that generation, with no ACO errors on any of them.

## 7. Turning the manual build into a reusable Meson project

The manual `gcc`/`g++` invocation with a hand-extracted flag string was
good enough to prove the concept but not something to hand to a user as
"here's how you build it" -- and the user had specifically asked to keep
Meson as the build system. Wrote a top-level `meson.build` that pulls
Mesa in via `subproject('mesa', default_options: {...})` with the same
option set from §4, then re-exposes what's needed as a single dependency
object.

This needed more than just grabbing `idep_aco`: Mesa's own
`idep_aco` only carries `src/amd/compiler`'s own include directory --
`inc_amd`/`inc_amd_common` (needed for `#include "common/amd_family.h"`-
style paths) and the `-DHAVE_*` feature macros from §6.1 are set via
`add_project_arguments()` *inside* the mesa subproject's own scope, which
Meson does not propagate to a separate top-level project automatically.
Fixed by pulling those specific variables back out with
`mesa.get_variable(...)` (`idep_aco`, `idep_vtn`, `idep_nir`,
`idep_mesautil`, `idep_amd_generated_headers`, `libamd_common`,
`libamdgpu_addrlib`, `inc_amd`, `inc_amd_common`, `pre_args`,
`c_cpp_args`) and assembling them into one `aco_standalone_dep` in our
own `meson.build`, which `example/meson.build` then just depends on.

Symlinked `subprojects/mesa -> ../vendor/mesa-src` so Meson's
`subproject()` lookup (which expects `subprojects/<name>/meson.build`)
resolves without duplicating the checkout.

Verified this was actually reproducible, not just "worked once with
leftover state": deleted `build/` entirely and re-ran `meson setup build
&& ninja -C build example/aco_hello` from a clean state, confirming a
~45s from-scratch build and a working binary.

## 8. Git repository

Set up as a separate, later step (see conversation for the exact
sequence): `git init`, a `.gitignore` excluding `build/`,
`vendor/mesa-src/` (600+ MB, not meant to be committed), and Meson's
auto-generated wrap-redirect files in `subprojects/`; a
`scripts/fetch-mesa.sh` that reproduces `vendor/mesa-src` (pinned to the
same `mesa-26.2.1` tag) and the `subprojects/mesa` symlink from a clean
checkout; and an initial commit. Two container-specific issues came up
and were fixed:

- `git` refused to operate at all ("detected dubious ownership") because
  the repo directory's owner didn't match the running user; worked around
  it with a per-invocation `-c safe.directory='*'` rather than writing
  that exception into git config.
- No git identity was configured in the container at all; committed with
  a per-invocation `-c user.name=... -c user.email=...` (using the email
  already available in this session's context) rather than writing
  identity into git config, global or local -- flagged to the user that
  their *next* commit will need their own `git config user.name/email`
  first.
- Everything created while running as root (this container's configured
  `remoteUser`) ended up owned by `root:root` instead of the original
  workspace owner (`uid/gid 1000`, no matching `/etc/passwd` entry in
  this image but clearly the intended owner); fixed with a recursive
  `chown -R 1000:1000` once this was noticed.

## What this process did *not* attempt

Deliberately out of scope, and not fudged or half-implemented to look
otherwise:

- A descriptor-set/push-constant/buffer-binding ABI. This is real driver
  design work specific to whatever binding model the user's own driver
  ends up with, not something to extract from RADV (see `README.md`).
- PM4 command-stream construction, GPU memory allocation, or submission
  via `libdrm_amdgpu`/`amdgpu`/`kfd` -- entirely outside ACO/Mesa's
  compiler layer.
- Pruning `vendor/mesa-src` down to only the files ACO's dependency
  closure needs. It stays a full pinned checkout built via Mesa's real
  Meson graph (with unrelated components merely disabled at configure
  time) specifically because that's lower-risk than hand-maintaining a
  file list that could silently drift from what upstream actually needs.
