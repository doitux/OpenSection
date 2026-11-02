#!/usr/bin/env bash
# OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
#
# Downloads the string samples (violin, viola, cello, double bass) of the University of Iowa
# Electronic Music Studios "Musical Instrument Samples" (https://theremin.music.uiowa.edu/MIS.html,
# "may be downloaded and used for any projects, without restrictions"). Every file is converted to
# FLAC (48 kHz, much smaller) right after its download; at the end one ZIP per recording series is
# written to ~/Downloads (Iowa-MIS-*.zip).
#
# Disk safety: before every download, conversion and ZIP the free space is checked. If less than
# the reserve (default 20 GB) would remain, the script stops cleanly. Everything loaded so far
# stays in ~/Downloads/Iowa-MIS-Streicher; a new start continues where it stopped.
#
#   bash fetch_iowa_mis.sh                   arco only, keep at least 20 GB free
#   bash fetch_iowa_mis.sh --reserve 40      keep at least 40 GB free
#   bash fetch_iowa_mis.sh --max 30          load at most 30 GB in total
#   bash fetch_iowa_mis.sh --mit-pizz        also pizzicato
set -uo pipefail

BASE="${MIS_BASE:-https://theremin.music.uiowa.edu}"
PIZZ=0; RESERVE_GB=20; MAX_GB=0
while [ $# -gt 0 ]; do
  case "$1" in
    --mit-pizz) PIZZ=1 ;;
    --reserve)  RESERVE_GB="${2:?Zahl in GB fehlt}"; shift ;;
    --max)      MAX_GB="${2:?Zahl in GB fehlt}"; shift ;;
    *) echo "unbekannte Option: $1"; exit 1 ;;
  esac
  shift
done
for t in curl zip unzip df du; do
  command -v "$t" >/dev/null || { echo "Bitte zuerst installieren: sudo apt install $t"; exit 1; }
done
FLAC=1; command -v ffmpeg >/dev/null || { FLAC=0; echo "Hinweis: ffmpeg fehlt – die AIFF-Dateien bleiben unverändert (viel größer). sudo apt install ffmpeg"; }

DL="$HOME/Downloads"
WORK="$DL/Iowa-MIS-Streicher"
mkdir -p "$WORK"
RESERVE_KB=$((RESERVE_GB * 1024 * 1024))
MAX_KB=$((MAX_GB * 1024 * 1024))

free_kb() { df -Pk "$DL" | awk 'NR==2 {print $4}'; }
gb() { awk -v k="$1" 'BEGIN {printf "%.1f", k / 1048576}'; }
used_kb() { du -sk "$WORK" 2>/dev/null | cut -f1; }

stop_full() {   # $1 = what needed the space
  echo
  echo "ANGEHALTEN ($1): nur noch $(gb "$(free_kb)") GB frei, Reserve ist $RESERVE_GB GB."
  echo "Bisher geladen: $(gb "$(used_kb)") GB in $WORK"
  echo "Platz schaffen und einfach neu starten – es geht dort weiter."
  exit 2
}
# room for $1 KB plus the reserve?
room_for() { [ $(( $(free_kb) - $1 )) -ge $RESERVE_KB ]; }

echo "Frei auf $(df -P "$DL" | awk 'NR==2 {print $6}'): $(gb "$(free_kb)") GB, Reserve $RESERVE_GB GB$([ "$MAX_GB" -gt 0 ] && echo ", höchstens $MAX_GB GB laden")"
room_for 0 || stop_full "Start"

convert() {   # AIFF -> FLAC right away (saves space during the whole run)
  local f="$1" kb
  [ $FLAC = 1 ] || return 0
  kb=$(du -k "$f" | cut -f1)
  room_for "$kb" || stop_full "Umwandeln"
  if nice ffmpeg -nostdin -v quiet -y -i "$f" -ar 48000 -c:a flac -compression_level 8 -f flac "${f%.*}.flac.part"; then
    mv "${f%.*}.flac.part" "${f%.*}.flac" && rm -f "$f"
  else                                   # not readable as audio: keep the original, carry on
    rm -f "${f%.*}.flac.part"; echo; echo "  Hinweis: konnte nicht umwandeln, bleibt als AIFF: $(basename "$f")"
  fi
  return 0
}

finish_zip() {   # $1 = zip file, $2 = folder: unpack (without macOS metadata), convert, mark done
  local z="$1" d="$2"
  if [ -e "$z" ]; then
    unzip -oq "$z" -d "$d" -x '__MACOSX/*' '*/__MACOSX/*' '._*' '*/._*' 2>&1 | grep -v 'excluded filename not matched'
    case "${PIPESTATUS[0]}" in
      0|11) rm -f "$z" ;;                # 11: an exclusion pattern simply did not occur
      *) echo; echo "  Hinweis: ZIP defekt, wird beim nächsten Start neu geladen: $(basename "$z")"; rm -rf "$z" "$d"; return 0 ;;
    esac
  fi
  rm -rf "$d/__MACOSX"; find "$d" -name '__MACOSX' -type d -prune -exec rm -rf {} + 2>/dev/null
  find "$d" -name '._*' -type f -delete 2>/dev/null
  find "$d" -type f \( -iname '*.aif' -o -iname '*.aiff' \) -print0 \
    | while IFS= read -r -d '' f; do convert "$f" || exit 2; done || exit 2
  touch "$d.done"
}

