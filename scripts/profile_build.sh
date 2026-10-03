#!/bin/sh
# Architecture-aware profile build, using Stockfish's existing PGO and LTO.
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_arch=${1:-native}
task_jobs=${JOBS:-4}
task_comp=${COMP:-gcc}
if [ "$(uname -s)" = Darwin ] && [ -z "${COMP:-}" ]; then task_comp=clang; fi
case "$task_arch" in *[!a-zA-Z0-9_-]*) echo "Invalid architecture" >&2; exit 2;; esac
case "$task_jobs" in ''|*[!0-9]*) echo "JOBS must be a positive integer" >&2; exit 2;; esac
if [ "$task_jobs" -le 0 ]; then echo "JOBS must be a positive integer" >&2; exit 2; fi
cd "$task_root/src"
case "$task_comp" in
  gcc|mingw) task_profile=gcc ;;
  clang) task_profile=clang ;;
  icx) task_profile=icx ;;
  *) echo "COMP must be gcc, mingw, clang or icx" >&2; exit 2 ;;
esac
# Serialize cleanup and rebuild every unit at both profile-flag transitions.
# Make does not track changes in compiler flags as object dependencies.
make net ARCH="$task_arch" COMP="$task_comp"
make objclean ARCH="$task_arch" COMP="$task_comp"
make profileclean ARCH="$task_arch" COMP="$task_comp"
make -B -j"$task_jobs" "$task_profile-profile-make" ARCH="$task_arch" COMP="$task_comp"
task_exe=./stockfish
if [ ! -f "$task_exe" ]; then task_exe=./stockfish.exe; fi
"$task_exe" bench > PGOBENCH.out 2>&1
if [ "$task_profile" = gcc ]; then
  if [ -z "$(find profdir -name '*search.gcda' -size +0c -print -quit)" ]; then
    echo "No search profile was produced; refusing an unprofiled optimization build" >&2
    exit 1
  fi
fi
make objclean ARCH="$task_arch" COMP="$task_comp"
make -B -j"$task_jobs" "$task_profile-profile-use" ARCH="$task_arch" COMP="$task_comp"
make profileclean ARCH="$task_arch" COMP="$task_comp"
