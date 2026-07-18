#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${NDSRECOMP_BUILD_DIR:-${project_dir}/build-linux}"
executable="${build_dir}/ndsrecomp"

if [[ ! -x "${executable}" && -x "${executable}.exe" ]]; then
    executable="${executable}.exe"
fi
if [[ ! -x "${executable}" ]]; then
    printf 'missing %s; run ./compile.sh first\n' "${build_dir}/ndsrecomp" >&2
    exit 1
fi

exec "${executable}" "$@"
