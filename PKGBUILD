# Maintainer: sulaweyo <sledge DOT sulaweyo AT gmail DOT com>
# Contributor: Paul Dunn <pwjdunn AT gmail DOT com>
# Contributor: sjuxax <sjuxax AT gmail DOT com>
# Contributor: amogus <amogussugomus AT proton DOT me>

pkgname=amogus-numa-daemon-git
pkgver=r8.gdaefcf8
pkgrel=1
pkgdesc="NUMA daemon with GPU-aware locality management and sched_ext-compatible cooperative behavior (amogus experimental fork)"
arch=('x86_64')
url="https://github.com/Am0g-us/amogus-numa-daemon"
license=('LGPL2.1')
depends=('bash' 'systemd')
makedepends=('git')
conflicts=('numad-git')
optdepends=(
  'logrotate: rotate /var/log/numad.log'
)
backup=('etc/numad.conf')
source=("git+https://github.com/Am0g-us/amogus-numa-daemon")
sha256sums=('SKIP')

pkgver() {
  cd "$srcdir/amogus-numa-daemon"
  printf "r%s.g%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
  cd "$srcdir/amogus-numa-daemon"
  make
}

package() {
  cd "$srcdir/amogus-numa-daemon"

  install -Dm755 numad "$pkgdir/usr/bin/numad"
  install -Dm644 numad.8 "$pkgdir/usr/share/man/man8/numad.8"

  install -Dm755 numad-wrapper "$pkgdir/usr/lib/numad/numad-wrapper"
  install -Dm644 numad.service "$pkgdir/usr/lib/systemd/system/numad.service"

  install -Dm644 numad.conf "$pkgdir/etc/numad.conf"
  install -Dm644 numad.logrotate "$pkgdir/etc/logrotate.d/numad"
}
