## Using pdp1central

This document describes pdp1central, the desktop control window for the pidp-1 emulator.

This is version 1.10\
Edit date 25-Oct-2026\
Added the new Companion control buttons and the drum and microtape libraries.

## What is pdp1central?

pdp1central is one window that does what the scripts in /opt/pidp1-mods/bin do from a desktop:
- it shows which of the emulator's programs are running;
- it starts, stops, restarts and reloads the emulator;
- it keeps the start-time choices: the interface, the t30dpy display, the front panel and the USB
  paper tape;
- it mounts paper tapes in the reader and saves what the punch punches;
- it fast loads a tape straight into the running pdp-1's memory, with fastload or with ovlload
  for a program with drum overlays;
- it mounts and unmounts microtapes on the Type 550 drives, with mtp, choosing from a tape library;
- it puts drum images from a library onto the Type 23 drum, and saves the drum to the library;
- it turns the emulator's sound on and off;
- it opens the typewriter, t30dpy and console windows;
- it sets the running emulator's gain, audio filters, tuning and sample rate, and its multiply and
  divide option, on the Live tab;
- it edits /opt/pidp1-mods/pidp1.config, the configuration file, setting by setting;
- its colors come from a choice of schemes, with single colors changeable in a file of its own.

It runs on the machine that runs the emulator.
The scripts stay, and can still be used on a system without a desktop.
Pdp1central starts, stops, and reloads the emulator and starts t30dpy by running bin/pdp1control.sh
so the two always do the same thing.

## Starting it

Use the pdp1central desktop icon or type

```
pdp1central
```

Exit or closing the window leaves pdp1central and does not stop the emulator or anything it started.
If a start, stop, or file dialog is still running, pdp1central waits for it to finish first.
Exit again to leave immediately.

Options:
- -r *root*: the install directory, /opt/pidp1-mods by default.
- -s *schema*: the settings description, *root*/src/pdp1central/pidp1config.schema by default.

## The window

### Moving and resizing

The window can be moved by its title bar and resized using the standard edge and corner grabs.
An additional drag mode is available to work around deficiences in the Pi Trixie window manager.
Holding the right mouse button down in the window allows moving it, the same functionality that t30dpy provides.

### The top strip

A light for each program the chosen start-time choices run, green when it is running:
- pdp1, the emulator;
- the panel driver: panel_pidp1 for the PiDP-1 hardware, vpanel_pdp1 for the on-screen panel;
- the front end: pdp1_periphES for gui, pdpsrv for web, t30dpy for apps;
- t30dpy, when *also start t30dpy* is checked with gui or web;
- the USB tape monitor, when USB paper tape is chosen;
- port 1040, the emulator's command port, green when it accepts a connection.

Then the buttons:
- Start, Stop and Restart run pdp1control start, stop and restart.
- Reload config runs pdp1control reload, the emulator rereads pidp1.config.
- Exit leaves pdp1central.

While one of these runs, the buttons are disabled and the strip shows what is running and for how long.
An error is shown under the strip, staying until another occurs.

Stop ends the emulator first, then the t30dpy display, which normally closes by itself when the
emulator stops.
Either still open after 3 seconds is ended.

### The Control tab

**Start-time choices.**

Interface (gui, web or apps), Type 30 display (also start t30dpy),
Front panel (PiDP-1 hardware or virtual) and USB paper tape (yes or no).
A click saves the choice at once, and it is used at the next start or restart.

*Also start t30dpy* starts the t30dpy display beside the gui or web front end.
Apps always starts t30dpy, the box is shown checked and cannot be changed.
The emulator sends the display to one window at a time, the last one to connect:
- with gui, t30dpy starts once pdp1_periphES has connected; t30dpy has the display and the
  gui's display window stays blank;
- with web, t30dpy has the display until the browser's display panel is opened; the browser
  then has it and t30dpy closes.

t30dpy starts only once the emulator is ready for it, at most 5 seconds after the emulator
starts, it closes if it cannot connect.
The line beside the box says which.

**Fast load.**

