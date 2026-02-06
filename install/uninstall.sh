#!/bin/bash
#
# uninstall script for PiDP-1
#
#PATH=/usr/sbin:/usr/bin:/sbin:/bin

echo
echo
echo PiDP-1 uninstall script
echo ======================
echo
echo The script will remove any installed commands and any desktop and autostart
echo files that were created by the install, then remove the entire /opt/pidp1-mods directory.
echo
echo This will execute various commands via sudo, you might be promped for your password.
echo

cd /opt

sudo rm -f /usr/local/bin/pdp1
sudo rm -f /usr/local/bin/pdp1control
sudo rm -f /usr/local/bin/encode_fiodec
sudo rm -f /usr/local/bin/decode_fiodec
sudo rm -f /usr/local/bin/tape_visualizer
sudo rm -f /usr/local/bin/monas
sudo rm -f /usr/local/bin/macro1_1
sudo rm -f /usr/local/bin/macro1
sudo rm -f /usr/local/bin/disassemble_tape
sudo rm -f /usr/local/bin/tkaskopenfile
sudo rm -f /usr/local/bin/tkaskopenfilewrite
sudo rm -f /usr/local/bin/pdp1audio

rm ~/.config/autostart/pdp1startup.desktop

echo Removing autostart from .profile if it exists
if grep -xq "pdp1 # autostart" $HOME/.profile
then
    echo A copy of your .profile is saved to profile.sav
    cp -p /home/$usr/.profile $HOME/profile.sav
    sed -i '/pdp1control start/d' $HOME/.profile
else
    echo It doesn\'t exist, not changing your profile.
    fi

# Remove the desktop entroes, if any
rm -f $HOME/Desktop/tty.desktop >/dev/null 2>&1
rm -f $HOME/Desktop/pdp1control.desktop >/dev/null 2>&1
rm -f $HOME/Desktop/type30.desktop >/dev/null 2>&1
rm -f $HOME/Desktop/ptr.desktop >/dev/null 2>&1
rm -f $HOME/Desktop/ptp.desktop  >/dev/null 2>&1
rm -f $HOME/Desktop/audioOn.desktop >/dev/null 2>&1
rm -f $HOME/Desktop/audioOff.desktop >/dev/null 2>&1

# wallpaper
echo Attempting to reset your wallpaper.
echo If your desktop background goes away, you will have to reset it via
echo your desktop preference setting.
if [ -d /usr/share/lxde/wallpapers
then
    pcmanfm --set-wallpaper /usr/share/lxde/wallpapers/lxde-blue.jpg --wallpaper-mode=fit
else
    echo Can\'t restore your wallpaper, sorry.
fi

echo Removing /opt/pidp1-mods
rm -rf pidp1-mods

echo Done.
