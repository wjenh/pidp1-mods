#!/bin/bash
#
#
# install script for PiDP-1
#
#PATH=/usr/sbin:/usr/bin:/sbin:/bin

INSTALLDIR=/opt/pidp1-mods

# check this script is NOT run as root
if [ "$(whoami)" = "root" ]; then
    echo script must NOT be run as root
    exit 1
fi

if [ ! -d "${INSTALLDIR}" ]; then
    echo clone git repo or copy distribution into /opt
    exit 1
fi

echo
echo PiDP-1 install script
echo ======================
echo
echo The script can be re-run at any time to change things. Re-running the install
echo script and answering \'n\' to questions will leave those things unchanged.
echo You can recompile from source, but it is easier to just install the precompiled
echo binaries. 
echo
echo Too Long, Didn\'t Read?
echo Just say Yes to everything.
echo

usr=$(whoami)
usrgroup=$(id -g -n)
cd $INSTALLDIR

# give pidp1 user owner after the sudo git clone command
# =============================================================================

while true; do
    echo
    read -p "Set owner of PiDP-1 directory? " yn
    case $yn in
        [Yy]* )
            # make sure that the directory does not have root ownership
            sudo chown -R $usr:$usrgroup $INSTALLDIR
	    break
	    ;;
        [Nn]* ) 
            echo Left ownership of PiDP directory unchanged
	    break
            ;;
        * ) echo "Please answer yes or no.";;
    esac
done


# Install required dependencies
# =============================================================================
while true; do
    echo
    read -p "Install the required dependencies? " prxn
    case $prxn in
        [Yy]* ) 
            sudo apt update
            #Install SDL2, optionally used for PDP-11 graphics terminal emulation
            sudo apt install -y libsdl2-dev
	    sudo apt install -y libsdl2-ttf-dev
            #Install SDL2-image. Strictly speaking, only needed for virtual panel, not PiDP-1
	    sudo apt install -y libsdl2-image-dev
            if apt-cache show libsdl3-dev >/dev/null 2>&1; then
                echo "SDL3 is available, installing."
                sudo apt install -y libsdl3-dev
            fi
            #Install ncat
            sudo apt install -y ncat
            #Install readline, used for command-line editing in simh
            #sudo apt install -y libreadline-dev
            # Install screen
            sudo apt install -y screen

	    # not needed for Pi OS but for generic Linux
	    sudo apt install lxterminal

	    # telnet is no longer part of standard linux...
	    sudo apt install telnet

            # needed to build am1
	    sudo apt -y install flex
	    sudo apt -y install bison

	    break
	    ;;
        [Nn]* ) 
            echo Skipped install of dependencies - OK if you installed them already
            break
	    ;;
        * ) echo "Please answer Y or N.";;
    esac
done

while true; do
    echo
    read -p "Install required dependencies for the web server? " prxn
    case $prxn in
        [Yy]* ) 
            sudo apt update
            sudo apt install golang
	    break
	    ;;
        [Nn]* ) 
            echo Skipped install of web server dependencies
            break
	    ;;
        * ) echo "Please answer Y or N.";;
    esac
done

if [ -f pidp1.config ] &&  [ pidp1.config.example -nt pidp1.config ]; then
    echo The pidp1.config.example file might have new settings, review it to see.
fi

while ! test -f pidp1.config; do
    echo
    read -p "Create pidp1.config from pidp1.config.example? " yn
    case $yn in
        [Yy]* )
            cp pidp1.config.example pidp1.config
	    break
	    ;;
        [Nn]* ) 
	    break
            ;;
        * ) echo "Please answer yes or no.";;
    esac
done

if [ -f pdp23drum ] &&  [ pdp23drum.example -nt pdp23drum ]; then
    echo The distributed Type 23 drum image is newer than your drum image.
    echo If you want to update it, do cp pdp23drum.example pdp23drum
fi

