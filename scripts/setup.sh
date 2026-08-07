#!/bin/sh
# Fetches the vendored dependencies and applies the Wren VM patch.
#
# Wren is used unmodified except for the JIT hooks in patches/. Rather than
# fork Wren, the patch is applied to the pinned upstream checkout in
# vendor/wren. Re-running this script is safe: an already-applied patch is
# detected and skipped.

set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
patch=$root/patches/0001-wren-jit-hooks.patch

git -C "$root" submodule update --init --recursive

if git -C "$root/vendor/wren" apply --reverse --check "$patch" 2>/dev/null; then
  echo "Wren JIT hooks already applied."
else
  git -C "$root/vendor/wren" apply "$patch"
  echo "Applied Wren JIT hooks."
fi
