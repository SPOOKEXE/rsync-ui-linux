#!/usr/bin/env bash
# Checks that the built binary actually opens a window and draws something.
# Catches the failures a compile cannot: no GL, GLFW refusing to init, a crash
# before the first frame, a UI that renders nothing at all.
#
#   usage: tools/smoke-test.sh [outdir]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-${TMPDIR:-/tmp}/rsync-ui-smoke}"

rm -rf "$OUT"
mkdir -p "$OUT"
cat > "$OUT/script.txt" <<'EOF'
wait 1
shot startup
EOF

"$HERE/headless-run.sh" "$OUT" "$OUT/script.txt"

# A window that opens but paints nothing is still a failure, so check the frame
# has real content rather than just trusting that the process stayed alive.
python3 - "$OUT/startup.png" <<'PY'
import sys
from PIL import Image

img = Image.open(sys.argv[1])
lit = sum(1 for p in img.convert("RGB").getdata() if p != (0, 0, 0))
total = img.size[0] * img.size[1]
print(f"{lit} of {total} pixels painted")
if lit < total // 100:
    sys.exit("window opened but drew almost nothing")
PY

echo "smoke test passed"