while ! test -f pdp23drum; do
    echo
    read -p "Create drum image from pdp23drum.example? " yn
    case $yn in
        [Yy]* )
            cp pdp23drum.example pdp23drum
            chmod a+rw pdp23drum
	    break
	    ;;
        [Nn]* ) 
	    break
            ;;
        * ) echo "Please answer yes or no.";;
    esac
done

# make required binaries
# =============================================================================

while true; do
    echo
    read -p "Make required PiDP-1 binaries? " yn
    case $yn in
        [Yy]* )
		make -C $INSTALLDIR/src/blincolnlights/pinctrl 	# pinctrl functions
		make -C $INSTALLDIR/src/blincolnlights/panel_pidp1 all 	# panel driver
		make -C $INSTALLDIR/src/blincolnlights/pdp1 	# simulator
		make -C $INSTALLDIR/src/p7sim			# returns sense switches
		make -C $INSTALLDIR/src/scanpf 			# returns sense switches
		make -C $INSTALLDIR/src/blincolnlights/tapevis	# returns sense switches
		make -C $INSTALLDIR/src/pidp1_test 		# hardware test program
		make -C $INSTALLDIR/src/pdp1_periph		# unified peripherals
		make all -C $INSTALLDIR/IOTs			# dynamic IOTs
		
		# this makes the virtual pdp-1 panel, used if no PiDP-1 hardware is attached:
		make -C $INSTALLDIR/src/blincolnlights/vpanel_pdp1 	# panel driver

                # The am1 macro assembler
		make -C $INSTALLDIR/Tools/AM1 install
		make -C $INSTALLDIR/Tools/AM1 clean
                # The ad1 symbolic debugger
		make -C $INSTALLDIR/Tools/AD1 install
		make -C $INSTALLDIR/Tools/AD1 clean
                # The drum tools
		make -C $INSTALLDIR/Tools/Drumupdater install
		make -C $INSTALLDIR/Tools/Drumupdater clean
                # The new Type 30 display, either the SDL2 or SDL3 version
                if dpkg-query -W -f='${Status}' libsdl3-dev 2>/dev/null | grep -q "ok installed"; then
                    echo "SDL3 found, installing type30dpy3 as type30dpy."
                    make -C $INSTALLDIR/Tools/T30dpy installdpy3
                else
                    echo "No SDL3 library is present, installing SDL2 type30dpy."
                    make -C $INSTALLDIR/Tools/T30dpy install
                fi
                
		make -C $INSTALLDIR/Tools/T30dpy clean

		# the macro1_1 cross-compiler:
		gcc $INSTALLDIR/src/macro/macro1_1.c -o $INSTALLDIR/src/macro/macro1_1
		# the usb_paper_tape tool:
		make -C $INSTALLDIR/src/usb_paper_tape
		# The disassembler been renamed, get rid of the old one
		test -f $INSTALLDIR/bin/disassemble_tape && rm $INSTALLDIR/bin/disassemble_tape
		test -f $INSTALLDIR/bin/disassemble_mem && rm $INSTALLDIR/bin/disassemble_mem
		test -f /usr/local/bin/disassemble_tape && rm /usr/local/bin/disassemble_tape
		make -C $INSTALLDIR/Tools install
            
		echo Setting required access privileges to pidp1 simulator
		# make sure pidp1 panel driver has the right privileges
            	# to access GPIO with root privileges:
            	sudo chmod +s $INSTALLDIR/src/blincolnlights/panel_pidp1
            	# to run as a RT thread:
            	sudo setcap cap_sys_nice+ep $INSTALLDIR/src/blincolnlights/panel_pidp1/panel_pidp1
            	sudo setcap cap_sys_nice+ep $INSTALLDIR/src/blincolnlights/panel_pidp1/newpanel

                ln -sf $INSTALLDIR/src/macro/macro1_1 $INSTALLDIR/bin/macro1_1
                ln -sf $INSTALLDIR/src/blincolnlights/tools/mkptyfio_telnet $INSTALLDIR/bin/mkptyfio_telnet
                ln -sf $INSTALLDIR/src/p7sim/p7sim $INSTALLDIR/bin/p7sim
                ln -sf $INSTALLDIR/src/p7sim/p7simES $INSTALLDIR/bin/p7simES
                ln -sf $INSTALLDIR/src/blincolnlights/vpanel_pdp1/panel_pdp1 $INSTALLDIR/bin/vpanel_pdp1
                ln -sf $INSTALLDIR/src/blincolnlights/pdp1/pdp1 $INSTALLDIR/bin/pdp1
                ln -sf $INSTALLDIR/src/pdp1_periph/pdp1_periphES $INSTALLDIR/bin/pdp1_periphES
                ln -sf $INSTALLDIR/src/usb_paper_tape/pdp1_usb_monitor $INSTALLDIR/bin/pdp1_usb_monitor
                ln -sf $INSTALLDIR/src/pidp1_test/pidp1_test $INSTALLDIR/bin/pidp1_test
                ln -sf $INSTALLDIR/src/scanpf/scanpf $INSTALLDIR/bin/scanpf
                ln -sf $INSTALLDIR/src/blincolnlights/tapevis/tapevis $INSTALLDIR/bin/tapevis
                while true; do
                    echo
                    read -p "Install the new PiDP hardware front panel (Y) or the old panel (N)? " yn
                    case $yn in
                        [Yy]* )
                            echo Installed the new PiDP hardware front panel
                            ln -sf $INSTALLDIR/src/blincolnlights/panel_pidp1/newpanel $INSTALLDIR/bin/panel_pidp1
                            break
                            ;;
                        [Nn]* ) 
                            echo Installed the old PiDP hardware front panel
                            ln -sf $INSTALLDIR/src/blincolnlights/panel_pidp1/panel_pidp1 $INSTALLDIR/bin/panel_pidp1
                            break
                            ;;
                        * ) echo "Please answer yes or no.";;
                    esac
                done
	    	echo Done.
		break
		;;
        [Nn]* ) 
            echo Did not compile PiDP-1 programs.
	    break
            ;;
        * ) echo "Please answer yes or no.";;
    esac