Load... opens a file dialog, and loads the chosen rim paper tape, either macro or am1,
straight into the running pdp-1's memory with fastload, then starts it without the time a
paper tape read takes.
A running program is stopped first.
A tape whose am1 source has a stop directive is loaded and not started.
The line beside the button says what the last load did or why Load... cannot be used:
the emulator is not running or ad1port is off in pidp1.config.
Only one client can be connected.
With ad1 attached the load fails and the reason is shown.

A program with drum overlays (UsingOverlays.md) is loaded with ovlload which then writes the overlays
to the drum and loads the root tape.
This happens when the tape chosen also has a .ovl with it in the same directory or when
prog.ovl itself is chosen.
The tape and the .ovl must have been assembled together.
If their modification times are more than 5 seconds apart, nothing is loaded and an error given.
Assemble again or remove an .ovl file left from an older build of a program that no longer has
overlays.
The line beside the button then says "with overlays" and reports if ovlload wrote over another tool's
data on the drum.
The Log tab list the tracks.
A drum image picked on the Drums tab afterwards replaces the whole drum, overlays included,
so pick the drum first, then load.

**Paper tape reader.**
- Mount... opens a file dialog, and mounts the chosen tape.
- Remount last mounts the last tape again, causing a read from the start.
- Unmount takes the tape out of the mount file.

The emulator cannot be asked what is mounted, pdp1central shows only what it mounted itself.
Pdp1central remembers the tape only while it runs.

**Paper tape punch.**

Save punch to... opens a file dialog, punch punches to the chosen file.

Tape commands go to the running front end as the regular tape scripts send them.
When no front end is running, they go straight to the emulator.
Under the apps interface that is the usual case, apps does not start tapevis,
the window that shows the reader and punch tapes.
To watch the tapes, run /opt/pidp1-mods/bin/tapevis yourself, tape commands then go to it.
The web interface's front end does not save the punch to a file.
A path with a space in it cannot be sent, since the command ports split at spaces.

**IMPORTANT!** Don't use Save until the program has finished producing a tape.
The punch output goes to a temporary file.
If it isn't complete or doesn't exist, punch will either save only part of the output
or save nothing.

**Audio.**

The button turns the emulator's sound on or off, as pdp1audio on and off do.
The audio enabled setting only allows audio; this is what starts it.

The indicator shows what the last click set: "is on", "is off", or "not set here yet" before the
first click and after pdp1central starts, stops, or restarts the emulator.
A change made elsewhere, with pdp1audio or the desktop audio icons, is not shown.
When the emulator is not running the lamp reads "pidp1 not running".

A reload sets the sound back to what the audio enabled setting says.
When pdp1central reloads the emulator after a click, it puts the sound back as the click set it.
A reload made elsewhere, by pdp1control reload, is not undone.

**Companions.**

The three buttons open the windows that go with the emulator using the same commands as the
desktop icons:
- Typewriter opens a terminal on the typewriter,
  lxterminal -e "telnet localhost 1041".
- t30dpy opens the t30dpy display, /opt/pidp1-mods/bin/t30dpy.
- Pdp1 monitor opens the emulator's screen console, not generally useful except to see
  error messages.
  Ctrl-A d exits it as does closing the window, the emulator continues to run.

The windows are not pdp1central's, they stay open when pdp1central exits.
If a program cannot be started, the message line under the top strip reports why, such as
"lxterminal: not found".

The buttons work only while the emulator runs, otherwise they are disabled and the row reads
"pidp1 not running".
Typewriter and t30dpy also wait for the emulator to respond since they would close with
nothing to connect to.
Pdp1 monitor needs only the emulator running.

The t30dpy button is disabled while a t30dpy runs.
The emulator sends the display to the last window to connect.
T30dpy will take it from the gui's display window or the browser's display panel,
the line beside the buttons says which one, if either.

Typewriter has no indicator, and a second click opens a second terminal.
The typewriter takes one connection at a time, the second terminal's telnet is refused and its
window closes at once.
Once the first terminal is closed, Typewriter opens a new one.

**Window colors**

Picks the color scheme (see Colors below).
A click changes the window at once and saves the choice.
When pdp1central.config also changes single colors, a line under the choice says how many were changed.

### The Live tab

Settings the running emulator can take while running.
They are reset to the config file values on the next emulator start unless saved.

