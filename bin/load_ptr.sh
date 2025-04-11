#!/bin/bash

BASE_DIR="/opt/pidp1"

# Launch zenity from the base dir
ABS_FILE=$(zenity --file-selection --title="Choose a file" --filename="${BASE_DIR}/tapes/")

if [ $? -ne 0 ]; then
    echo "Cancelled."
    exit 1
fi

# Get relative path
REL_FILE="${ABS_FILE#$BASE_DIR/}"

echo "Relative path: $REL_FILE"

echo "r $REL_FILE" | ncat -w 1 localhost 1050
