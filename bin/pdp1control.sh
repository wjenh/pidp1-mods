#!/bin/bash

# start script for pidp1.  
# 7-Apr-26 wje add reload to have the config file reloaded
# 29-Apr-26 wje change reload to use sighup
# 30-Sep-26 wje (Claude) keep the interface, panel and usbtape choices in pdp1control.config
#    instead of rewriting this script; PIDP1_ROOT; stop, reload and reloadpanel act on exact
#    process names; a missing screen or pdp1 stops the script; apps starts t30dpy only, and
#    stop lets t30dpy end on its own when pdp1 goes

# The install directory; PIDP1_ROOT points it elsewhere, for testing.
root="${PIDP1_ROOT:-/opt/pidp1-mods}"

# The start-time choices are kept in $root/pdp1control.config, as name=value lines:
#   interface=web       gui, web or apps
#   frontpanel=virtual  pidp or virtual
#   usbtape=n           y to use the USB ports for the paper tape reader and punch
# A missing file or line keeps the default below. The set, panel and usbtape commands write it.
settings="$root/pdp1control.config"
interface="web"
frontpanel="virtual"
usb_paper_tape="n"

argc=$#
pidp1="$root/bin/pdp1"
cd "$root"

# Requires screen utility for detached pidp1 console functionality.
#
if ! test -x /usr/bin/screen; then
	echo "screen not found"
	exit 1
fi
if ! test -x "$pidp1"; then
	echo "$pidp1 not found"
	exit 1
fi

# Read the start-time choices. The file is read line by line, never sourced, so it cannot run
# code. '#' starts a comment; an unknown name or an invalid value keeps the default.
read_settings() {
	test -r "$settings" || return 0
	while IFS= read -r line || [ -n "$line" ]; do
		line="${line%$'\r'}"
		case "$line" in
		''|'#'*) continue ;;
		esac
		name="${line%%=*}"
		value="${line#*=}"
		if [ "$name" = "$line" ]; then
			echo "pdp1control.config: '$line' is not name=value, ignored" >&2
			continue
		fi
		name="${name%"${name##*[![:space:]]}"}"
		value="${value#"${value%%[![:space:]]*}"}"
		value="${value%%[[:space:]]*}"
		case "$name=$value" in
		interface=gui|interface=web|interface=apps)
			interface="$value" ;;
		frontpanel=pidp|frontpanel=virtual)
			frontpanel="$value" ;;
		usbtape=y|usbtape=n)
			usb_paper_tape="$value" ;;
		interface=*|frontpanel=*|usbtape=*)
			echo "pdp1control.config: invalid value '$value' for $name, ignored" >&2 ;;
		*)
			echo "pdp1control.config: unknown setting '$name', ignored" >&2 ;;
		esac
	done < "$settings"
}

# Set one start-time choice in the settings file: every line for the name gets the value,
# or a line is added. The file is written to a temporary name and renamed.
write_setting() {
	temp_file="$settings.tmp"
	if test -f "$settings" && grep -q "^$1[[:space:]]*=" "$settings"; then
		sed "s/^\($1[[:space:]]*=[[:space:]]*\)[^[:space:]]*/\1$2/" "$settings" > "$temp_file" || return 1
	else
		{
			if test -s "$settings"; then
				cat "$settings"
				# the file may not end with a newline
				test -z "$(tail -c 1 "$settings")" || echo
			fi
			echo "$1=$2"
		} > "$temp_file" || return 1
	fi
	mv "$temp_file" "$settings"
}

read_settings

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
	        nohup bin/panel_pidp1 > /dev/null 2>&1 &
	elif [ "$frontpanel" = "virtual" ]; then
		echo starting on-screen virtual front panel, not PiDP-1 hardware
		echo use
		echo    pdp1control panel pidp
		echo to change that, if you have a PiDP-1!
	        nohup bin/vpanel_pdp1 > /dev/null 2>&1 &
	else
                echo ERROR: NO VALID FRONT PANEL DRIVER, fix with
		echo    pdp1control panel
	fi


	sleep 1 # only needed for autoboot at startup

	echo read boot config from sense switches
	bin/scanpf
	sw=$?

	#sw="${2:-$sw}"
	sw=$(printf "%o" $sw)
	echo switches set to $sw

	echo start pidp1 in screen
	screen -dmS pidp1 bin/pdp1
	status=$?

	if [ "$interface" = "gui" ]; then
		echo start gui peripherals
		sleep 2
		nohup bin/pdp1_periphES > /dev/null 2>&1 &
	elif [ "$interface" = "web" ]; then
		echo start web server
		cd "$root/web_pdp1"
		nohup go run "$root/web_pdp1/pdpsrv.go" > /dev/null 2>&1 &
		cd "$root"
	elif [ "$interface" = "apps" ]; then
		echo start apps
		sleep 1
		echo start /usr/local/bin/t30dpy
		nohup /usr/local/bin/t30dpy >/dev/null 2>&1 &
		# tapevis, the tape reader and punch window, is rarely wanted; run bin/tapevis for it
	fi

	sleep 1 # 0.3
	echo "Configuring PiDP-1 for boot number $sw"
	cat "bootcfg/${sw}.cfg" | bin/pdp1ctl
	
	if [ "$usb_paper_tape" = "y" ]; then
		echo start usb paper tape tool
		nohup bin/pdp1_usb_monitor > /dev/null 2>&1  &
	fi

	return $status
}