**Audio**

Enable or disable audio.
Until a value is read or set or a reload is done,
its values show the pidp1.config file value or the default if not set.
The gain, the cutoff for all channels, the cutoff for each channel 1 to 4,
tuning and sample rate can all be changed.
The last column shows the emulator state for the setting.
Type a value and press Enter or click Set to apply changes.
A value outside its range is not sent, an error error message will say what the range is.
The valid rages are gain 0 to 100, a cutoff 1 to 100000 Hz, tuning 0.001 to 100 (SDL3 only),
sample rate 8000 to 192000.

- Read current values asks the emulator for its settings  and fills the fields.
- CHM filters puts back the Computer History Museum's real PDP-1 filters on all four channels.
- Overflows shows how often the sound clipped, a help when setting the gain.

Setting the cutoff for all channels replaces each channel's cutoff.
A channel set afterwards overrides it for that channel.

Every audio command also applies the audio enabled setting,
read current values and overflows included.

**Save to pidp1.config**

Writes the values set here into pidp1.config the same way the Settings tab does
and does not reload, the emulator already has them.

A cutoff for all channels is saved with the channel lines commented out, since a channel line would override it.
CHM filters comments out all five cutoff lines since those values are the default.
It is refused while the Settings tab has unsaved changes.

When pdp1central reloads the emulator it sends the values set here again.
A start, stop or restart starts the emulator with the config file's values and the values set here are ignored.

**Multiply and divide.**

This shows whether the emulator has the type 10 multiply and divide option on.
It is read every 2 seconds while this tab is shown so a change made elsewhere such as  at the console, with Ctrl-F2
in pdp1_periphES or by a reload is tracked.
The button turns it off or on until the next reload.

The CPU tab's muldiv setting under Settings is the saved value.

### The Microtape tab

A row for each Type 550 drive, 1 to 8, as /opt/pidp1-mods/microtapes.txt lists them,
the tape's file or "empty", and:
- write lock: the drive's WRITE LOCK switch.
  On a drive with a tape a click mounts the same tape
  again with the lock on or off and rewinds it.
  On an empty drive it sets the lock for the next mount.
  A line whose name starts with ./ or ../ is written back with the tape's full path, since mtp
  would take such a name from its own current directory; any other line keeps its name.
- Mount... opens a file dialog.
  Choose an existing tape or type a new name.
  A new tape is blank when it is mounted.
  A name typed with no extension gets .mtp, the microtape image extension; a name with any
  extension is used as typed.
  A tape already on another drive is refused.
  The check resolves a tape rgardless of how its line lists it, a full path or a name relative to
  /opt/pidp1-mods/Microtapes, with or without ./ or ../.
- Mount selected mounts the tape chosen in the tape library below, in the same way.
- Unmount takes the drive's tape out of the mount list.

Pdp1central makes each change with **mtp**.
If mtp fails, the error is shown and the Log tab has what mtp printed.

The table is read back from microtapes.txt so a change made in a terminal with mtp or by
editing the file shows within a second or two.
Comment lines are not shown.
A tape a program mounts itself with the mmt IOT is not in the file and is not shown.
The tab works whether the emulator is running or not.

**The tape library.**

Under the drives, the tapes in /opt/pidp1-mods/Microtapes, then what its index.txt lists (see
Libraries below).
Choose a tape, then Mount selected on a drive's row.
A tape in Microtapes, or under it, is given to mtp by its name relative to Microtapes; any
other by its full path.
Mounting changes only the mount list, as mtp does; no tape file is copied or changed.
A program writing to the tape writes the library's file itself, unless the drive's write lock
is on.

### The Drums tab

The drum images in /opt/pidp1-mods/Drums, then what its index.txt lists (see Libraries below),
and beside them what the chosen image holds, as drumlist lists it: Adventure, a Z-machine story,
drumupdater's tracks or overlays.

- Pick puts the chosen image on the drum:
  1. The drum as it is now is copied to /opt/pidp1-mods/pdp23drum.bak, replacing the last one.
  2. The tab asks whether to save the current drum to the library first:
     Save as... opens Save drum's dialog and picks once the drum is saved;
     Don't save picks at once; Cancel picks nothing, leaving pdp23drum.bak as made.
  3. The image is copied onto /opt/pidp1-mods/pdp23drum.
     The emulator reads the new drum in at its next drum transfer, whether it is running or
     halted.
     An image shorter than a whole drum is fine; the rest of the drum reads as 0.
