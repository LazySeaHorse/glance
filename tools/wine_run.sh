#!/usr/bin/env bash
# Usage: wine_run.sh <image> <outdir> [xdotool steps...]
# Each step is "sleep:SECONDS", "key:NAME", "click:X,Y", "wheel:X,Y,up|down,N", "shot:NAME", "drag:X1,Y1,X2,Y2"
# Needs: wine64, Xvfb running on :99 (plus a window manager such as openbox), xdotool, imagemagick.
here="$(cd "$(dirname "$0")" && pwd)"
exe="$here/../build/Glance.exe"
export WINEDEBUG=-all WINEPREFIX="${WINEPREFIX:-$HOME/.wine-glance}" DISPLAY="${DISPLAY:-:99}" PATH=$PATH:/usr/lib/wine GLANCE_TRACE=1
img="$1"; out="$2"; shift 2
mkdir -p "$out"
rm -f "$WINEPREFIX/drive_c/users/root/AppData/Local/Temp/glance-trace.txt" "$WINEPREFIX/drive_c/users/root/Temp/glance-trace.txt" 2>/dev/null
# solid "desktop" with a pattern so the dimmed background is visible
convert -size 1920x1080 plasma:steelblue-wheat -blur 0x8 /tmp/desk.png 2>/dev/null || true
(display -window root /tmp/desk.png >/dev/null 2>&1 &) ; sleep 0.5
wine64 "$exe" "Z:${img//\//\\}" > "$out/wine.log" 2>&1 &
pid=$!
for step in "$@"; do
  kind="${step%%:*}"; arg="${step#*:}"
  case "$kind" in
    sleep) sleep "$arg" ;;
    key) xdotool search --name Glance windowactivate --sync >/dev/null 2>&1; xdotool key "$arg" ;;
    click) IFS=, read x y <<< "$arg"; xdotool mousemove "$x" "$y" click 1 ;;
    move) IFS=, read x y <<< "$arg"; xdotool mousemove "$x" "$y" ;;
    dbl) IFS=, read x y <<< "$arg"; xdotool mousemove "$x" "$y" click --repeat 2 --delay 80 1 ;;
    wheel) IFS=, read x y dir n <<< "$arg"; b=4; [ "$dir" = down ] && b=5; xdotool mousemove "$x" "$y"; for i in $(seq "$n"); do xdotool click $b; done ;;
    drag) IFS=, read x1 y1 x2 y2 <<< "$arg"; xdotool mousemove "$x1" "$y1" mousedown 1; for i in $(seq 1 10); do xdotool mousemove $((x1+(x2-x1)*i/10)) $((y1+(y2-y1)*i/10)); sleep 0.02; done; xdotool mouseup 1 ;;
    shot) import -window root "$out/$arg.png" ;;
  esac
done
sleep 1
alive=$(pgrep -fc "Glance.exe" || true)  # 0 after a clean Escape
echo "still_running=$alive"
wineserver -k 2>/dev/null; sleep 1
find "$WINEPREFIX/drive_c" -name glance-trace.txt -exec cat {} \; 2>/dev/null
