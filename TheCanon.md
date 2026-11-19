## The `pidp1-mods` Canon

Everything in `pidp1-mods` and `pidp1-tools` is built around four guiding principles:

1. **"If it could have been done at the time, then it's historically accurate to do it now."**  
   Every feature respects authentic 1961 PDP-1 hardware constraints and available peripherals and hacks
   that could have been built with hardware available at the time, but see the note below.
   Cost is not a factor, DARPA had plenty of money.
2. **"Go big or go home."**  
   Don't settle for toy demos when you can build a complete implementation.
3. **"If it's worth doing, it's worth overdoing."**  
   Recreate historically authentic programs, then add more features that could have been done if
   someone had thought of them.
4. **"Do it right."**  
   No half-measures.
   Tested, documented, complete code with examples where appropriate.

The PDP-1 was the beginning of the hacker age.
Hardware was frequently modified, new assemblers, compilers, editors, there was a flood of new ideas.
The only reason the PDP-1 didn't have some of the major historical programs wasn't because it couldn't do them,
it was because DEC was churning out new computers rapidly, the PDP-1 became obsolete quickly.

As an example, the oringinal Unix was implemented at Bell Labs on a PDP-7, a direct descendent of the PDP-1.
Why? Because that was what was lying around at the time.
If a PDP-1 had been available instead, it certainly would have been used.

As a hacker from that era, we didn't use paper tapes, Sorobans, punched cards, etc. because we liked them,
they were what we had.
Any true hacker would have jumped at the chance to use better tools.

Even tools like am1, ad1, and c1 were theoretically possible, cross-compilers existed at the time running on
big mainframes, it was actually not uncommon.

*NOTE* - Some divergence can't be avoided. The pidp-1 isn't hardware, it's sofware running with the constrants
being wrapped in a complex operating system forces, such as battling with the Linux scheduler, having to use
sockets and ports, etc.
But, these things are hidden as well as is practical.
