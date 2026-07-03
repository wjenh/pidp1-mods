## Include files for the High Speed Channel test gateway (IOT 44)

This directory only contains definitions, no code.

## What's here?

- hscgatewaydefs.ah, defines the iots for IOT_44 (IOTs/TestGateway), a test-only device
  that exposes the emulator's internal High Speed Channel implementation to am1 test
  programs. There is no real Type 44 hardware, and the real Type 19 High Speed Channel had
  no interface reachable from user code at all -- see IOTs/TestGateway/README.txt.
