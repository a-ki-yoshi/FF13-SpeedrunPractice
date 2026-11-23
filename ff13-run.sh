#!/usr/bin/env bash
# FF13-SpeedrunPractice launcher for Linux (Steam / Proton) -- practice tools OFF, for measured runs.
# Parks dinput8.dll as dinput8.dll.off (so the mod cannot load) and starts the game through
# Steam. Idempotent: declares a state, does not toggle. Same steps as ff13-run.bat.
cd "$(dirname "$0")" || exit 1
TITLE="FF13-SpeedrunPractice"
TAG="ff13-srpractice"
APP=292120

# Started from a file manager there is no terminal to read, so messages also go to a window.
gui() {  # gui error|info TEXT
    [ -t 1 ] && return
    if [ "$1" = info ]; then
        if command -v notify-send >/dev/null 2>&1; then notify-send "$TITLE" "$2"
        elif command -v kdialog >/dev/null 2>&1; then kdialog --title "$TITLE" --passivepopup "$2" 5 &
        fi
    elif command -v kdialog >/dev/null 2>&1; then kdialog --title "$TITLE" --error "$2"
    elif command -v zenity >/dev/null 2>&1; then zenity --error --no-markup --title "$TITLE" --text "$2"
    elif command -v notify-send >/dev/null 2>&1; then notify-send -u critical "$TITLE" "$2"
    fi
} 2>/dev/null
stop() { local m; m="$(printf '%s\n' "$@")"; printf '[!] %s\n' "$m"; gui error "$m"; exit 1; }

launch() {
    local url="steam://rungameid/$APP"
    echo "$1"
    if command -v steam >/dev/null 2>&1; then
        setsid steam "$url" </dev/null >/dev/null 2>&1 &
    elif command -v flatpak >/dev/null 2>&1 && flatpak info com.valvesoftware.Steam >/dev/null 2>&1; then
        setsid flatpak run com.valvesoftware.Steam "$url" </dev/null >/dev/null 2>&1 &
    elif command -v xdg-open >/dev/null 2>&1; then
        xdg-open "$url" >/dev/null 2>&1
    else
        stop "Steam was not found (no steam, flatpak Steam or xdg-open)." \
             "dinput8.dll is parked: start the game from Steam."
    fi
    gui info "$1"
    sleep 3
    exit 0
}

# Wine names the game's process after its exe; matching whole command lines would also catch
# anything that merely mentions it.
pgrep -ix 'ffxiiiimg\.exe' >/dev/null &&
    stop "FINAL FANTASY XIII is running. Close it, then run this again."

# Only the mod's own dll is parked: another dinput8.dll stops everything here.
if [ -e dinput8.dll ]; then
    grep -qaF "$TAG" dinput8.dll || stop "The dinput8.dll here is not this mod's (another mod?)." \
        "Nothing was renamed, and the game was not started."
    [ -e dinput8.dll.off ] && stop "Both dinput8.dll and dinput8.dll.off are here. Nothing was renamed." \
        "Keep only this mod's dll (as dinput8.dll), then run this again."
    mv -n dinput8.dll dinput8.dll.off && [ ! -e dinput8.dll ] ||
        stop "Could not rename dinput8.dll to dinput8.dll.off in this folder."
fi
launch "Practice tools: OFF  (no dinput8.dll in the game folder)"
