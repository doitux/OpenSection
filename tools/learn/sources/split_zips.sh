#!/usr/bin/env bash
# OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
#
# Repacks ZIP files that are too big for an upload into several independent ZIPs
# (each one complete on its own, no joining needed): Name-teil1.zip, Name-teil2.zip, …
# The original is only deleted after all parts were written successfully.
#
#   bash split_zips.sh                      all Iowa-MIS-*.zip in ~/Downloads, max 350 MiB each
#   bash split_zips.sh --max 300            other limit (MiB)
#   bash split_zips.sh --muster 'X*.zip'    other files
set -uo pipefail

MAX_MIB=350; PATTERN='Iowa-MIS-*.zip'; DIR="$HOME/Downloads"; RESERVE_GB=20
while [ $# -gt 0 ]; do
  case "$1" in
    --max) MAX_MIB="${2:?Zahl fehlt}"; shift ;;
    --muster) PATTERN="${2:?Muster fehlt}"; shift ;;
    *) echo "unbekannte Option: $1"; exit 1 ;;
  esac
  shift
done
for t in zip unzip df du stat; do command -v "$t" >/dev/null || { echo "Bitte installieren: sudo apt install $t"; exit 1; }; done
LIMIT=$((MAX_MIB * 1024 * 1024))
free_kb() { df -Pk "$DIR" | awk 'NR==2 {print $4}'; }

cd "$DIR" || exit 1
shopt -s nullglob
todo=0
for z in $PATTERN; do
  case "$z" in *-teil[0-9]*.zip) continue ;; esac
  size=$(stat -c %s "$z")
  if [ "$size" -le "$LIMIT" ]; then echo "ok (klein genug): $z"; continue; fi
  todo=$((todo + 1))
  # room for the unpacked content + the new parts, plus a reserve
  need_kb=$(( size / 1024 * 2 + RESERVE_GB * 1024 * 1024 ))
  [ "$(free_kb)" -gt "$need_kb" ] || { echo "Zu wenig Platz für $z – übersprungen."; continue; }
  base="${z%.zip}"; tmp="$DIR/.stueckeln-$$"
  rm -rf "$tmp"; mkdir -p "$tmp"
  echo "Stückle $z ($((size / 1048576)) MiB) …"
  if ! unzip -q "$z" -d "$tmp"; then echo "  ZIP lässt sich nicht entpacken – übersprungen."; rm -rf "$tmp"; continue; fi
  rm -f "$base"-teil*.zip
  part=1; acc=0; list=()
  flush() {
    [ ${#list[@]} -gt 0 ] || return 0
    (cd "$tmp" && printf '%s\n' "${list[@]}" | zip -0 -q "$DIR/$base-teil$part.zip" -@) || return 1
    echo "  $base-teil$part.zip ($(( $(stat -c %s "$DIR/$base-teil$part.zip") / 1048576 )) MiB, ${#list[@]} Dateien)"
    part=$((part + 1)); acc=0; list=()
  }
  ok=1
  while IFS= read -r -d '' f; do
    rel="${f#"$tmp"/}"; s=$(stat -c %s "$f")
    if [ $((acc + s + 65536)) -gt "$LIMIT" ] && [ ${#list[@]} -gt 0 ]; then flush || { ok=0; break; }; fi
    [ "$s" -gt "$LIMIT" ] && echo "  Achtung: einzelne Datei größer als $MAX_MIB MiB: $rel"
    list+=("$rel"); acc=$((acc + s + 200))
  done < <(find "$tmp" -type f -print0 | sort -z)
  [ $ok = 1 ] && flush || ok=0
  rm -rf "$tmp"
  if [ $ok = 1 ]; then rm -f "$z"; echo "  Original gelöscht."; else echo "  Fehler beim Packen – Original bleibt."; rm -f "$base"-teil*.zip; fi
done
[ $todo = 0 ] && echo "Nichts zu stückeln."
echo "Fertig. Alle Dateien sind jetzt höchstens $MAX_MIB MiB groß:"
ls -1sh $PATTERN 2>/dev/null
