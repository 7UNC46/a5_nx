#!/bin/sh
# Pack a GitHub release zip from a finished build:
#   ./build.sh && launcher/build.sh && tools/make_release.sh
# -> a5_nx-v<version>.zip with the SD card layout (switch/a5_nx/a5_nx.nro),
#    README.md, LICENSE and THIRD_PARTY_NOTICES.md. No game files.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"

VERSION="$(sed -n 's/^#define PORT_VERSION "\(.*\)"/\1/p' source/port_config.h)"
[ -n "$VERSION" ] || { echo "no PORT_VERSION in source/port_config.h" >&2; exit 1; }
NRO="launcher/a5_nx.nro"
[ -f "$NRO" ] || { echo "$NRO missing: run ./build.sh and launcher/build.sh first" >&2; exit 1; }
for v in "Makefile:PORT_NPDM_VERSION" "launcher/Makefile:APP_VERSION"; do
  f="${v%%:*}"; k="${v#*:}"
  grep -q "^$k *:= *$VERSION\$" "$f" || echo "warning: $k in $f is not $VERSION" >&2
done
# an APK or game data must never end up in a release
for x in apk cnk glsnd bar; do
  if ls "$HERE"/launcher/*."$x" >/dev/null 2>&1; then
    echo "a .$x file lies in launcher/: not packing game files" >&2; exit 1
  fi
done

OUT="$HERE/a5_nx-v$VERSION.zip"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/switch/a5_nx"
cp "$NRO" "$STAGE/switch/a5_nx/a5_nx.nro"
cp README.md LICENSE THIRD_PARTY_NOTICES.md "$STAGE/"
rm -f "$OUT"
if command -v zip >/dev/null 2>&1; then
  (cd "$STAGE" && zip -qr9 "$OUT" .)
else
  python3 -c 'import shutil,sys; shutil.make_archive(sys.argv[1][:-4], "zip", sys.argv[2])' "$OUT" "$STAGE"
fi
echo "$OUT"
(cd "$STAGE" && find . -type f | sort)
