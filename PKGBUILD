# Maintainer: GB4ME Team
pkgname=gb4me
pkgver=1.0.0
pkgrel=1
pkgdesc="Visually accurate Game Boy / Game Boy Color emulator — DMG + CGB, Vulkan + SDL3"
arch=('x86_64')
url="https://github.com/amine/GB4ME"
license=('MIT')
depends=('sdl3' 'vulkan-icd-loader' 'hicolor-icon-theme' 'desktop-file-utils')
makedepends=('cmake' 'vulkan-headers' 'glslang' 'pkgconf')
optdepends=('xdg-desktop-portal: native file dialogs on Wayland')
source=("git+https://github.com/amine/GB4ME.git")
sha256sums=('SKIP')

build() {
  cmake -S "$srcdir/GB4ME" -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build build -j$(nproc)
}

check() {
  # headless sanity — no window
  ./build/bin/GB4ME --help >/dev/null
}

package() {
  DESTDIR="$pkgdir" cmake --install build

  # Desktop/icon/shaders/assets — handled here instead of CMake (clean)
  install -Dm644 GB4ME.desktop "$pkgdir/usr/share/applications/GB4ME.desktop"
  install -Dm644 assets/GB4ME.png "$pkgdir/usr/share/icons/hicolor/512x512/apps/GB4ME.png"
  install -Dm644 assets/gbc_body.png "$pkgdir/usr/share/GB4ME/assets/gbc_body.png"
  install -Dm644 shaders/quad.vert "$pkgdir/usr/share/GB4ME/shaders/quad.vert"
  install -Dm644 shaders/quad.frag "$pkgdir/usr/share/GB4ME/shaders/quad.frag"
  install -Dm644 shaders/gui.vert "$pkgdir/usr/share/GB4ME/shaders/gui.vert"
  install -Dm644 shaders/gui.frag "$pkgdir/usr/share/GB4ME/shaders/gui.frag"
  install -Dm644 build/bin/shaders/quad.vert.spv "$pkgdir/usr/share/GB4ME/shaders/quad.vert.spv"
  install -Dm644 build/bin/shaders/quad.frag.spv "$pkgdir/usr/share/GB4ME/shaders/quad.frag.spv"
  install -Dm644 build/bin/shaders/gui.vert.spv "$pkgdir/usr/share/GB4ME/shaders/gui.vert.spv"
  install -Dm644 build/bin/shaders/gui.frag.spv "$pkgdir/usr/share/GB4ME/shaders/gui.frag.spv"
}
