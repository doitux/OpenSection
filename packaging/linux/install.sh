#!/usr/bin/env bash
# OpenSection – install the Linux build for the current user (no root needed)
#   bash install.sh            install VST3, CLAP, LV2 and the standalone app
#   bash install.sh --remove   uninstall
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
G=$'\e[32m'; R=$'\e[31m'; N=$'\e[0m'; [[ -t 1 ]] || { G=; R=; N=; }
ok()  { printf '  %s✔%s %s\n' "$G" "$N" "$*"; }
bad() { printf '  %s✘%s %s\n' "$R" "$N" "$*"; }

T_VST3="$HOME/.vst3/OpenSection.vst3"; T_CLAP="$HOME/.clap/OpenSection.clap"; T_LV2="$HOME/.lv2/OpenSection.lv2"
T_APP="$HOME/.local/bin/OpenSection"; T_DESK="$HOME/.local/share/applications/opensection.desktop"

if [[ "${1:-}" == "--remove" ]]; then
  rm -rf "$T_VST3" "$T_CLAP" "$T_LV2" "$T_APP" "$T_DESK" && ok "OpenSection entfernt / removed"; exit 0
fi
pgrep -x reaper >/dev/null && echo "  ! Reaper läuft – nach der Installation neu starten / restart Reaper afterwards"

mkdir -p "$HOME/.vst3" "$HOME/.clap" "$HOME/.lv2" "$HOME/.local/bin" "$HOME/.local/share/applications"
fail=0
cp_item() { rm -rf "$2"; cp -r "$HERE/$1" "$2" && ok "$1 → $2" || { bad "$1"; fail=1; }; }
cp_item OpenSection.vst3 "$T_VST3"
cp_item OpenSection.clap "$T_CLAP"
cp_item OpenSection.lv2 "$T_LV2"
cp_item OpenSection "$T_APP" && chmod +x "$T_APP"
cat > "$T_DESK" <<EOF
[Desktop Entry]
Type=Application
Name=OpenSection
Comment=Computed string section
Exec=$T_APP
Categories=AudioVideo;Audio;Music;
EOF

# check: the plugin binaries must load (all libraries present)
for so in "$T_VST3/Contents/x86_64-linux/OpenSection.so" "$T_CLAP" "$T_LV2"/*.so; do
  [[ -f $so ]] || continue
  miss=$(ldd "$so" 2>/dev/null | grep "not found")
  [[ -z $miss ]] && ok "Bibliotheken vollständig: $(basename "$so")" || { bad "fehlt: $miss"; fail=1; }
done
echo
[[ $fail -eq 0 ]] && echo "${G}Fertig.${N} In Reaper: Optionen → Einstellungen → Plug-ins → VST/CLAP → Re-scan, dann FX „OpenSection“." \
                  || { echo "${R}Nicht alles ok.${N}"; exit 1; }
