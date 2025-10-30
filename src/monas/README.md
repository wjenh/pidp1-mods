Μονάς
=====

Monas is a PDP-1 cross-assembler that supports extended memory but no macros.
If you need macros, consider preprocessing with `m4`.
Apart from that it should be fairly but not fully compatible with MACRO.

Usage
=====

Run `monas prog.mac` and, if there are no errors,
it will produce `prog.rim` and `prog.lst`.
Currently `prog.rim` will be written out in standard BIN format
with a loader in RIM format included.
If extended memory was used, a different BIN format and loader will be used.

TODO
====

of course!