- Save drum... opens a file dialog in Drums and copies the drum to the chosen file.
  A name with no extension gets .drm.
  A file saved outside Drums also gets an index.txt line, so it is listed.

With no drum file yet, Pick puts the image there without asking, and Save drum has nothing to
save.

**IMPORTANT!** While pidp1 is running, the tab warns that a program writing the drum during a
Pick or Save drum can leave a mix of old and new words.
The drum writes its file a few seconds behind the program, so halt the program first to be sure.

Fast load with overlays writes its tracks onto the drum: a Pick afterwards replaces them, and a
fast load after a Pick writes over part of the image.
Pick first, then load.

### Libraries

The Microtape and Drums tabs each list a library: a directory's own images, .mtp for tapes and
.drm for drum images, and its subfolders, then the lines of an optional index.txt in that
directory, one path per line.
- A line is a file or a folder.
  A relative line is taken from the library's directory.
  A line naming a file is listed whatever its extension, so an older .img tape can be listed by
  its own line.
- Lines starting with # are comments, and blank lines are skipped.
  A path listed twice is shown once.
- A folder has a + that opens it to show its images and subfolders, and - closes it.
  A folder shows at most 500 entries, and says how many more there are.
- A line whose path does not exist is shown grayed and marked missing, so it can be deleted.
- Add file... and Add folder... open a dialog and add the chosen path as a line, relative when
  it is inside the library's directory, creating index.txt if there is none.
- Delete entry takes the chosen entry's line out of index.txt.
  It never deletes a file, and cannot be used on an entry that no line names.
- The first change each time pdp1central runs keeps the old file as index.txt.bak; comments,
  the other lines and their order are kept.
- A change made to the directory or index.txt elsewhere shows within a second or two.

### The Settings tab

A tab for each group of settings, as in UsingTheConfigFile.md, then:
- Other: settings in pidp1.config that pdp1central does not describe, e.g. for an IOT of your own.

Settings that nothing reads any more are not shown; UsingTheConfigFile.md lists them.

Each row has:
- the setting's name, hold the mouse over it for what it does.
  A few show a more understandable name, such
  as *audio enabled* for audio; the tooltip gives the name as used in the file.
- its value, a checkbox for on and off, checked when on, with what a click does beside it,
  "click to turn off" or "click to turn on", a number or a text field.
  Press Enter or leave the field to set it.
- a default box:, checked means pidp1.config does not set it, so the program that reads it uses
  its own default, which is shown.
- when a change takes effect.

A changed setting is marked with a \*.
The botom of the table counts the changes by when they take effect.

**Save**

Writes pidp1.config.
Then, if the emulator is running and a change applies on save or
at the next run, pdp1central runs pdp1control reload.
If the hardware panel driver is running and a panel setting changed, it runs pdp1control reloadpanel.
Changes that need a restart are named with a Restart now button when the emulator is running.

**Revert**

Drops the changes.

When a change takes effect:
- on save the emulator or the hardware panel driver rereads the file at Save.
- at the next run after save the device's plugin rereads it the next time the machine goes to
  run, a START or CONTINUE.
- needs a restart, the next time the emulator starts.
- when pdp1_periph restarts, the next time the program that reads it starts.

How pdp1central writes the file:
- Only the edited value changes.
  Comments, the order of the lines and settings it does not know are kept.
- When a setting appears on more than one line, the last one is the one used, and pdp1central
  changes that one.
  The earlier ones are commented out with a #, so every program reads the same value.
- Checking the default box comments out the setting's line.
  Unchecking it restores a commented
  "#name=value" line if there is one.
- On and off are written as on and off.
 A number with a fraction is always written with a decimal point.
- The first save each time pdp1central runs keeps the old file as pidp1.config.bak.
- If pidp1.config is changed by something else while pdp1central is open, it is read again.
  If there are unsaved changes, pdp1central asks whether to read it again or keep the changes.
