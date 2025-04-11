#!/bin/bash

DEFAULT_DIR="/opt/pidp1/tapes"
echo "Default directory is: $DEFAULT_DIR"

# Prompt the user for a filename (can be just a name or full path)
read -p "Enter filename (just name or full path): " INPUT

BASENAME="${INPUT##*/}"
DEFAULT_PATH="$DEFAULT_DIR/$BASENAME"

if [[ -f "$DEFAULT_PATH" ]]; then
    echo save in tapes
    FILE="$BASENAME"
    echo "p tapes/YYYYYYY" | ncat -w 1 localhost 1050
else
    echo save in custom path
    FILE="${INPUT/#\~/$HOME}"
    echo "p $FILE" | ncat -w 1 localhost 1050
fi

#echo "p $FILE" | ncat -w 1 localhost 1050

