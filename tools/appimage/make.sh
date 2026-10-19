#!/bin/bash
# Package build/sr2recomp as a personal-use AppImage that bundles its shared libraries (FFmpeg, SDL2, ...).
# Needs appimagetool (https://github.com/AppImage/appimagetool) in PATH or $APPIMAGETOOL, and the game
# folder for the icon:  tools/appimage/make.sh /path/to/game
set -e
cd "$(dirname "$0")/../.."
GAME=${1:?usage: $0 /path/to/game}
A=build/AppDir
# the AppImage runs on other people's CPUs, so it must not be built for this one
[ "$(cat build/march 2>/dev/null)" != native ] || { echo "build/sr2recomp was built with MARCH=native; rebuild with the default MARCH first"; exit 1; }
rm -rf $A; mkdir -p $A/usr/bin $A/usr/lib
cp build/sr2recomp $A/usr/bin/sr2recomp; strip $A/usr/bin/sr2recomp
# system libraries the host must provide itself (glibc, GL/X11/Wayland/audio drivers)
SKIP='^(linux-vdso|libc\.so|libm\.so|libdl\.so|libpthread\.so|librt\.so|ld-linux|libGL|libEGL|libGLX|libGLdispatch|libOpenGL|libX11|libxcb|libXext|libXau|libXdmcp|libdrm|libgbm|libwayland|libasound|libpulse|libz\.so)'
ldd $A/usr/bin/sr2recomp | awk '/=>/{print $1, $3}' | while read n p; do echo "$n" | grep -qE "$SKIP" || cp -L "$p" $A/usr/lib/; done
cp tools/appimage/AppRun tools/appimage/sr2recomp.desktop $A/
python3 -c "from PIL import Image; import sys; Image.open(sys.argv[1]).convert('RGBA').resize((256,256)).save(sys.argv[2])" \
  "$(find "$GAME" -ipath '*/PSP_GAME/ICON0.PNG' -print -quit)" $A/sr2recomp.png
ARCH=x86_64 ${APPIMAGETOOL:-appimagetool} -n $A build/SonicRivals2-recomp-x86_64.AppImage
