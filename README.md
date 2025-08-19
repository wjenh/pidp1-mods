# Github repository for the PiDP-1 project

Main web sites:

- https://obsolescence.dev/pdp1 - Overview & context
- https://obsolescence.wixsite.com/obsolescence/pidp1 - Further details
- [https://groups.google.com/g/pidp-1](https://groups.google.com/g/pidp-1) - User group

<img src="https://github.com/user-attachments/assets/8e383528-861b-4829-8799-4ecbf265fde4" align="center" />
Credits: Simulation written by Angelo Papenhoff (https://github.com/aap), hardware: Oscar Vermeulen

# Install instructions

WARNING: PROJECT NOT FINAL YET, TO AVOID FRUSTRATION PLS USE AFTER AUGUST 25th :-)

    cd /opt
    sudo git clone https://github.com/obsolescence/pidp1.git
    /opt/pidp1/install/install.sh

Note that you do not necessarily need to have the PiDP-1 hardware. 

This will run on any Pi. Without PiDP-1 front panel hardware, you can use the on-screen virtual front panel. See the manual for a how-to: <PiDP-1_Manual.pdf>

With the virtual front panel, this will ALSO run fine on regular Linux (tested on Ubuntu 24.10) and presumably, Windows 11 with WSL2 subsystem (although untested). Just in case you want to have a mobile PDP-1 to develop on the go.

# Operating the PiDP-1

Depending on your preferences, you can run the PiDP-1 headless from your laptop, or from the Pi GUI with monitor & keyboard.

![image](https://github.com/user-attachments/assets/e80a1c29-a8c9-4a50-a3a8-43e7163490fb)

**How to use from the Pi GUI or command line:

**pdp1control** is the command to control the (simulated) PDP-11. You can use the pdp1control desktop icon, or use the Linux command line:

- `pdp1control stop`, `pdp11control status` or `pdp1control stat` rather do what you'd expect.
- `pdp1control start`, `pdp1control restart`: (re)start the PDP-1
- `pdp1control start x` or `pdp11control restart x` can be used if you are without the PiDP-1 hardware. `x` is the boot number you want to run.

There are two **configuration** options:

- `pdp1control set gui` or `pdp11control set web` switch between the gui setup (HMDI/keyboard on the Pi) and the web server (http://pidp1.local:8080/).
- `pdp1control panel pidp` or `pdp11control panel virtual` switch between the PiDP-1 hardware front panel or a virtual on-screen front panel.

**pdp1** is the simple way to get access to the PDP-1 peripherals. Or, you can use its equivalent collection of desktop icons:

- `pdp1 type30` will open the Type 30 graphics display. Resize to your liking, F11 makes it full-screen
- `pdp1 soroban` opens the Soroban/IBM typewriter used as the teletype console
- `pdp1 ptp` saves a freshly generated tape coming out of the paper tape punch
- `pdp1 ptr` loads an existing paper tape into the paper tape reader
- `pdp1 sim` drops you into the PDP-1 simulator program, which you'll not normally need to do

**How to use the web browser interface:

If you want to run your PiDP-1 headless (i.e., without monitor and keyboard), enable the PiDP-1 web server (`pdp1control set web`). Then use your web browser to go to **http://pidp1.local:8080**
The Web interface offers some creature comforts for developing code, integrating a cross assembler etc

**How to use PiDP-1 devices headless, without the web browser:
(Forthcoming, undergoing changes 20250821)
The Type 30 simulator as well as the Soroban Typewriter and the High Speed Paper Tape Reader & Punch connect over TCP/IP, so you can run these on your laptop just as well as on your local PiDP-1 itself. This makes it possible to leave the PiDP-1 completely headless. Good for repurposing older, slow Raspberry Pis!
<a rdpd1control and rpdp1 script will make this comfortable - WIP>

**Two quick examples to test everything:

Use `pdp1control set` and `pdp1control panel` to configure to your preferences.
Then, to test the Type 30 display:
`pdp1control start 1`. The Type 30 should light up with Snowflake.
Then, to test the DDT debugger in action:
`pdp1 ptr` , load /opt/pidp1/tapes/ddt.rim. Press the READ IN switch on the front panel. After the paper tape has been read, DDT is up and running.
Do `pdp1 soroban` to connect a terminal. Press L (Shift-L) and type HELLO WORLD. You'll see the paper tape punch outputting this. Hit return and from there on, read the DDT manual :-)
