#!/usr/bin/env bash
# Capture smoke screenshots of the Agora app on the CI emulator.
#
# The debug buildType is the emulator build: x86_64-only natives and applicationId
# com.newoether.agora.screenshots (release is arm64-v8a and cannot install on an
# x86_64 image). Invoked by .github/workflows/screenshots.yml inside
# reactivecircus/android-emulator-runner, so adb must be on PATH and an APK must
# already be built.
#
# Usage: scripts/emulator_screenshots.sh [apk] [outdir]
set -euo pipefail

pkg="com.newoether.agora.screenshots"
apk="${1:-app/build/outputs/apk/fdroid/debug/app-fdroid-debug.apk}"
out="${2:-screenshots}"

mkdir -p "$out"

wait_boot() {
  adb wait-for-device
  for _ in $(seq 1 90); do
    [ "$(adb shell getprop sys.boot_completed | tr -d '\r')" = "1" ] && break
    sleep 2
  done
  # Screenshots must not wait on animations.
  adb shell settings put global window_animation_scale 0
  adb shell settings put global transition_animation_scale 0
  adb shell settings put global animator_duration_scale 0
}

shot() {
  adb exec-out screencap -p > "$out/$1.png"
  echo "saved $out/$1.png ($(wc -c < "$out/$1.png") bytes)"
}

# Prints the centre of the first visible uiautomator node whose text or
# content-description contains one of the given patterns; exits 1 when there is
# none (e.g. the app already opened straight into the chat).
tap_matching() {
  adb shell uiautomator dump /sdcard/ui.xml >/dev/null 2>&1 || return 1
  adb pull /sdcard/ui.xml /tmp/agora_ui.xml >/dev/null 2>&1 || return 1
  python3 - "$@" <<'PY'
import sys
import xml.etree.ElementTree as ET

patterns = [p.lower() for p in sys.argv[1:]]
try:
    root = ET.parse("/tmp/agora_ui.xml").getroot()
except Exception:
    sys.exit(1)

for node in root.iter("node"):
    haystack = f"{node.get('text', '')} {node.get('content-desc', '')}".lower()
    if any(pattern in haystack for pattern in patterns):
        bounds = node.get("bounds") or ""
        coords = list(map(int, bounds.strip("[]").replace("][", ",").split(",")))
        if len(coords) != 4:
            continue
        print((coords[0] + coords[2]) // 2, (coords[1] + coords[3]) // 2)
        sys.exit(0)
sys.exit(1)
PY
}

wait_boot

echo "Installing $apk..."
adb install -r "$apk" >/dev/null

# Record the whole tour (launch -> first-run -> chat) as video next to the PNGs;
# screenrecord writes to the device and is pulled at the end.
adb shell "rm -f /sdcard/agora_tour.mp4" || true
adb shell "screenrecord --time-limit 40 --bit-rate 6000000 /sdcard/agora_tour.mp4" >/dev/null 2>&1 &
rec_pid=$!

echo "Launching $pkg..."
adb shell monkey -p "$pkg" -c android.intent.category.LAUNCHER 1 >/dev/null
sleep 12
shot 01_launch

# Onboarding: fresh install lands on Welcome; tap the first Skip/Start control we
# can find so the second shot shows the actual chat surface. Bilingual patterns
# cover the English and Russian first-run copy.
if xy=$(tap_matching skip start "let's go" пропустить продолжить начать); then
  echo "onboarding control at $xy"
  # shellcheck disable=SC2086
  adb shell input tap $xy
  sleep 8
  shot 02_after_onboarding
else
  echo "no onboarding control found; keeping the launch shot only"
fi

# Give screenrecord time to finalise the container, then pull it.
wait "$rec_pid" 2>/dev/null || true
adb pull /sdcard/agora_tour.mp4 "$out/tour.mp4" >/dev/null 2>&1 \
  && echo "saved $out/tour.mp4" \
  || echo "screenrecord unavailable; keeping stills only"

# Optional live view: expose the emulator's adb port (host 127.0.0.1:5555, raw adb
# protocol -- exactly what the adb server speaks) on this machine's tailnet address
# for LIVE_MINUTES, so the owner can watch and drive the screen from their own
# desktop with a native scrcpy window. Everything is captured first, so a missing
# secret or a short job budget only costs the live part.
live_minutes="${LIVE_MINUTES:-0}"
case "$live_minutes" in ''|*[!0-9]*) live_minutes=0 ;; esac
if [ "$live_minutes" -gt 0 ] && [ -n "${TS_AUTHKEY:-}" ]; then
  echo "enabling live view for ${live_minutes} minutes..."
  # Never fail the job on the live part: the captures are already on disk.
  {
    command -v tailscale >/dev/null || curl -fsSL https://tailscale.com/install.sh | sh
    command -v socat >/dev/null || sudo apt-get install -y --no-install-recommends socat
    # netfilter off: the runner's iptables belong to the platform (docker, gh cache),
    # and a service listening on the tailscale address does not need Tailscale's rules.
    sudo tailscale up --authkey="$TS_AUTHKEY" --netfilter-mode=off \
      --hostname="agora-emu-${GITHUB_RUN_ID:-local}" --accept-dns=false
  } || echo "tailscale setup failed; skipping live view"

  # The backend needs a moment to get an address after `up`.
  ts_ip=""
  for _ in $(seq 1 30); do
    ts_ip="$(tailscale ip -4 2>/dev/null | head -1 || true)"
    [ -n "$ts_ip" ] && break
    sleep 2
  done

  if [ -n "$ts_ip" ]; then
    # The emulator binds its adb port to the runner loopback only, so publish that
    # raw-adb endpoint on the tailnet address; from the client side this is a plain
    # `adb connect <ip>:5555`.
    if sudo socat TCP-LISTEN:5555,bind="$ts_ip",fork,reuseaddr TCP:127.0.0.1:5555 >/dev/null 2>&1 & \
       sleep 2 && ss -ltn 2>/dev/null | grep -q ":5555"; then
      echo "=== LIVE VIEW IS ON for ${live_minutes} min — on your machine run: ==="
      echo "    adb connect ${ts_ip}:5555"
      echo "    scrcpy -s ${ts_ip}:5555"
      sleep "$((live_minutes * 60))"
    else
      echo "could not publish port 5555 on ${ts_ip}; skipping live hold"
    fi
  else
    echo "no tailnet address after 60s; skipping live hold"
  fi
else
  echo "live view off (LIVE_MINUTES=${live_minutes}, TS_AUTHKEY $([ -n "${TS_AUTHKEY:-}" ] && echo set || echo 'not configured'))"
fi

echo "captured:"
ls -la "$out"
