#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${NDSRECOMP_BUILD_DIR:-${project_dir}/build-linux}"

if [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
    cmake -S "${project_dir}" -B "${build_dir}" -G "${NDSRECOMP_CMAKE_GENERATOR:-Ninja}"
else
    cmake -S "${project_dir}" -B "${build_dir}"
fi

if [[ -n "${NDSRECOMP_BUILD_JOBS:-}" ]]; then
    cmake --build "${build_dir}" --parallel "${NDSRECOMP_BUILD_JOBS}"
else
    cmake --build "${build_dir}" --parallel
fi
