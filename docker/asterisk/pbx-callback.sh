#!/bin/sh
set -eu

target="${1:-}"
case "$target" in
  [0-9][0-9][0-9][0-9]) ;;
  *) exit 0 ;;
esac

sleep 2
for uri in $(asterisk -rx "pjsip show aor ${target}" | awk '/^ *contact *:/ {print $3}'); do
  asterisk -rx "channel originate PJSIP/${target}/${uri} application Echo" >/dev/null 2>&1 &
done