done

# Install pidp1 commands
# =============================================================================
while true; do
    echo
    read -p "Install PiDP-1 commands into OS? " prxn
    case $prxn in
        [Yy]* ) 
            # First, clean /usr/local/bin to be sure no leftover original programs
            sudo $INSTALLDIR/install/cleanbin.sh
            # put pdp1 command into /usr/local
            sudo ln -f -s $INSTALLDIR/bin/pdp1.sh /usr/local/bin/pdp1
            # put pdp1control script into /usr/local
            sudo ln -f -s $INSTALLDIR/bin/pdp1control.sh /usr/local/bin/pdp1control
	    #
	    #
	    sudo ln -sf $INSTALLDIR/bin/encode_fiodec /usr/local/bin/encode_fiodec
            sudo ln -sf $INSTALLDIR/bin/decode_fiodec /usr/local/bin/decode_fiodec
	    sudo ln -sf $INSTALLDIR/bin/tape_visualizer /usr/local/bin/tape_visualizer
	    #
	    sudo ln -sf $INSTALLDIR/bin/macro1_1 /usr/local/bin/macro1_1
	    sudo ln -sf $INSTALLDIR/bin/macro1_1 /usr/local/bin/macro1
	    sudo ln -sf $INSTALLDIR/bin/disassemble /usr/local/bin/disassemble
	    #
	    sudo ln -sf $INSTALLDIR/bin/am1 /usr/local/bin/am1
	    sudo ln -sf $INSTALLDIR/bin/ad1 /usr/local/bin/ad1
	    sudo ln -sf $INSTALLDIR/bin/drumupdater /usr/local/bin/drumupdater
	    sudo ln -sf $INSTALLDIR/bin/drumlist /usr/local/bin/drumlist
	    sudo ln -sf $INSTALLDIR/bin/t30dpy /usr/local/bin/t30dpy
            #
	    sudo ln -sf $INSTALLDIR/bin/tkaskopenfile /usr/local/bin/tkaskopenfile
	    sudo ln -sf $INSTALLDIR/bin/tkaskopenfilewrite /usr/local/bin/tkaskopenfilewrite
        #
	    sudo ln -sf $INSTALLDIR/bin/pdp1audio /usr/local/bin/pdp1audio

	    break
	    ;;
        [Nn]* ) 
            echo Skipped software install
            break
	    ;;
        * ) echo "Please answer Y or N.";;
    esac
