pkgname=javelin-mail-git
_project_name=Javelin-Mail
pkgver=0.1.0.r85.g96e643b
pkgrel=1
pkgdesc='A Qt Widgets JMAP email client'
arch=('x86_64')
url='https://javelin.app'
license=('GPL-3.0-only')
depends=(
  'gcc-libs'
  'glibc'
  'kcoreaddons'
  'kxmlgui'
  'qcoro'
  'qt6-base'
  'qt6-svg'
  'qt6-webengine'
)
makedepends=(
  'cmake'
  'extra-cmake-modules'
  'git'
  'glaze'
  'ninja'
  'qt6-tools'
)
provides=('javelin-mail')
conflicts=('javelin-mail')

_repo_root() {
  cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P
}

_canonical_version() {
  sed -nE 's/^project\(Javelin-Mail VERSION ([0-9]+(\.[0-9]+)*) LANGUAGES CXX\)$/\1/p' "$(_repo_root)/CMakeLists.txt"
}

pkgver() {
  cd "$(_repo_root)"

  local base_version
  base_version="$(_canonical_version)"
  if [[ -z "$base_version" ]]; then
    printf '0.0.0.r0.g%s\n' "$(git rev-parse --short HEAD)"
    return
  fi

  local commit_count commit_hash
  commit_count="$(git rev-list --count HEAD)"
  commit_hash="$(git rev-parse --short HEAD)"
  printf '%s.r%s.g%s\n' "$base_version" "$commit_count" "$commit_hash"
}

build() {
  cd "$(_repo_root)"
  cmake --preset release -B out/build/pkg \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=OFF \
    -DJAVELIN_ENABLE_LOCAL_DATA_INSTALL=OFF
  cmake --build out/build/pkg
}

package() {
  cd "$(_repo_root)"
  DESTDIR="$pkgdir" cmake --install out/build/pkg
}