# Signals go by exact process name: a pattern such as 'pdp1$' also matches vpanel_pdp1,
# which has no SIGHUP handler and would end.
do_reload() {
        pkill -x -HUP pdp1
}

do_t30reload() {
        pkill -HUP 't30dpy'
        pkill -HUP 't30dpy3'
}

do_panelreload() {
        pkill -x -HUP panel_pidp1
}

do_stop() {
	#kill any support programs that may be running
	pkill -x tapevis
	pkill -x pdp1_periphES
	pkill -x pdpsrv
	pkill -x panel_pidp1
	pkill -x vpanel_pdp1
	# its name is over the kernel's 15 characters, so match the command line
	pkill -f bin/pdp1_usb_monitor

	sleep 0.5
	is_running
	if [ $? -eq 0 ]; then
	    echo "PiDP-1 is already stopped." >&2
	    status=1
	else
	    echo "Stopping PiDP-1"
	    #screen -S pidp1 -X quit
	    pkill -x pdp1
	    status=$?
	fi

	sleep 0.5
        # pdp1 may be running outside of screen
	pkill -x pdp1

	# t30dpy ends by itself when pdp1's display connection closes. Its SIGTERM handler can hang
	# it for good, so it is signaled only if it is still there after 3 seconds, and killed if
	# that does not end it.
	for i in $(seq 30); do
		pgrep -x 't30dpy|t30dpy3' > /dev/null || break
		sleep 0.1
	done
	if pgrep -x 't30dpy|t30dpy3' > /dev/null; then
		pkill -x 't30dpy|t30dpy3'
		for i in $(seq 20); do
			pgrep -x 't30dpy|t30dpy3' > /dev/null || break
			sleep 0.1
		done
		pkill -KILL -x 't30dpy|t30dpy3'
	fi

	return $status
}

do_set() {
	if [ -z "$2" ]; then
		echo "Error: No interface specified. Use 'gui', 'web', or 'apps'." >&2
		return 1
	fi
	
	case "$2" in
		gui|web|apps)
			if write_setting interface "$2"; then
				echo "Interface set to '$2', used at the next start"
				exit 0
			else
				echo "Error: Failed to update $settings" >&2
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
			if write_setting frontpanel "$2"; then
				echo "Panel set to '$2', used at the next start"
				exit 0
			else
				echo "Error: Failed to update $settings" >&2
				return 1
			fi
			;;
		*)
			echo "Error: Invalid panel option '$2'. Use 'pidp' or 'virtual'." >&2
			return 1
			;;
	esac
}

do_usbtape() {
	if [ -z "$2" ]; then
		echo "Error: No usbtape option specified. Use 'y' or 'n'." >&2
		return 1
	fi
	
	case "$2" in
		y|n)
			if write_setting usbtape "$2"; then
				echo "usb_paper_tape set to '$2', used at the next start"
				exit 0
			else
				echo "Error: Failed to update $settings" >&2
				return 1
			fi
			;;
		*)
			echo "Error: Invalid usbtape option '$2'. Use 'y' or 'n'." >&2
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

  reload)
	do_reload
	;;

  reloadt30)
	do_t30reload
	;;

  reloadpanel)
	do_panelreload
	;;

  set)
	do_set $1 $2
	;;
  panel)
	do_panel $1 $2
	;;
  usbtape)
	do_usbtape $1 $2
	;;

  status)
	screen -ls pidp1 | egrep '[0-9]+\.pidp1'
	;;

  stat)
	do_stat
	;;
  ?)
	echo "Usage: pdp1control {start|stop|reload|restart|set|panel|usbtape|status|stat}" || true
	echo "       pdp1control {reloadt30|reloadpanel}" || true
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

