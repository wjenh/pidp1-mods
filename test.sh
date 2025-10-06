echo Starting PiDP-1 test program
echo
echo stopping pdp-1 simulation, if it is running...

pdp1control stop

echo ...stopped pdp-1 simulation
sleep 2
echo
echo starting pidp-1 panel driver...

sudo /opt/pidp1/bin/panel_pidp1&

echo ...started panel driver
sleep 1
echo
echo starting test program.
echo "What you should expect:"
echo
echo "Lights: shows a light running across the panel," 
echo "        (including the IO panel lamps if you have them)"
echo "--> check for any dead lamps."
echo
echo "Switches: set all switches to the DOWN position"
echo "          and the three horizontal switches to RIGHT"
echo "          then you should see the output as:" 
echo "          000000 000000 000000 000000"
echo "--> check for all zeros if switches are set to DOWN and RIGHT"
echo
echo "          toggle each switch and see if it changes the output"
echo "--> check that they do. Or, a quicker check is:"
echo
echo "          If all switches are set to UP and LEFT,"
echo "          and you leave the bottom 8 switches untouched, expect"
echo "          777777 777777 776000 000000"
echo "-->quick check for all small switches, but manually test the lower 8"
echo
echo "Joystick test: press each button"
echo "--> check for a change in output"
echo

/opt/pidp1/bin/pidp1_test

echo
echo test program has ended
echo killing the panel driver

pkill panel_pidp1

echo done.
