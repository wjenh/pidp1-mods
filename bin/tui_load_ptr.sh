#!/bin/bash

# Directory to list files from
SUBDIR="/opt/pidp1/tapes"

# Check if the directory exists
if [[ ! -d "$SUBDIR" ]]; then
    echo "Directory '$SUBDIR' not found!"
    exit 1
fi

# Get list of files (non-hidden) in the subdirectory
FILES=("$SUBDIR"/*.rim "$SUBDIR"/*.bin)

# Check if any files found
if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "No files found in $SUBDIR."
    exit 1
fi

echo "Choose a file from '$SUBDIR':"

# Use Bash's built-in menu
select FILE in "${FILES[@]}"; do
    if [[ -n "$FILE" ]]; then
        #echo "You selected: $FILE"
        # Do something with the file, like open it or pass to telnet or cat
        # Example: cat "$FILE"
        break
    else
        echo "Invalid selection. Try again."
    fi
done
echo "r $FILE" 
echo "r $FILE" | ncat -w 1 localhost 1050

