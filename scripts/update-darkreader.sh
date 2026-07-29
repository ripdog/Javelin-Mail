#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd -- "${script_dir}/.." && pwd)"
source_dir="${repository_root}/references/darkreader"
vendor_dir="${repository_root}/res/vendor/darkreader"
patch_file="${vendor_dir}/mail-profile.patch"
patch_applied=false

restore_source() {
    if [[ "${patch_applied}" == true ]]; then
        git -C "${source_dir}" apply --reverse "${patch_file}"
    fi
}
trap restore_source EXIT

if [[ ! -d "${source_dir}/.git" ]]; then
    echo "Dark Reader source checkout is missing: ${source_dir}" >&2
    exit 1
fi

if ! git -C "${source_dir}" diff --quiet ||
    ! git -C "${source_dir}" diff --cached --quiet; then
    echo "Dark Reader source checkout must be clean before updating the bundle." >&2
    exit 1
fi

git -C "${source_dir}" apply --check "${patch_file}"
git -C "${source_dir}" apply "${patch_file}"
patch_applied=true

npm --prefix "${source_dir}" ci --ignore-scripts
npm --prefix "${source_dir}" run api

sed 's/\r$//' "${source_dir}/darkreader.js" >"${vendor_dir}/darkreader.js.tmp"
install -m 0644 "${vendor_dir}/darkreader.js.tmp" "${vendor_dir}/darkreader.js"
rm "${vendor_dir}/darkreader.js.tmp"
install -m 0644 "${source_dir}/LICENSE" "${vendor_dir}/LICENSE"

version="$(node -p "require('${source_dir}/package.json').version")"
commit="$(git -C "${source_dir}" rev-parse HEAD)"
checksum="$(sha256sum "${vendor_dir}/darkreader.js" | cut -d ' ' -f 1)"

printf 'version=%s\ncommit=%s\nsha256=%s\n' "${version}" "${commit}" "${checksum}" \
    >"${vendor_dir}/VERSION"

echo "Updated Dark Reader ${version} (${commit})."
