#!/usr/bin/env bash
# Install (or remove) the scoped NOPASSWD sudoers rule that lets the
# rust-monitor XFCE panel plugin set the ThinkPad fan level without a
# password prompt.
#
# The plugin runs exactly:  /usr/bin/sudo -n /usr/bin/tee /proc/acpi/ibm/fan
# so the rule is pinned to that one command and that one file argument.
# It grants nothing else: no shell, no other paths, no other commands.
#
# Usage:
#   scripts/install-fan-sudoers.sh             install for the invoking user
#   scripts/install-fan-sudoers.sh --user U    install for user U
#   scripts/install-fan-sudoers.sh --remove     uninstall the rule
#   scripts/install-fan-sudoers.sh --dry-run    print actions, change nothing
#
# Installing writes /etc/sudoers.d/rust-monitor-fan and needs root; if you are
# not root this re-executes under sudo (a single interactive password prompt).
# The generated rule is validated with `visudo -c` before it is installed and
# the merged sudoers is re-validated afterward, rolling back on any failure, so
# a bad rule can never lock you out of sudo.
set -euo pipefail

DROPIN=/etc/sudoers.d/rust-monitor-fan
TEE=/usr/bin/tee
FAN=/proc/acpi/ibm/fan

# Resolve visudo; it commonly lives in /usr/sbin, off a normal user's PATH.
VISUDO=$(command -v visudo 2>/dev/null || true)
if [ -z "$VISUDO" ]; then
  for p in /usr/sbin/visudo /sbin/visudo /usr/bin/visudo; do
    [ -x "$p" ] && { VISUDO=$p; break; }
  done
fi

usage() { awk 'NR>1 && /^#/ {sub(/^# ?/, ""); print; next} NR>1 {exit}' "$0"; }

mode=install
dry=0
user_arg=""
orig=("$@")
self=$(readlink -f "$0")

while [ $# -gt 0 ]; do
  case $1 in
    --remove)  mode=remove ;;
    --dry-run) dry=1 ;;
    --user)    user_arg=${2:?--user needs a value}; shift ;;
    --user=*)  user_arg=${1#--user=} ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

# Re-exec under sudo when root is required and we are not root.
if [ "$dry" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
  command -v sudo >/dev/null 2>&1 || { echo "sudo is required to modify $DROPIN" >&2; exit 1; }
  exec sudo -- "$self" ${orig[@]+"${orig[@]}"}
fi

# Resolve the target user: --user, then the pre-sudo user, then the current one.
if [ -n "$user_arg" ]; then
  user=$user_arg
elif [ -n "${SUDO_USER:-}" ] && [ "${SUDO_USER}" != root ]; then
  user=$SUDO_USER
else
  user=$(id -un)
fi
id "$user" >/dev/null 2>&1 || { echo "no such user: $user" >&2; exit 1; }

rule="$user ALL=(root) NOPASSWD: $TEE $FAN"

# ---- remove -----------------------------------------------------------------
if [ "$mode" = remove ]; then
  if [ ! -e "$DROPIN" ]; then
    echo "nothing to remove: $DROPIN is absent"
    exit 0
  fi
  if [ "$dry" -eq 1 ]; then
    echo "would remove $DROPIN"
    exit 0
  fi
  rm -f "$DROPIN"
  echo "removed $DROPIN"
  if [ -n "$VISUDO" ]; then
    if "$VISUDO" -c >/dev/null 2>&1; then
      echo "sudoers still valid"
    else
      echo "WARNING: sudoers validation failed after removal" >&2
      "$VISUDO" -c >&2 || true
    fi
  fi
  exit 0
fi

# ---- install ----------------------------------------------------------------
[ -x "$TEE" ] || echo "WARNING: $TEE not found; the plugin invokes this exact path" >&2
[ -e "$FAN" ] || echo "WARNING: $FAN not present (ThinkPad fan interface / thinkpad_acpi not loaded?)" >&2

content="# rust-monitor smart fan control
# Scoped NOPASSWD rule for the plugin's exact command; grants nothing else.
$rule
"

if [ -r "$DROPIN" ] && [ "$(cat "$DROPIN")" = "$(printf '%s' "$content")" ]; then
  echo "already installed: $DROPIN (no change)"
  exit 0
fi

if [ "$dry" -eq 1 ]; then
  echo "would write $DROPIN (mode 0440, root:root):"
  printf '%s' "$content"
  exit 0
fi

# Validate the fragment in isolation before it can affect the real sudoers.
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
printf '%s' "$content" > "$tmp"
if [ -z "$VISUDO" ]; then
  echo "visudo not found; cannot safely validate the rule, aborting" >&2
  exit 1
fi
if ! "$VISUDO" -c -f "$tmp" >/dev/null 2>&1; then
  echo "generated rule failed visudo validation; aborting" >&2
  "$VISUDO" -c -f "$tmp" >&2 || true
  exit 1
fi

install -m 0440 -o root -g root "$tmp" "$DROPIN"

# Confirm the merged sudoers is still valid; roll back if not.
if ! "$VISUDO" -c >/dev/null 2>&1; then
  echo "merged sudoers invalid after install; rolling back" >&2
  rm -f "$DROPIN"
  "$VISUDO" -c >&2 || true
  exit 1
fi

echo "installed $DROPIN:"
printf '  %s\n' "$rule"
echo
echo "Verify (no password prompt expected):"
echo "  sudo -n $TEE $FAN <<< 'level auto'"