- If there is no pidp1.config, pdp1central offers to start from pidp1.config.example.

### The Log tab

What the scripts printed, what the command ports answered, and any errors, the last 500 lines.

## The start-time choices file

The interface, t30dpy, front panel and USB paper tape choices are kept in
/opt/pidp1-mods/pdp1control.config with lines of the form name=value:

```
interface=web
t30dpy=n
frontpanel=virtual
usbtape=n
```

- interface: gui, web or apps.
  The default is web. apps starts the t30dpy display.
- t30dpy: y or n, start t30dpy beside the gui or web front end.
  The default is n.
- frontpanel: pidp or virtual.
  The default is virtual.
- usbtape: y or n.
  The default is n.

Lines starting with # are comments.
  A missing file or line uses the default.

Pdp1control.sh reads the file each time it runs.
Its commands change it, as pdp1central does.

Commands used are:
```
pdp1control set gui
pdp1control t30dpy y
pdp1control panel pidp
pdp1control usbtape y
```

## Colors

pdp1central has five built-in color schemes:
- light, the default, a near-white window with dark text.
- pdp1, the colors of the PDP-1 as in the pidp-1 desktop, a light gray window with blue buttons.
- gray, a mid-gray window with black text.
- dark, a dark gray window with light gray text.
- contrast, black and white, with yellow under the mouse.

The choice is kept in /opt/pidp1-mods/pdp1central.config which pdp1central writes when a scheme
is picked on the Control tab.
The same file can change single colors of the chosen scheme, one line each,
in red, green and blue from 0 to 255, with no spaces:
```
scheme=light
# a darker window, and a blue lit lamp
colorWindow=220,220,215
colorLampOn=40,120,255
```

- A missing file or scheme line uses light.
  A scheme name pdp1central does not know uses light also and is noted in the Log tab.
- The color lines apply to whichever scheme is chosen.
- When a name appears on more than one line, the last one is used.
- A name that is not a color, a value that is not three numbers from 0 to 255 or a line that is
  not name=value is noted in the Log tab and ignored, the rest of the file still applies.
- Picking a scheme in the window changes only the scheme line.
  Comments, color lines and their
  order are kept, and the first change each time pdp1central runs keeps the old file as
  pdp1central.config.bak.
- An edit made to the file while pdp1central is open shows within a second or two.

The color names:

| Name | Colors |
|---|---|
| colorText | text |
| colorWindow | the window |
| colorBorder | the edges of buttons and of the settings and log panes |
| colorButton, colorButtonHover, colorButtonActive | a button, under the mouse, and pressed |
| colorButtonText, colorButtonTextHover, colorButtonTextActive | a button's label, the same three ways |
| colorDisabledButton, colorDisabledText, colorDisabledBorder | a button or box that cannot be used now |
| colorToggle, colorToggleHover | a checkbox or option button, and under the mouse |
| colorToggleCursor, colorToggleCursorHover | its check mark or dot, and under the mouse |
| colorSelect, colorSelectActive | a tab, and the chosen tab |
| colorEdit, colorEditCursor | a text field, and its cursor |
| colorProperty | a number field with arrows |
| colorScrollbar, colorScrollbarCursor, colorScrollbarCursorHover, colorScrollbarCursorActive | a scroll bar, and its handle three ways |
| colorLampOn, colorLampOff | a status lamp, lit and unlit |
| colorWarning | errors and notes |
| colorChanged | a changed setting's name, and the note after Save |

## Building it

pdp1central is C, using the Nuklear toolkit on SDL2 (2.0.18 or later).
Nuklear is included in src/pdp1central, unmodified.

Its file dialogs, for Mount..., Load..., Save punch to..., the Microtape and Drums tabs, run bin/askfile,
which needs Python 3, and shows zenity's file chooser or Python's Tk one when zenity is missing.
Fast load and the Microtape and Drums tabs run bin/fastload, bin/ovlload, bin/mtp and bin/drumlist,
which the install builds.
If zenity, Python 3 or its Tk is missing, use:
```
sudo apt install zenity python3 python3-tk
```

To build pdp1central, in /pidp1-mods/src/pdp1central:

```
make
```
