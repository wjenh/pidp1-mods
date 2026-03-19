## Using the additional PDP-1D instructions

Quite a few new instructions were added for the PDP-1D, mostly for #45, the BBN timesharing machine.\
Some are useful and are implemented.\
**Ad1** has the mnenmonics for these built in.

These can be enabled or disabled in the /opt/pidp1-mods/pdp1.config file.

## Skip instructions

- sni 644000 skip the next instruction if IO is not zero
- szi 654000 skip the next instruction if IO is zero

## Operate instructions

As with all operate instructions, these can be combined in one instruction.\
If combined, they are done in the order below, first to last.

- cmi 770000 complement IO
- lia 760020 load IO from AC
- lai 760040 load AC from IO
- lsw 760060 swap AC and IO, combines lai and lia
- swp 760060 same instruction as lsw, just a different name

## Special operate instructions

If combined, they are done in the order below, first to last.

- scf 0740040 special clear flags, pf1-6 are cleared
- sci 0740100 special clear IO, IO is cleared
- ifi 0742000 bitwise or pf1-6 with IO bits 12-17, put in pf
- iif 0744000 bitwise or IO bits 12-17 with pf1-6, put in IO
- ida 0740400 index AC, add 1 to AC

Combining ifi and iif will save a copy of IO, bitwise or with pf, then bitwise or pf with the saved copy of IO.\
Using scf or sci before the corresponding ifi or iif will effectively set the destination, nor or into it.

Using both at once with ifi or iif probably makes no sense.
