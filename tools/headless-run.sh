#!/usr/bin/env bash
# Drives rsync-ui on a private Xvfb display, so a UI change can be checked without
# a window appearing on anyone's desktop and without a desktop session at all.
#
#   usage: tools/headless-run.sh <outdir> <script-file>
#
# The script file holds one command per line, run against the app window:
#   click X Y      click at window-relative coordinates
#   key NAME       send a key, e.g. Escape
#   type TEXT      type a string
#   shot NAME      capture the window to <outdir>/NAME.png
#   wait SECONDS
#
# There is no window manager on the display, so the window sits at 0,0 with no
# title bar and screenshot coordinates map straight to click coordinates.
set -uo pipefail

OUT="${1:?usage: headless-run.sh <outdir> <script>}"
SCRIPT="${2:?usage: headless-run.sh <outdir> <script>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="${APP:-$HERE/../build/rsync-ui}"
DISP="${DISPLAY_NUM:-:99}"

for tool in Xvfb xdotool xwd python3; do
    command -v "$tool" > /dev/null || { echo "missing $tool"; exit 1; }
done
[ -x "$APP" ] || { echo "no binary at $APP, build first"; exit 1; }

# A leftover Xvfb on this display silently steals the run: the new server exits,
# the app connects to the stale one, and every capture comes back blank.
if [ -e "/tmp/.X${DISP#:}-lock" ]; then
    echo "display $DISP already in use. kill the old Xvfb and remove /tmp/.X${DISP#:}-lock"
    exit 1
fi

mkdir -p "$OUT"
Xvfb "$DISP" -screen 0 1220x860x24 > /dev/null 2>&1 &
XVFB_PID=$!
sleep 2
kill -0 $XVFB_PID 2>/dev/null || { echo "Xvfb failed to start on $DISP"; exit 1; }

cleanup() {
    kill $APP_PID 2>/dev/null
    wait $APP_PID 2>/dev/null
    kill $XVFB_PID 2>/dev/null
    rm -f "/tmp/.X${DISP#:}-lock"
}
trap cleanup EXIT

DISPLAY="$DISP" LIBGL_ALWAYS_SOFTWARE=1 "$APP" > "$OUT/app.log" 2>&1 &
APP_PID=$!
sleep 4

export DISPLAY="$DISP"
WID=$(xdotool search --name "^rsync-ui$" | head -1)
if [ -z "$WID" ]; then
    echo "no window appeared; app log:"
    cat "$OUT/app.log"
    exit 1
fi
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "window $WID at $X,$Y ${WIDTH}x${HEIGHT}"

while read -r cmd a b; do
    case "$cmd" in
        click) xdotool mousemove $((X + a)) $((Y + b)) click 1; sleep 0.4 ;;
        key)   xdotool key "$a"; sleep 0.3 ;;
        type)  xdotool type --delay 12 "$a $b"; sleep 0.3 ;;
        wait)  sleep "$a" ;;
        # Xvfb has no compositor and llvmpipe's front buffer is only reliable
        # right after a swap, so grab a few frames and keep one with content.
        shot)  for try in 1 2 3 4 5 6; do
                   xdotool mousemove $((X + 5 + try)) $((Y + 5))
                   sleep 0.12
                   xwd -id "$WID" -silent -out "$OUT/$a.$try.xwd"
               done
               python3 "$HERE/xwd2png.py" --best "$OUT/$a.png" "$OUT/$a".*.xwd
               rm -f "$OUT/$a".*.xwd ;;
        ""|\#*) ;;
        *) echo "unknown command: $cmd" ;;
    esac
done < "$SCRIPT"

echo "done"
