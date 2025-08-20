#!/bin/bash

# start script for pidp1.  

# Interface setting - can be 'gui', 'web', or 'apps'
interface="web"

# Front panel setting - can be 'pidp' or 'virtual'
frontpanel="pidp"

argc=$#
pidp1="/opt/pidp1/bin/pdp1"
cd /opt/pidp1

# Requires screen utility for detached pidp1 console functionality.
#
test -x /usr/bin/screen || ( echo "screen not found" && exit 0 )
test -x $pidp1 || ( echo "$pidp1 not found" && exit 0 )

# Check if pidp1 is already runnning under screen.
#
is_running() {
	procs=`screen -ls pidp1 | egrep '[0-9]+\.pidp1' | wc -l`
	return $procs
}

do_stat() {
	is_running
	status=$?
	if [ $status -gt 0 ]; then
	    echo "PiDP-1 is up." >&2
	    return $status
	else
	    echo "PiDP-1 is down." >&2
	    return $status
	fi
}

 

do_start() {
	is_running
	if [ $? -gt 0 ]; then
	    echo "PiDP-1 is already running, not starting again." >&2
	    exit 0
	fi
	
	echo start panel driver, either virtual or real, depends on the symlink
	if [ "$frontpanel" = "pidp" ]; then
		echo starting PiDP-1 hardware front panel driver
	        nohup bin/panel_pidp1 &
	elif [ "$frontpanel" = "virtual" ]; then
		echo starting on-screen virtual front panel, not PiDP-1 hardware
		echo use
		echo    pdp1control panel pidp
		echo to change that, if you have a PiDP-1!
	        nohup bin/vpanel_pdp1 &
	else
                echo ERROR: NO VALID FRONT PANEL DRIVER, fix with
		echo    pdp1control panel
	fi


	sleep 1 # only needed for autoboot at startup

	echo read boot config from sense switches
	bin/scanpf
	sw=$?

	sw="${2:-$sw}"
	echo switches set to $sw

	echo start pidp1 in screen
	screen -dmS pidp1 bin/pdp1
	status=$?

	if [ "$interface" = "gui" ]; then
		echo start gui peripherals
		sleep 2
		nohup bin/pdp1_periphES &
	elif [ "$interface" = "web" ]; then
		echo start web server
		cd /opt/pidp1/web_pdp1
		nohup go run /opt/pidp1/web_pdp1/pdpsrv.go &
		cd /opt/pidp1
	elif [ "$interface" = "apps" ]; then
		echo start apps
		sleep 1
		echo start p7simES
		nohup bin/p7simES localhost &
		sleep 1
		echo start tapevis
		nohup bin/tapevis &
	fi

	sleep 1 # 0.3
	echo "Configuring PiDP-1 for boot number $sw"
	cat "bootcfg/${sw}.cfg" | bin/pdp1ctl

	return $status
}

do_stop() {
	#kill any support programs that may be running
	pkill tapevis
    	pkill p7simES
	pkill pdp1_periphES
        pkill pdpsrv
    	pkill panel_pidp1
    	pkill vpanel_pidp1

	sleep 0.5
	is_running
	if [ $? -eq 0 ]; then
	    echo "PiDP-1 is already stopped." >&2
	    status=1
	else
	    echo "Stopping PiDP-1"
	    #screen -S pidp1 -X quit
	    pkill 'pdp1$'
	    status=$?
	fi

	sleep 0.5
        # pdp1 may be running outside of screen
	pkill pdp1

	return $status
}


do_set() {
	if [ -z "$2" ]; then
		echo "Error: No interface specified. Use 'gui', 'web', or 'apps'." >&2
		return 1
	fi
	
	case "$2" in
		gui|web|apps)
			# Use specific temporary file path
			temp_file="/opt/pidp1/bin/pdp1control.tmp"
			
			# Use sed to replace the interface variable assignment
			sed "s/^interface=\"[^\"]*\"/interface=\"$2\"/" "$0" > "$temp_file"
			
			# Replace the original script with the modified version
			if cp "$temp_file" "$0" && rm -f "$temp_file"; then
				chmod +x "$0"
				echo "Interface set to '$2'"
				exit 0
			else
				echo "Error: Failed to update script" >&2
				rm -f "$temp_file"
				return 1
			fi
			;;
		*)
			echo "Error: Invalid interface '$2'. Use 'gui', 'web', or 'apps'." >&2
			return 1
			;;
	esac
}

do_panel() {
	if [ -z "$2" ]; then
		echo "Error: No panel option specified. Use 'pidp' or 'virtual'." >&2
		return 1
	fi
	
	case "$2" in
		pidp|virtual)
			# Use specific temporary file path
			temp_file="/opt/pidp1/bin/pdp1control.tmp"
			
			# Use sed to replace the frontpanel variable assignment
			sed "s/^frontpanel=\"[^\"]*\"/frontpanel=\"$2\"/" "$0" > "$temp_file"
			
			# Replace the original script with the modified version
			if cp "$temp_file" "$0" && rm -f "$temp_file"; then
				chmod +x "$0"
				echo "Panel set to '$2'"
				exit 0
			else
				echo "Error: Failed to update script" >&2
				rm -f "$temp_file"
				return 1
			fi
			;;
		*)
			echo "Error: Invalid panel option '$2'. Use 'pidp' or 'virtual'." >&2
			return 1
			;;
	esac
}

case "$1" in
  start)
	do_start $1 $2
	;;

  stop)
	do_stop
	;;

  restart)
	do_stop
	sleep 4
	do_start $1 $2
	;;
  set)
	do_set $1 $2
	;;
  panel)
	do_panel $1 $2
	;;

  status)
	screen -ls pidp1 | egrep '[0-9]+\.pidp1'
	;;

  stat)
	do_stat
	;;
  ?)
	echo "Usage: pdp1control {start|stop|restart|set|panel|status|stat}" || true
	exit 1
	;;
  *)
	do_stat
	if [ $status = 0 ]; then
		read -p "(S)tart, Start with boot (number), or (C)ancel? " respx
		#read -p "(S)tart or (C)ancel? " respx
		case $respx in
			[Ss]* )
				do_start
				;;
			[0-9]* )
				#boot_number=$respx
				# convert to decimal
				#boot_number=$((8#$ooot_number))
				set -- "start" "$respx"
				echo reassigned s2 to .$2. and s1 is .$1.
				do_start $1 $2
				;;
			[Cc]* )
				exit 1
				;;
			* )
				echo "Please answer with S or C.";;
				#echo "Please answer with S, a boot number, or C.";;
		esac
	else
		read -p "(S)top or (C)ancel? " respx
		case $respx in
			[Ss]* )
				do_stop
				;;
			[Cc]* )
				exit 1
				;;
			* )
				echo "Please answer with S or C.";;
		esac
	fi
esac
exit 0

