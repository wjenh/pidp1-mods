#!/bin/bash

if [ $# -eq 0 ]; then

	procs=`screen -ls pidp1 | egrep '[0-9]+\.pidp1' | wc -l`
	echo $procs
	if [ $procs -ne 0 ]; then
		screen -Dr pidp1
	fi

else
	case $1 in
		con)
			//nohup /opt/pidp10/bin/sty > /dev/null 2>&1 &
			nohup /opt/pidp10/bin/sty -e telnet localhost 1025 > /dev/null 2>&1 &
			;;
		soroban)
			nohup lxterminal --command="telnet localhost 1041" > /dev/null 2>&1 &
			;;
		ptr)
			nohup /opt/pidp1/bin/load_ptr.sh > /dev/null 2>&1 &
			;;
		ptp)
			nohup /opt/pidp1/bin/save_ptp.sh > /dev/null 2>&1 &
			;;
		type30)
			nohup /opt/pidp1/bin/p7simES localhost > /dev/null 2>&1 &
			;;
		type30b)
			nohup /opt/pidp1/bin/p7simES -p 3401 localhost > /dev/null 2>&1 &
			;;
		tape)
			nohup /opt/pidp1/bin/tapevis > /dev/null 2>&1 &
			;;
		*)
			echo options are: 
			echo  [soroban ptr ptp type30 tape]
			echo when run without options, 
			echo  pdp1 brings you into the simulator - Ctrl-A d to leave
			;;
	esac
fi

