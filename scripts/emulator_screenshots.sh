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
# Cold start on the emulator shows a splash for a while; give it time to paint the
# real UI before the first frame (12s captured only the white splash).
sleep 22
shot 01_launch

# Dismiss whatever covers the UI on a fresh install. Best effort: tap the first
# matching visible control, or say so in the log when nothing matched (the
# notification dialog shows up asynchronously, after the onboarding tap).
dismiss() {
  if xy=$(tap_matching "$@"); then
    echo "dismissing via control at $xy ($*)"
    # shellcheck disable=SC2086
    adb shell input tap $xy
    sleep 3
    return 0
  fi
  echo "no control matching '$*' visible"
  return 1
}
clean_ui() {
  # Order matters: a system window sits above everything (the cold-boot Pixel
  # Launcher ANR appears nondeterministically and blocks taps), then the
  # permission dialog, then the in-app update card. Repeat: each appears with
  # its own delay.
  for _ in 1 2 3; do
    dismiss "wait" "ждать" || true
    dismiss "don't allow" "don’t allow" "не разрешать" || true
    dismiss "later" "позже" || true
  done
}

clean_ui

# Machine-readable companion to a frame: Compose publishes its semantics to uiautomator, so a
# dump records whether a control exists and whether it is enabled - reachable without eyeballing
# pixels. Skip's alpha and its enabled flag share one condition, so disabled == invisible.
dump_ui() {
  adb shell uiautomator dump /sdcard/agora_ui.xml >/dev/null 2>&1 || true
  adb pull /sdcard/agora_ui.xml "$out/$1.xml" >/dev/null 2>&1 || true
}

if [ "${FLOW:-main}" = "onboarding" ]; then
  # Diagnostic flow: page through every onboarding screen instead of skipping it. Every step
  # writes a frame AND a uiautomator dump - the dump says whether the Skip control exists and
  # whether it is enabled on that page. Steps wait past the intro gate (showContent keys the
  # fade) and the 500ms pager animation.
  echo "flow=onboarding: paging all 8 pages, frame + UI dump each"
  for i in 0 1 2 3 4 5 6 7; do
    if [ "$i" -gt 0 ]; then
      adb shell input swipe 900 1300 150 1300 350
      sleep 2.4
    fi
    shot "page_${i}"
    dump_ui "page_${i}"
  done
  # Slide two pages back: the report is about a vanishing control, so returns matter too.
  adb shell input swipe 150 1300 900 1300 350
  sleep 2.4
  shot "page_back_6"
  dump_ui "page_back_6"
  adb shell input swipe 150 1300 900 1300 350
  sleep 2.4
  shot "page_back_5"
  dump_ui "page_back_5"

  # Fast burst: three swipes back to back with no settle pause. If recomposition is still
  # restarting the intro-gate effect, the content alpha never reaches 1 and every control -
  # Skip included - stays invisible right after each swipe.
  echo "flow=onboarding: fast burst, no settle pause"
  for i in 1 2 3; do
    adb shell input swipe 900 1300 150 1300 300
    sleep 0.45
    shot "burst_${i}"
    dump_ui "burst_${i}"
  done
  sleep 2.5
  shot "burst_after_pause"
  dump_ui "burst_after_pause"

  # Rotation recreates the activity. showContent is a plain remember (not rememberSaveable), so
  # a recreation should blank every control for ~2s - the "Skip disappeared" case a real device
  # hits on rotate, locale switch or after the app was backgrounded and its process recycled.
  echo "flow=onboarding: rotate to force activity recreation"
  adb shell settings put system accelerometer_rotation 0 >/dev/null 2>&1 || true
  adb shell content insert --uri content://settings/system --bind name:s:user_rotation --bind value:i:1 >/dev/null 2>&1 || true
  sleep 0.7
  shot "rot_immediate"
  dump_ui "rot_immediate"
  sleep 3.5
  shot "rot_after_3s"
  dump_ui "rot_after_3s"
  adb shell content insert --uri content://settings/system --bind name:s:user_rotation --bind value:i:0 >/dev/null 2>&1 || true
  sleep 2
  shot "rot_back"
  dump_ui "rot_back"

  # Intro timeline: relaunch and sample the first seconds densely. Skip is the escape hatch, so
  # the first frame that shows the walkthrough at all must already carry the control.
  echo "flow=onboarding: intro timeline after relaunch"
  adb shell am force-stop "$pkg"
  adb shell monkey -p "$pkg" -c android.intent.category.LAUNCHER 1 >/dev/null
  for t in $(seq 1 14); do
    sleep 0.6
    shot "intro_${t}"
    dump_ui "intro_${t}"
  done
else
# Onboarding: fresh install lands on Welcome; tap the first Skip/Start control we
# can find so the second shot shows the actual chat surface. Bilingual patterns
# cover the English and Russian first-run copy. One retry after a cleanup pass -
# a system dialog may have been covering the control the first time around.
onboarded=1
for _ in 1 2; do
  if xy=$(tap_matching skip start "let's go" пропустить продолжить начать); then
    echo "onboarding control at $xy"
    # shellcheck disable=SC2086
    adb shell input tap $xy
    sleep 8
    onboarded=0
    break
  fi
  clean_ui
  sleep 3
