#!/bin/bash

DEFAULT_DIR="/opt/pidp1/tapes"
echo "Default directory is: $DEFAULT_DIR"

# Prompt the user for a filename (can be just a name or full path)
read -p "Enter filename (just name or full path): " INPUT

BASENAME="${INPUT##*/}"
DEFAULT_PATH="$DEFAULT_DIR/$BASENAME"

if [[ -f "$DEFAULT_PATH" ]]; then
    FILE="$BASENAME"
else
    FILE="$INPUT"
fi

echo "Saving as $FILE"

echo "p $FILE" | ncat -w 1 localhost 1050


