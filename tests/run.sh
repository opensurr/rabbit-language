#!/bin/sh

cd "$(dirname "$0")" || exit 1
RABBIT="${RABBIT:-../bin/rabbit}"
RABBIT_LIB="${RABBIT_LIB:-../lib}"
export RABBIT_LIB
status=0
for program in *.rab; do
  name="${program%.rab}"
  if [ "$name" = "bridge" ] && ! command -v python3 >/dev/null 2>&1; then
    echo "skip  $name (python3 not installed)"
    continue
  fi
  actual=$("$RABBIT" "$program" 2>&1)
  if [ "$actual" = "$(cat "$name.expected")" ]; then
    echo "ok    $name"
  else
    echo "FAIL  $name"
    echo "$actual" | diff "$name.expected" - | sed 's/^/      /'
    status=1
  fi
done
exit $status
