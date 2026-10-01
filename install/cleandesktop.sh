#!/bin/bash
#
# uninstall script for desktop icons
#

usr=$(whoami)

rm /home/$usr/Desktop/tty.desktop
rm /home/$usr/Desktop/pdp1control.desktop
rm /home/$usr/Desktop/type30.desktop
rm /home/$usr/Desktop/t30dpy.desktop
rm /home/$usr/Desktop/ptr.desktop
rm /home/$usr/Desktop/ptp.desktop
rm /home/$usr/Desktop/audioOn.desktop
rm /home/$usr/Desktop/audioOff.desktop 
rm /home/$usr/Desktop/pdp1central.desktop

