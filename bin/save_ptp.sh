#!/bin/bash

BASE_DIR="/opt/pidp1"
DEFAULT_NAME="newfile"

# Launch zenity save dialog, starting in /opt/pidp1
ABS_FILE=$(zenity --file-selection --save --confirm-overwrite \
                  --title="Save As" \
                  --filename="${BASE_DIR}/tapes/${DEFAULT_NAME}")

# Handle cancel
if [ $? -ne 0 ]; then
    echo "User cancelled."
    exit 1
fi

# Convert absolute path to relative (from BASE_DIR)
REL_FILE="${ABS_FILE#$BASE_DIR/}"

echo "Relative path: $REL_FILE"
    echo "p $REL_FILE" | ncat -w 1 localhost 1050

