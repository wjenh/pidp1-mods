# Github repository for the PiDP-1 project

Main web sites:
- https://obsolescence.dev/pdp1 - Overview & context
- https://obsolescence.wixsite.com/obsolescence/pidp1 - Further details
- [https://groups.google.com/g/pidp-1](https://groups.google.com/g/pidp-1) - User group

<img src="https://github.com/user-attachments/assets/8e383528-861b-4829-8799-4ecbf265fde4" align="center" />
Credits: Simulation written by Angelo Papenhoff (https://github.com/aap), hardware: Oscar Vermeulen

# Install instructions

WARNING: PROJECT NOT READY YET, TO AVOID FRUSTRATION PLS USE AFTER APRIL 18TH :-)

    cd /opt
    sudo git clone https://github.com/obsolescence/pidp1.git
    /opt/pidp1/install/install.sh

Note that you do not necessarily need to have the PiDP-11 hardware. 

This will run on any Pi, but without PiDP-1 front panel hardware, you'll have to tell which paper tape you want to load from the command line rather than from the front panel switches. See the manual for a how-to: <PiDP-1_Manual.pdf>

This will ALSO run on any Linux laptop (command line only, no desktop features, tested on Ubuntu 24.10) and presumably, Windows 11 with WSL2 subsystem (although untested). Just in case you want to have a mobile PDP-1 to develop on the go.

# Operating the PiDP-11

Depending on your preferenes, you can run the PiDP-1 headless from your laptop, or from the Pi GUI with monitor & keyboard.

![image](https://github.com/user-attachments/assets/e80a1c29-a8c9-4a50-a3a8-43e7163490fb)

**How to use from the Pi GUI:

**pdp1control** is the command to control the (simulated) PDP-11. You can use the pdp1control desktop icon, or use the Linux command line:
- `pdp1control stop`, `pdp11control status` or `pdp1control stat` rather do what you'd expect.
- `pdp1control start, `pdp1control restart`: (re)start the PDP-1
- `pdp1control start x` or `pdp11control restart x` can be used if you are without the PiDP-1 hardware. `x` is the boot number you want to run.

**pdp1** is the simple way to get access to the PDP-1 peripherals. Or, you can use its collection desktop icons:
- `pdp1 type30` will open the Type 30 graphics display. Resize to your liking, F11 makes it full-screen
- `pdp1 soroban` opens the Soroban/IBM typewriter used as the teletype console
- `pdp1 ptp' saves a freshly generated tape coming out of the paper tape punch
- `pdp1 ptr' loads an existing paper tape into the paper tape reader
- `pdp1 sim` drops you into the PDP-1 simulator program, which you'll not normally need to do

**How to use from the Web Control Panel:

If you want to run your PiDP-1 headless (i.e., without monitor and keyboard), the above functionality is also accessible through the Web-based control panel. Just go to **http://pidp1.local:1234**
The Web Control Panel offers some more creature comforts for developing code, integrating a cross assembler etc

**How to use PiDP-1 devices headless:

The Type 30 simulator as well as the Soroban Typewriter and the High Speed Paper Tape Reader & Punch connect over TCP/IP, so you can run these just as well on your laptop as on your local Pi itself. This makes it possible to leave the PiDP-1 itself completely headless.
<a rdpd1control and rpdp1 script will make this comfortable - WIP>
