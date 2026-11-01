#!/usr/bin/env bash
# OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
#
# Downloads the string samples (violin, viola, cello, double bass) of the University of Iowa
# Electronic Music Studios "Musical Instrument Samples" (https://theremin.music.uiowa.edu/MIS.html,
# "may be downloaded and used for any projects, without restrictions"), converts them to FLAC
# (48 kHz, much smaller) and packs everything into ~/Downloads/Iowa-MIS-Streicher.zip.
# The working folder is deleted afterwards. Already downloaded files are skipped on a re-run.
#
#   bash fetch_iowa_mis.sh              arco only (default), all recording series
#   bash fetch_iowa_mis.sh --mit-pizz   also pizzicato
#   bash fetch_iowa_mis.sh --behalten   keep the working folder (to resume later)
set -uo pipefail

BASE="${MIS_BASE:-https://theremin.music.uiowa.edu}"
PIZZ=0; KEEP=0
for a in "$@"; do
  case "$a" in
    --mit-pizz) PIZZ=1 ;;
    --behalten) KEEP=1 ;;
    *) echo "unbekannte Option: $a"; exit 1 ;;
  esac
done
for t in curl zip unzip; do
  command -v "$t" >/dev/null || { echo "Bitte zuerst installieren: sudo apt install $t"; exit 1; }
done
FLAC=1; command -v ffmpeg >/dev/null || { FLAC=0; echo "Hinweis: ffmpeg fehlt – die AIFF-Dateien werden unverändert gepackt (größer). sudo apt install ffmpeg"; }

WORK="$HOME/Downloads/Iowa-MIS-Streicher"
OUT="$HOME/Downloads/Iowa-MIS-Streicher.zip"
mkdir -p "$WORK"

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
  html=$(curl -fsSL "$url" 2>/dev/null) || { echo "  (Seite nicht erreichbar: $p)"; continue; }
  series=$(echo "$p" | sed -E 's#/#_#g; s#\.html$##')
  links=$(echo "$html" | grep -oiE 'href="[^"]+\.(aif|aiff|zip)"' | sed -E 's/^href="//I; s/"$//' | sort -u)
  [ -z "$links" ] && continue
  n=$(echo "$links" | wc -l); echo "$series: $n Dateien"
  mkdir -p "$WORK/$series"
  while IFS= read -r href; do
    name=$(basename "${href//%20/ }")
    if [ $PIZZ = 0 ] && echo "$name" | grep -qi 'pizz'; then continue; fi
    total=$((total + 1))
    dst="$WORK/$series/$name"
    stem="${dst%.*}"
    if [ -e "$dst" ] || [ -e "$stem.flac" ] || [ -e "$stem.done" ]; then got=$((got + 1)); continue; fi
    if curl -fsSL --retry 3 -o "$dst.part" "$(resolve "$url" "$href")"; then
      mv "$dst.part" "$dst"; got=$((got + 1))
      printf '\r  %d geladen ' "$got"
      case "$dst" in
        *.zip|*.ZIP) unzip -oq "$dst" -d "$stem" && rm -f "$dst" && touch "$stem.done" ;;
      esac
    else
      rm -f "$dst.part"; failed=$((failed + 1)); echo "  Fehler: $name"
    fi
  done <<< "$links"
  echo
done

if [ $FLAC = 1 ]; then
  echo "Wandle in FLAC (48 kHz) um …"
  find "$WORK" -type f \( -iname '*.aif' -o -iname '*.aiff' \) -print0 | while IFS= read -r -d '' f; do
    ffmpeg -nostdin -v error -y -i "$f" -ar 48000 -c:a flac -compression_level 8 "${f%.*}.flac" && rm -f "$f"
  done
fi
find "$WORK" -name '*.done' -delete

echo "$got von $total Dateien vorhanden, $failed Fehler."
[ "$got" = 0 ] && { echo "Nichts geladen – ist die Seite erreichbar?"; exit 1; }
rm -f "$OUT"
(cd "$HOME/Downloads" && zip -qr "$OUT" "Iowa-MIS-Streicher")
echo "Fertig: $OUT ($(du -h "$OUT" | cut -f1))"
if [ $KEEP = 0 ]; then rm -rf "$WORK"; echo "Arbeitsordner gelöscht."; fi
echo "Jetzt die ZIP-Datei im Chat an Claude anhängen."
