#!/usr/bin/env bash
# Fetches the pinned upstream Mesa checkout that ACO is built from.
#
# vendor/mesa-src is gitignored (600+ MB) rather than committed -- this
# script reproduces it. subprojects/mesa is a symlink to vendor/mesa-src,
# used by the top-level meson.build so Meson can treat Mesa as a subproject.
set -euo pipefail

MESA_TAG="mesa-26.2.1"
MESA_URL="https://gitlab.freedesktop.org/mesa/mesa.git"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="$repo_root/vendor/mesa-src"

if [ -e "$dest" ]; then
   echo "error: $dest already exists, remove it first if you want to re-fetch" >&2
   exit 1
fi

mkdir -p "$repo_root/vendor"
git clone --depth 1 --branch "$MESA_TAG" "$MESA_URL" "$dest"

mkdir -p "$repo_root/subprojects"
ln -sfn ../vendor/mesa-src "$repo_root/subprojects/mesa"

echo "Fetched $MESA_TAG into $dest"
echo "subprojects/mesa -> vendor/mesa-src symlink is in place"
