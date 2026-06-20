#!/bin/bash

BASE_DIR="/opt/pidp1-mods"

# Launch zenity from the base dir
ABS_FILE=$(zenity --file-selection --title="Choose a file" --filename="${BASE_DIR}/tapes/")

if [ $? -ne 0 ]; then
    echo "Cancelled."
    exit 1
fi

# Send the absolute path, not a path relative to BASE_DIR: whichever backend is listening
# on port 1050 (the C pdp1_periphES daemon, or pdpsrv.go under the "web" interface) opens
# this path as-is, relative to its OWN current working directory, which is not guaranteed
# to be BASE_DIR (e.g. pdp1control.sh cd's into web_pdp1/ before launching pdpsrv.go). A
# relative path here silently fails to load on the far end with no visible error, since
# pdp1control.sh redirects that backend's stderr to /dev/null. tui_load_ptr.sh already
# uses the absolute path for the same reason.
echo "Sending path: $ABS_FILE"

echo "r $ABS_FILE" | ncat -w 1 localhost 1050