PAGES="MISviolin.html MISviola.html MIScello.html MISdoublebass.html
MISviolin2012.html MISviola2012.html MIScello2012.html MISdoublebass2012.html
MIS-Pitches-2012/MISViolin2012.html MIS-Pitches-2012/MISViola2012.html
MIS-Pitches-2012/MISCello2012.html MIS-Pitches-2012/MISDoubleBass2012.html"
# the menu on MIS.html may know more string pages than the list above
extra=$(curl -fsSL "$BASE/MIS.html" 2>/dev/null | grep -oiE 'href="[^"]*(violin|viola|cello|bass)[^"]*\.html"' \
        | sed -E 's/^href="//I; s/"$//; s#^https?://[^/]+/##; s#^/##' | grep -viE 'clarinet|trombone|flute|tuba|oon' || true)
PAGES=$(printf '%s\n%s\n' "$PAGES" "$extra" | tr ' ' '\n' | grep -v '^$' | sort -u)

resolve() {   # page-url href -> absolute url
  local page="$1" href="$2"
  href="${href// /%20}"
  case "$href" in
    http*) echo "$href" ;;
    /*)    echo "$BASE$href" ;;
    *)     echo "${page%/*}/$href" ;;
  esac
}

total=0; got=0; failed=0
for p in $PAGES; do
  url="$BASE/$p"
  html=$(curl -fsSL "$url" 2>/dev/null) || continue
  series=$(echo "$p" | sed -E 's#/#_#g; s#\.html$##')
  links=$(echo "$html" | grep -oiE 'href="[^"]+\.(aif|aiff|zip)"' | sed -E 's/^href="//I; s/"$//' | sort -u)
  [ -z "$links" ] && continue
  echo "$series: $(echo "$links" | wc -l) Dateien"
  mkdir -p "$WORK/$series"
  while IFS= read -r href; do
    name=$(basename "${href//%20/ }")
    if [ $PIZZ = 0 ] && echo "$name" | grep -qi 'pizz'; then continue; fi
    total=$((total + 1))
    dst="$WORK/$series/$name"; stem="${dst%.*}"
    if [ -e "$stem.flac" ] || [ -e "$stem.done" ]; then got=$((got + 1)); continue; fi
    case "$dst" in
      *.zip|*.ZIP)   # interrupted earlier: finish unpacking/converting without a new download
        if [ -e "$dst" ] || [ -d "$stem" ]; then finish_zip "$dst" "$stem" || exit 2; got=$((got + 1)); continue; fi ;;
      *) if [ -e "$dst" ]; then convert "$dst"; got=$((got + 1)); continue; fi ;;
    esac
    src=$(resolve "$url" "$href")
    # size of the file (if the server says it), otherwise assume 500 MB to be safe
    len=$(curl -fsSIL "$src" 2>/dev/null | tr -d '\r' | awk 'tolower($1) == "content-length:" {n = $2} END {print n + 0}')
    need=$(( len > 0 ? len / 1024 : 512000 ))
    case "$dst" in *.zip|*.ZIP) need=$((need * 3)) ;; esac      # zip + unpacked content
    room_for "$need" || stop_full "vor $name, braucht $(gb "$need") GB"
    if [ "$MAX_KB" -gt 0 ] && [ $(( $(used_kb) + need )) -gt "$MAX_KB" ]; then
      echo; echo "Grenze von $MAX_GB GB erreicht – höre hier auf und packe, was da ist."; break 2
    fi
    if curl -fsSL --retry 3 -o "$dst.part" "$src"; then
      mv "$dst.part" "$dst"; got=$((got + 1))
      printf '\r  %d geladen, frei %s GB ' "$got" "$(gb "$(free_kb)")"
      case "$dst" in
        *.zip|*.ZIP) finish_zip "$dst" "$stem" || exit 2 ;;
        *) convert "$dst" || exit 2 ;;
      esac
    else
      rm -f "$dst.part"; failed=$((failed + 1)); echo "  Fehler: $name"
    fi
  done <<< "$links"
  echo
done

echo "$got von $total Dateien vorhanden, $failed Fehler, zusammen $(gb "$(used_kb)") GB."
[ "$got" = 0 ] && { echo "Nichts geladen – ist die Seite erreichbar?"; exit 1; }

# one ZIP per series; each folder is removed right after its ZIP, so at most one series is doubled
for d in "$WORK"/*/; do
  [ -d "$d" ] || continue
  s=$(basename "$d"); z="$DL/Iowa-MIS-$s.zip"
  room_for "$(du -sk "$d" | cut -f1)" || stop_full "ZIP $s"
  rm -f "$z"
  (cd "$WORK" && zip -0 -qr "$z" "$s" -x '*.done' '*.part') && rm -rf "$d" && echo "  $(basename "$z") ($(du -h "$z" | cut -f1))"
done
rm -f "$WORK"/*.done; rmdir "$WORK" 2>/dev/null
echo "Fertig. Frei: $(gb "$(free_kb)") GB. Jetzt die Iowa-MIS-*.zip-Dateien im Chat an Claude anhängen."
