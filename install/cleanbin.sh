#!/bin/bash
#
# uninstall script for PiDP-1
#
#PATH=/usr/sbin:/usr/bin:/sbin:/bin

echo
echo
echo PiDP-1 clean /usr/local/bin
echo ======================
echo
echo The script will remove any installed commands in /usr/local/bin to be sure
echo old binaries are not mixed with new ones.
echo
echo This will execute various commands via sudo, you might be promped for your password.
echo

cd /opt

sudo rm -f /usr/local/bin/pdp1 2>&1 >/dev/null
sudo rm -f /usr/local/bin/pdp1control 2>&1 >/dev/null
sudo rm -f /usr/local/bin/encode_fiodec 2>&1 >/dev/null
sudo rm -f /usr/local/bin/decode_fiodec 2>&1 >/dev/null
sudo rm -f /usr/local/bin/tape_visualizer 2>&1 >/dev/null
sudo rm -f /usr/local/bin/macro1_1 2>&1 >/dev/null
sudo rm -f /usr/local/bin/macro1 2>&1 >/dev/null
sudo rm -f /usr/local/bin/disassemble 2>&1 >/dev/null
sudo rm -f /usr/local/bin/tkaskopenfile 2>&1 >/dev/null
sudo rm -f /usr/local/bin/tkaskopenfilewrite 2>&1 >/dev/null
sudo rm -f /usr/local/bin/pdp1audio 2>&1 >/dev/null
sudo rm -f /usr/local/bin/am1 2>&1 >/dev/null
sudo rm -f /usr/local/bin/fastload 2>&1 >/dev/null
sudo rm -f /usr/local/bin/ad1 2>&1 >/dev/null
sudo rm -f /usr/local/bin/drumupdater 2>&1 >/dev/null
sudo rm -f /usr/local/bin/drumlist 2>&1 >/dev/null
sudo rm -f /usr/local/bin/t30dpy 2>&1 >/dev/null

echo Done.
