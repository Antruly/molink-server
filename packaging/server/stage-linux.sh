#!/usr/bin/env bash
# Build molink-server and the admin SPA, then stage a Linux install tree.
# Inputs: VERSION, BUILD_DIR, STAGE_DIR, CMAKE_PREFIX_PATH, UVCPP_LIB_DIR.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-0.3.3}
BUILD=${BUILD_DIR:-$ROOT/build/server}
STAGE=${STAGE_DIR:-$BUILD/root}
UI=$ROOT/ui

rm -rf "$STAGE"
mkdir -p "$STAGE/opt/molink-server/bin" "$STAGE/opt/molink-server/lib" \
         "$STAGE/opt/molink-server/share/web-admin" \
         "$STAGE/etc/molink-server" "$STAGE/lib/systemd/system" \
         "$STAGE/usr/bin"

CMAKE_ARGS=(-S "$ROOT" -B "$BUILD/cmake" -DCMAKE_BUILD_TYPE=Release
            -DBUILD_TESTING=OFF)
if [ -n "${CMAKE_PREFIX_PATH:-}" ]; then
  CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH")
fi
if [ -n "${UVCPP_ROOT:-}" ] && [ -z "${CMAKE_PREFIX_PATH:-}" ]; then
  CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=$UVCPP_ROOT")
fi

echo "== build molink-server"
cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD/cmake" --parallel

install -m 0755 "$BUILD/cmake/molink-server" \
  "$STAGE/opt/molink-server/bin/molink-server"

if [ ! -f "$UI/dist/index.html" ]; then
  echo "== build admin UI"
  (cd "$UI" && pnpm install --frozen-lockfile && pnpm build)
fi
cp -a "$UI/dist/." "$STAGE/opt/molink-server/share/web-admin/"

LIBDIR=${UVCPP_LIB_DIR:-}
if [ -z "$LIBDIR" ]; then
  LIBDIR=$(pkg-config --variable=libdir uvcpp 2>/dev/null || true)
fi
if [ -n "$LIBDIR" ] && [ -d "$LIBDIR" ]; then
  for pattern in 'libuvcpp.so*' 'libllhttp.so*'; do
    cp -P "$LIBDIR"/$pattern "$STAGE/opt/molink-server/lib/" 2>/dev/null || true
  done
fi

install -m 0644 "$ROOT/packaging/linux/molink-server.service" \
  "$STAGE/lib/systemd/system/molink-server.service"
install -m 0644 "$ROOT/packaging/linux/molink-server-config.json" \
  "$STAGE/etc/molink-server/config.json"
# Reference template next to the executable: relative cer/ paths and a
# self-signed pair generated on first start.
install -m 0644 "$ROOT/packaging/linux/molink-server-config.example.json" \
  "$STAGE/opt/molink-server/config.example.json"
install -m 0644 "$ROOT/LICENSE" "$STAGE/opt/molink-server/LICENSE"
install -m 0755 "$ROOT/packaging/linux/install-server.sh" "$STAGE/install.sh"
install -m 0755 "$ROOT/packaging/linux/uninstall-server.sh" "$STAGE/uninstall.sh"
ln -sf ../opt/molink-server/bin/molink-server "$STAGE/usr/bin/molink-server"

# Icon for the admin-console desktop entry, plus a copy inside the install tree
# so tarball installs and package managers carry the same artwork.
for dir in hicolor/scalable/apps hicolor/128x128/apps hicolor/256x256/apps; do
  mkdir -p "$STAGE/usr/share/icons/$dir"
done
install -m 0644 "$ROOT/packaging/icons/molink-server.svg" \
  "$STAGE/usr/share/icons/hicolor/scalable/apps/molink-server.svg"
install -m 0644 "$ROOT/packaging/icons/molink-server-128.png" \
  "$STAGE/usr/share/icons/hicolor/128x128/apps/molink-server.png"
install -m 0644 "$ROOT/packaging/icons/molink-server-256.png" \
  "$STAGE/usr/share/icons/hicolor/256x256/apps/molink-server.png"
install -D -m 0644 "$ROOT/packaging/linux/molink-server.desktop" \
  "$STAGE/usr/share/applications/molink-server.desktop"
install -D -m 0644 "$ROOT/packaging/icons/molink-server.svg" \
  "$STAGE/opt/molink-server/share/icons/molink-server.svg"
install -D -m 0644 "$ROOT/packaging/icons/molink-server-256.png" \
  "$STAGE/opt/molink-server/share/icons/molink-server.png"

echo "staged $STAGE (version $VERSION)"
