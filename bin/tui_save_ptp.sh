#!/bin/bash
#
# Save the paper tape punch's tape to a file named on the terminal: a bare name goes in tapes/,
# a path is used as given. save_ptp.sh does the saving.
#
# 06-Oct-26 wje (Claude) save through save_ptp.sh, which finds who holds the tape; a name
#    already in tapes/ was sent as tapes/YYYYYYY; ask before overwriting

root="${PIDP1_ROOT:-/opt/pidp1-mods}"
DEFAULT_DIR="$root/tapes"
echo "Default directory is: $DEFAULT_DIR"

# Prompt the user for a filename (can be just a name or full path)
read -r -p "Enter filename (just name or full path): " INPUT
if [ -z "$INPUT" ]; then
    echo "No name given, nothing saved."
    exit 1
fi

case "$INPUT" in
    */*) FILE="${INPUT/#\~/$HOME}" ;;
    *)   FILE="$DEFAULT_DIR/$INPUT" ;;
esac

if [ -e "$FILE" ]; then
    read -r -p "$FILE exists. Overwrite it? (y/n) " ANSWER
    case "$ANSWER" in
        y|Y|yes) ;;
        *)  echo "Not saved."
            exit 1 ;;
    esac
fi

exec "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/save_ptp.sh" "$FILE"
