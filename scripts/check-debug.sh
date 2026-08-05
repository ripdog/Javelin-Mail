#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  scripts/check-debug.sh --target TARGET [--target TARGET ...] [--tests REGEX]
  scripts/check-debug.sh --full

Serializes debug-preset configuration, builds, and tests in the shared workspace.
Use --full only for final configure/build/test/format verification.
EOF
}

declare -a targets=()
test_regex=
full=false

while (($# > 0)); do
    case "$1" in
    --target)
        if (($# < 2)); then
            echo "--target requires a CMake target" >&2
            usage >&2
            exit 2
        fi
        targets+=("$2")
        shift 2
        ;;
    --tests)
        if (($# < 2)); then
            echo "--tests requires a CTest regular expression" >&2
            usage >&2
            exit 2
        fi
        test_regex=$2
        shift 2
        ;;
    --full)
        full=true
        shift
        ;;
    --help|-h)
        usage
        exit 0
        ;;
    *)
        echo "Unknown argument: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

if [[ "$full" == true ]] && { ((${#targets[@]} > 0)) || [[ -n "$test_regex" ]]; }; then
    echo "--full cannot be combined with --target or --tests" >&2
    exit 2
fi

if [[ "$full" != true ]] && ((${#targets[@]} == 0)); then
    echo "Specify at least one --target, or use --full for final verification" >&2
    usage >&2
    exit 2
fi

for required_command in ccache cmake flock; do
    if ! command -v "$required_command" >/dev/null 2>&1; then
        echo "Required command is unavailable: $required_command" >&2
        exit 1
    fi
done

repository_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
runtime_directory=/tmp/javelin-mail-xdg-runtime
lock_file=/tmp/javelin-mail-debug-build-${USER:?USER is required}.lock
ccache_directory=/tmp/javelin-mail-ccache-${USER:?USER is required}

install -d -m 700 "$runtime_directory"
install -d -m 700 "$ccache_directory"
export XDG_RUNTIME_DIR=$runtime_directory
export CCACHE_DIR=$ccache_directory

exec 9>"$lock_file"
echo "Waiting for the shared Javelin Mail debug build lock..."
flock 9

cd "$repository_root"

if [[ "$full" == true ]]; then
    cmake --workflow --preset debug-check
    exit 0
fi

cmake --preset debug
cmake --build --preset debug --target "${targets[@]}"

if [[ -n "$test_regex" ]]; then
    ctest --test-dir out/build/debug --output-on-failure --tests-regex "$test_regex"
fi
