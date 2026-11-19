#!/bin/sh
# Picker lifecycle. Hardware qualification of launch/preload/reboot remains BENCH-GATED.
ROOT=/usrdata/ascent/widgets/picker
# The PID file persists across boots; a reused PID is not our supervisor.
# Never suppress startup or signal another process based on that number alone.
supervisor_running() {
  oldpid=$(cat "$ROOT/supervisor.pid" 2>/dev/null)
  case "$oldpid" in ''|*[!0-9]*) return 1;; esac
  kill -0 "$oldpid" 2>/dev/null || return 1
  cmd=$(tr '\000' ' ' < "/proc/$oldpid/cmdline" 2>/dev/null) || return 1
  [ "$cmd" = "/bin/sh $ROOT/picker-start.sh supervise " ] ||
    [ "$cmd" = "$ROOT/picker-start.sh supervise " ]
}
case "$1" in
 stop|supervise-stop)
   if supervisor_running; then
     kill "$oldpid" 2>/dev/null || true
     i=0
     while kill -0 "$oldpid" 2>/dev/null && [ $i -lt 5 ]; do i=$((i+1)); sleep 1; done
     supervisor_running && exit 1
   fi
   rm -f "$ROOT/supervisor.pid"
   [ "$1" != supervise-stop ] || exit 0
   killall ascent_picker_rt ascent_picker_overlay 2>/dev/null || true
   i=0
   while pidof ascent_picker_rt ascent_picker_overlay >/dev/null && [ $i -lt 5 ]; do i=$((i+1)); sleep 1; done
   pidof ascent_picker_rt ascent_picker_overlay >/dev/null && exit 1
   exit 0 ;;
 supervise)
   echo $$ > "$ROOT/supervisor.pid"
   trap 'rm -f "$ROOT/supervisor.pid"; exit 0' TERM INT EXIT
   for f in ascent_picker_geom ascent_picker_size; do
     [ ! -f "$ROOT/layout/$f" ] || cp "$ROOT/layout/$f" "/tmp/$f"
   done
   while [ -x "$ROOT/ascent_picker_rt" ]; do
     pidof ascent_picker_rt >/dev/null || "$ROOT/ascent_picker_rt" >> "$ROOT/runtime.log" 2>&1 &
     pidof ascent_picker_overlay >/dev/null || "$ROOT/ascent_picker_overlay" >> "$ROOT/overlay.log" 2>&1 &
     mkdir -p "$ROOT/layout"
     for f in ascent_picker_geom ascent_picker_size; do
       if [ -f "/tmp/$f" ] && ! cmp -s "/tmp/$f" "$ROOT/layout/$f"; then
         cp "/tmp/$f" "$ROOT/layout/$f.new" && mv "$ROOT/layout/$f.new" "$ROOT/layout/$f"
       fi
     done
     sleep 3
   done
   exit 0 ;;
 start|"") ;;
 *) exit 2 ;;
esac
[ ! -f "$ROOT/.unloaded" ] || exit 0
[ -x "$ROOT/ascent_picker_rt" ] || exit 1
[ ! -f "$ROOT/ascent.manifest" ] && exit 1
(cd "$ROOT" && sha256sum -c -s ascent.manifest) || exit 1
if supervisor_running; then exit 0; fi
"$ROOT/picker-start.sh" supervise >/dev/null 2>&1 &
# Reuse Mapster's attach pattern; one attempt, stock monitor provides recovery.
SHIM=$ROOT/libascent_frame_overlay.so
for p in $(pidof GlassesUI); do
  grep -q ascent_frame_overlay "/proc/$p/maps" && exit 0
done
i=0
while ! pidof GlassesUI >/dev/null && [ $i -lt 40 ]; do i=$((i+1)); sleep 1; done
pidof GlassesUI >/dev/null || exit 0
ARGS=$(tr '\0' ' ' < "/proc/$(pidof GlassesUI | cut -d' ' -f1)/cmdline")
ARGS=${ARGS#*GlassesUI }
kill $(pidof GlassesUI)
i=0
while pidof GlassesUI >/dev/null && [ $i -lt 5 ]; do i=$((i+1)); sleep 1; done
QT_QPA_PLATFORM=linuxfb:size=1920x1080 QT_QPA_FB_CUSTOM=1 LD_PRELOAD=$SHIM /ar8030soc/GlassesUI $ARGS >> "$ROOT/glassesui.log" 2>&1 &