done


# Use virtual panel or PiDP hardware panel
# =============================================================================

while true; do
    echo
    read -p "Use PiDP hardware front panel (Y) or on-screen virtual panel (V)? " yv
    case $yv in
        [Yy]* )
	    echo Activated PiDP hardware front panel
	    $INSTALLDIR/bin/pdp1control.sh panel pidp
	    break
            ;;
        [Vv]* ) 
            echo Activated virtual front panel - PiDP hardware deactivated
	    $INSTALLDIR/bin/pdp1control.sh panel virtual
	    break
            ;;
        * ) echo "Please answer yes or no.";;
    esac
done
echo
echo "NOTE - change this choice anytime through the 'pdp1control panel' command"


# Use GUI or Web user interface
# =============================================================================

while true; do
    echo
    read -p "Use the Pi's GUI (Y), the Web (W) or the Apps (A)? " ywa
    case $ywa in
        [Yy]* )
	    echo Activated GUI user interface
	    $INSTALLDIR/bin/pdp1control.sh set gui
	    break
            ;;
        [Ww]* ) 
            echo Activated Web interface
	    $INSTALLDIR/bin/pdp1control.sh set web
	    break
            ;;
        [Aa]* ) 
            echo Activated Apps interface
	    $INSTALLDIR/bin/pdp1control.sh set apps
	    break
            ;;
        * ) echo "Please answer Y, W, or A.";;
    esac
done
echo
echo "NOTE - change this choice anytime through the 'pdp1control set' command"


# Use USB paper tape feature?
# =============================================================================

while true; do
    echo
    read -p "Use USB sticks as paper tapes (Y/N)? " yn
    case $yn in
        [Yy]* )
	    echo Activated USB paper tape option
	    $INSTALLDIR/bin/pdp1control.sh usbtape y


	    # Disable annoying popup when USB stick is inserted:
	    CONFU="$HOME/.config/pcmanfm/default/pcmanfm.conf"
	    # disable removable-media popup
	    mkdir -p "$(dirname "$CONFU")"
	    if grep -q '^autorun=' "$CONFU"; then
	        sed -i 's/^autorun=.*/autorun=0/' "$CONFU"
	    else
	        echo 'autorun=0' >> "$CONFU"
	    fi


	    break
            ;;
        [Nn]* ) 
            echo USB paper tape option NOT activated
	    $INSTALLDIR/bin/pdp1control.sh usbtape n
	    break
            ;;
        * ) echo "Please answer Y or N.";;
    esac
done
echo
echo "NOTE - change this choice anytime through the 'pdp1control usbtape' command"


# Install autostart at boot
# =============================================================================
if [ "$ARCH" = "amd64" ]; then
	echo skipping autostart, because this is not a Raspberry Pi
	echo start manually by typing 
	echo pdp1control start x
	echo ...where x is the tape number normally set on the front panel.
	echo 
	echo "But that is all in the manual..."
	echo
