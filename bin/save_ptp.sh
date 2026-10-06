#!/bin/bash
#
# Save the paper tape punch's tape to a file: what was punched since the last save, or since
# the emulator started.
#   save_ptp.sh         asks for the file in a dialog, and reports in one
#   save_ptp.sh FILE    saves to FILE, and reports on stdout, a failure on stderr
# Exits 0 if the tape was saved, else 1.
#
# Whoever holds the punch has the tape:
# - no front end: the emulator punches into punch.out in its own directory. That file is
#   saved, and the emulator is given a fresh punch.out;
# - pdp1_periphES or tapevis: it keeps the tape, and is sent "p FILE" on port 1050;
# - pdpsrv, with a browser open: the tape is in the browser, whose punch panel saves it;
# - a file chosen with pdp1central's Save punch to..., or the console's p command, already is
#   the tape.
#
# 06-Oct-26 wje (Claude) save the emulator's punch.out when no front end holds the punch, as
#    under apps, which starts none; absolute paths; a FILE argument; say what happened

root="${PIDP1_ROOT:-/opt/pidp1-mods}"
default_name="new_tape"
dialog=0

# Report a result, in a dialog too when the file was chosen in one: the terminal the desktop
# icon opens closes as soon as this script ends.
say() {
    echo "$1"
    if [ $dialog -eq 1 ]; then
        zenity --info --no-markup --title="Save Paper Tape Punch" --text="$1" 2>/dev/null
    fi
}

fail() {
    echo "$1" >&2
    if [ $dialog -eq 1 ]; then
        zenity --error --no-markup --title="Save Paper Tape Punch" --text="$1" 2>/dev/null
    fi
    exit 1
}

# Copy the punch file $1 to $file. Fails if nothing has been punched, or if $file is the punch
# file itself.
copy_tape() {
    if [ "$(readlink -f "$file")" = "$(readlink -f "$1")" ]; then
        fail "$file is the punch file itself; choose another name."
    fi

    if [ ! -s "$1" ]; then
        fail "Nothing has been punched since the last save; $file was not written."
    fi

    cp "$1" "$file" || fail "Could not write $file."
}

# The emulator punches into $1: rename it, which the emulator goes on writing, give the
# emulator a fresh one, then save the renamed file. Nothing punched meanwhile is lost.
save_from_emulator() {
    local pf="$1"
    local old="$1.saving"
    local reply

    if [ "$(readlink -f "$file")" = "$(readlink -f "$pf")" ]; then
        fail "$file is the punch file itself; choose another name."
    fi

    if [ ! -s "$pf" ]; then
        fail "Nothing has been punched since the last save; $file was not written."
    fi

    mv -f "$pf" "$old" || fail "Could not rename $pf; nothing was saved."
    reply=$(echo "p $pf" | ncat -w 1 localhost 1040 2>&1)
    if [ "$reply" != "ok" ]; then
        mv -f "$old" "$pf"
        fail "The emulator did not take a fresh punch file (${reply:-no reply}); nothing was saved."
    fi

    # The emulator adopts the new file at its next pass, and writes the old one until then.
    sleep 0.2
    copy_tape "$old"
    rm -f "$old"
    # say "Saved $(stat -c %s "$file") bytes of punched tape to $file."
}

if [ $# -ge 1 ]; then
    file="$1"
else
    dialog=1

    # Launch zenity save dialog, starting in /opt/pidp1-mods
    file=$(zenity --file-selection --save --confirm-overwrite \
                  --title="Save As" \
                  --filename="${root}/tapes/${default_name}")

    # Handle cancel
    if [ $? -ne 0 ]; then
        echo "User cancelled."
        exit 1
    fi
fi

# A front end opens a relative path from its own directory, which need not be ours.
case "$file" in
    /*) ;;
    *)  file="$PWD/$file" ;;
esac

pid=$(pgrep -x pdp1 | head -n 1)
if [ -z "$pid" ]; then
    copy_tape "$root/punch.out"
    # say "The emulator is not running: saved $(stat -c %s "$file") bytes from its last run's punch.out to $file."
    exit 0
fi

frontend=""
for name in pdp1_periphES tapevis pdpsrv; do
    if pgrep -x "$name" > /dev/null; then
        frontend="$name"
        break
    fi
done

dir=$(readlink "/proc/$pid/cwd" 2>/dev/null)
dir="${dir:-$root}"

# A front end's socket replaces punch.out, which the emulator then closes, so an open punch.out
# means no front end holds the punch, whatever runs. Another user's emulator cannot be looked
# at: then whatever front end runs is taken to hold it.
held=unknown
if ls "/proc/$pid/fd" > /dev/null 2>&1; then
    held=no
    for fd in /proc/$pid/fd/*; do
        if [ "$(readlink "$fd" 2>/dev/null)" = "$dir/punch.out" ]; then
            held=yes
            break
        fi
    done
fi

if [ "$held" = "yes" ] || { [ "$held" = "unknown" ] && [ "$frontend" != "pdp1_periphES" ] && [ "$frontend" != "tapevis" ]; }; then
    save_from_emulator "$dir/punch.out"
    exit 0
fi

case "$frontend" in
    pdp1_periphES|tapevis)
        # Its command port splits a line at spaces.
        case "$file" in
            *[[:space:]]*)
                fail "$frontend cannot be sent a name with a space in it; choose another." ;;
        esac

        before=$(stat -c %y "$file" 2>/dev/null)
        echo "p $file" | ncat -w 1 localhost 1050 > /dev/null 2>&1 ||
            fail "$frontend did not answer on port 1050; nothing was saved."

        # It never replies; the file it writes is the answer.
        for i in $(seq 20); do
            after=$(stat -c %y "$file" 2>/dev/null)
            [ -n "$after" ] && [ "$after" != "$before" ] && break
            sleep 0.1
        done
        [ -n "$after" ] && [ "$after" != "$before" ] || fail "$frontend did not write $file."
        # say "$frontend saved $(stat -c %s "$file") bytes of punched tape to $file."
        ;;
    pdpsrv)
        fail "The punch is connected to the web interface: save the tape with the Save button on the browser's Paper Tape Punch panel. $file was not written."
        ;;
    *)
        fail "The emulator is punching into a file chosen with pdp1central's Save punch to..., or the console's p command; that file already is the tape. $file was not written."
        ;;
esac