done
if [ "$onboarded" -eq 1 ]; then
  echo "no onboarding control found; assuming no onboarding"
fi
clean_ui
# Drop the keyboard the dismissal taps may have opened (tap an empty area - a BACK
# keyevent could finish the root activity on some builds).
adb shell input tap 540 700 >/dev/null 2>&1 || true
sleep 2
shot 02_after_onboarding
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
live_tunnel="${LIVE_TUNNEL:-tailscale}"  # tailscale | ssh | none
hold_seconds=$((live_minutes * 60))

# Prints the connection recipe to the job log and to the Actions step summary, so the owner
# can copy the two commands without digging through raw logs.
announce_live() {
  local serial="$1"
  local warn="${2:-}"
  echo "=== LIVE VIEW IS ON for ${live_minutes} min - on your machine run: ==="
  echo "    adb connect ${serial}"
  echo "    scrcpy -s ${serial}"
  if [ -n "$warn" ]; then
    echo "    NOTE: ${warn}"
  fi
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
      echo "### Live view: next ${live_minutes} minutes"
      echo '```sh'
      echo "adb connect ${serial}"
      echo "scrcpy -s ${serial}"
      echo '```'
      if [ -n "$warn" ]; then
        echo "> **Warning:** ${warn}"
      fi
    } >> "$GITHUB_STEP_SUMMARY"
  fi
}

if [ "$live_minutes" -le 0 ]; then
  echo "live view off (LIVE_MINUTES=0)"
elif [ "$live_tunnel" = "none" ]; then
  echo "live view off (tunnel=none)"
elif [ "$live_tunnel" = "ssh" ]; then
  # Keyless mode: a reverse SSH tunnel through serveo.net publishes the loopback adb port for
  # exactly the hold duration - no account, no secret (localhost.run rejects anonymous
  # forwarding with port 0, serveo allocates one). PUBLIC endpoint: anyone who learns the URL
  # can speak adb while the hold lasts. Keep live_minutes small.
  echo "starting keyless SSH reverse tunnel (mode=ssh, serveo.net)..."
  rm -f /tmp/tunnel.log
  ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
      -o ServerAliveInterval=30 -o ExitOnForwardFailure=yes -o ConnectTimeout=15 \
      -R 0:127.0.0.1:5555 -N -n serveo.net >/tmp/tunnel.log 2>&1 &
  tunnel_pid=$!
  endpoint=""
  for _ in $(seq 1 45); do
    port=$(sed -nE 's/.*Allocated port +([0-9]+).*/\1/p' /tmp/tunnel.log 2>/dev/null | head -1 | tr -d '\r')
    forwarded=$(sed -nE 's/.*[Ff]orwarding TCP connections from +([^ ]+).*/\1/p' /tmp/tunnel.log 2>/dev/null | head -1 | tr -d '\r')
    if [ -n "$port" ]; then
      endpoint="serveo.net:${port}"
      break
    fi
    if [ -n "$forwarded" ]; then
      endpoint="${forwarded}"
      break
    fi
    kill -0 "$tunnel_pid" 2>/dev/null || break
    sleep 1
  done
  if [ -z "$endpoint" ]; then
    echo "ssh tunnel failed; output:"
    cat /tmp/tunnel.log 2>/dev/null || true
  elif timeout 6 bash -c "exec 3<>/dev/tcp/${endpoint%:*}/${endpoint##*:}" 2>/dev/null; then
    announce_live "$endpoint" \
      "mode=ssh - the adb endpoint is PUBLIC for the next ${live_minutes} min. Disconnect (adb disconnect) when done."
    sleep "$hold_seconds"
    kill "$tunnel_pid" 2>/dev/null || true
  else
    echo "tunnel endpoint ${endpoint} is not reachable; skipping live hold"
    kill "$tunnel_pid" 2>/dev/null || true
  fi
elif [ "$live_tunnel" = "tailscale" ]; then
  if [ -z "${TS_AUTHKEY:-}" ]; then
    echo "live view off: tunnel=tailscale but TS_AUTHKEY is not configured."
    echo "  create one: gh secret set TS_AUTHKEY --body tskey-auth-...  (tailscale.com admin -> Keys)"
  else
    echo "enabling live view for ${live_minutes} minutes (tailscale)..."
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
        announce_live "${ts_ip}:5555" ""
        sleep "$hold_seconds"
      else
        echo "could not publish port 5555 on ${ts_ip}; skipping live hold"
      fi
    else
      echo "no tailnet address after 60s; skipping live hold"
    fi
  fi
else
  echo "unknown LIVE_TUNNEL=${live_tunnel} (expected tailscale|ssh|none); live view off"
fi

echo "captured:"
ls -la "$out"