else
	while true; do
	    echo
	    echo 
		read -p "Autostart the PDP-1 using the GUI(Y), or using .profile for (H)eadless Pis, or (N)ot at all?" yhn
		case $yhn in
		      [Yy]* ) 
			mkdir -p ~/.config/autostart
			cp $INSTALLDIR/install/pdp1startup.desktop ~/.config/autostart
			echo
			echo Autostart via .desktop file for GUI setup
			break
			;;
		      [Hh]* ) 
			# add pdp11 to the end of pi's .profile to let a new login 
			# grab the terminal automatically
			#   first, make backup .foo copy...
			test ! -f /home/$usr/profile.foo && cp -p /home/$usr/.profile /home/$usr/profile.foo
			#   add the line to .profile if not there yet
			if grep -xq "pdp11 # autostart" /home/$usr/.profile
			then
			    echo .profile already contains pdp11 for autostart, OK.
			else
			    sed -e "\$apdp1control start # autostart" -i /home/$usr/.profile
			fi
			echo
			echo autostart via .profile for headless use without GUI
			break
			;;
		      [Nn]* ) 
			echo No autostart
			break
			;;
		      * ) echo "Please answer Y, H or N.";;
	    esac
	done	
fi

# 20241126 Add desktop icons etc
# =============================================================================
while true; do
    echo
    read -p "Add desktop icons and desktop settings? " prxn
    case $prxn in
        [Yy]* ) 
            cp $INSTALLDIR/install/tty.desktop /home/$usr/Desktop/
            cp $INSTALLDIR/install/pdp1control.desktop /home/$usr/Desktop/
            cp $INSTALLDIR/install/type30.desktop /home/$usr/Desktop/
            cp $INSTALLDIR/install/t30dpy.desktop /home/$usr/Desktop/
            cp $INSTALLDIR/install/ptr.desktop /home/$usr/Desktop/
            cp $INSTALLDIR/install/ptp.desktop /home/$usr/Desktop/

            # audio control app for new audio system
            cp $INSTALLDIR/install/audioOn.desktop /home/$usr/Desktop/
            cp $INSTALLDIR/install/audioOff.desktop /home/$usr/Desktop/

            chmod u+x /home/$usr/Desktop/tty.desktop
            chmod u+x /home/$usr/Desktop/pdp1control.desktop
            chmod u+x /home/$usr/Desktop/type30.desktop
            chmod u+x /home/$usr/Desktop/t30dpy.desktop
            chmod u+x /home/$usr/Desktop/ptr.desktop
            chmod u+x /home/$usr/Desktop/ptp.desktop
            chmod u+x /home/$usr/Desktop/audioOn.desktop
            chmod u+x /home/$usr/audioOff.desktop

            #make pcmanf run on double click, change its config file
            config_file="/home/$usr/.config/libfm/libfm.conf"
            # Create the directory if it doesn't exist
            mkdir -p "$(dirname "$config_file")"
            # Add or update the quick_exec setting
            if grep -q "^\s*quick_exec=" "$config_file" 2>/dev/null; then
                echo ...Update existing setting...
                sed -i 's/^\s*quick_exec=.*/quick_exec=1/' "$config_file"
            else
                echo ...Adding the config file, it does not exist yet
                echo -e "[config]\nquick_exec=1" >> "$config_file"
            fi
        
            # wallpaper
            pcmanfm --set-wallpaper $INSTALLDIR/install/wallpaper.png --wallpaper-mode=fit

            #echo
            #echo "Installing Teletype font..."
            #echo
            #mkdir ~/.fonts
            #    cp $INSTALLDIR/install/TTY33MAlc-Book.ttf ~/.fonts/
            #fc-cache -v -f


            echo "Desktop updated."
            break
	    ;;

        [Nn]* ) 
            echo Skipped. You can do it later by re-running this install script.
            break
	    ;;
        * ) echo "Please answer Y or N.";;
    esac
done


echo
echo Done. Please do a sudo reboot and the front panel will come to life.
echo Rerun this script if you want to do any install modifications.
echo You can uninstall everyting including /opt/pidp1-mods by using the uninstall.sh script.
echo The demo prograns are not automatically assembled. Do a make in the Demo or other program directories
echo FunStuff if you want to try them.
