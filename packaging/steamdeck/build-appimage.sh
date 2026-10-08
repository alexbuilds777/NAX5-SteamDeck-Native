#!/bin/bash
# Сборка AppImage NAX5 для Steam Deck.
# Запускать внутри контейнера Debian 13 (distrobox), где собран build_deb и лежат userdeps/SDL3.
# Результат: NAX5-SteamDeck-x86_64.AppImage в корне репозитория (или путь из OUTPUT).
set -euo pipefail
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$SRC"
LINUXDEPLOY="${LINUXDEPLOY:-$HOME/nax5-build/tools/linuxdeploy-x86_64.AppImage}"
APPDIR="$SRC/AppDirDeb"
export OUTPUT="${OUTPUT:-$SRC/NAX5-SteamDeck-x86_64.AppImage}"

cmake --build build_deb -j"$(nproc)"
# Always from an empty AppDir: files left by earlier builds must not end up in the AppImage.
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install build_deb
# Headers and static libraries installed by bundled third-party code are not needed at run time.
rm -rf "$APPDIR/usr/include"
find "$APPDIR" \( -name '*.a' -o -name '*.la' -o -name '*.pc' \) -delete
cp -a build_deb/third-party/cpp-steam-tools/libcpp-steam-tools.so* "$APPDIR/usr/lib/"
cp -a userdeps/SDL3-3.4.0/build/libSDL3*.so* "$APPDIR/usr/lib/"
rm -f "$APPDIR/AppRun"
install -m 755 packaging/steamdeck/AppRun "$APPDIR/AppRun"

export APPIMAGE_EXTRACT_AND_RUN=1 NO_STRIP=1 QML_SOURCES_PATHS="$SRC/gui/src/qml"
export LD_LIBRARY_PATH="$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Vulkan, VA-API и DRM берутся из системы: они зависят от видеодрайвера.
"$LINUXDEPLOY" --appdir "$APPDIR" --plugin qt \
    --exclude-library "libvulkan.so*" --exclude-library "libva*.so*" --exclude-library "libdrm*.so*" \
    --output appimage \
    -d "$APPDIR/usr/share/applications/chiaking.desktop" \
    -i "$APPDIR/usr/share/icons/hicolor/512x512/apps/chiaking.png"
(cd "$(dirname "$OUTPUT")" && sha256sum "$(basename "$OUTPUT")" | tee "$(basename "$OUTPUT").sha256")
