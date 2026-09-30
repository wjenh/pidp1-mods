IOT_44 -- Test Gateway (device 044 octal)

This is NOT a real PDP-1 peripheral. There is no Type 44 device, and the real Type 19
High Speed Channel hardware had no interface reachable from user code at all -- HSC was
purely a private data path used internally by other devices. This IOT exists solely so an
am1 test program can drive the emulator's internal HSC implementation
(src/blincolnlights/pdp1/highSpeedChannels.c) directly, in order to test it. Device number
044 was picked arbitrarily from the unused range in IOTs/KnownIOTs.txt.

Do not treat this as a model for how a real device IOT should be written -- see
IOTs/Type23Drum or IOTs/DCS2 for that. This one deliberately reaches into an internal,
normally-private emulator subsystem, which is only acceptable because it exists purely to
test that subsystem.

Just copy to the IOTs directory and make in order to use it.

Sub-commands are selected via the "ch" field (MB bits 6-11), DCS2-style. See
Am1Includes/HSC/hscgatewaydefs.ah for the am1-side mnemonics and encodings, and the header
comment in IOT_44.c for the full design rationale, including a deadlock hazard around hgw
(HSCwait) that any test program using this IOT must respect.

Tests/ contains the am1 test programs exercising HSC allocate/free, IMMEDIATE mode,
THREADED mode, NORMAL (real cycle-stealing) mode and channel priority arbitration, error
handling, and simulated abort via HSCreset(). See Tests/README.md for how to build and run
them.
