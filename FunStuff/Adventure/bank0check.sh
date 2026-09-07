#!/bin/sh
# bank0check.sh -- fail the build when bank 0 has grown into am1's read-in
# loader.  TASK-BANK0 B-5, 05-Sep-26.
#
# Bank 0's usable ceiling is 07750 (4072 decimal), NOT 07777 and NOT
# 4096.  am1 writes its read-in loader at 07751-07774 (Tools/AM1/xldr.h:
# LDR_START_ADDR plus a twenty-word xloader[], whose last two words are
# the pointers it deposits through) and the loader is still RUNNING while
# it loads, so one program word deposited at 07751 or above destroys it
# mid-tape.
#
# Nothing in am1 says a word about this: "-S" reports "highest address
# used" against 4096 and the bank-overflow error only fires at 4096.  The
# S25 build sat at 07764 -- twelve words INSIDE the loader -- and ran
# clean through four test files, because the harness loads through
# fastload, which skips the loader block entirely (Tools/AM1/fastload.c).
# A passing test proves nothing about this; only this check does.
#
# WHAT THE CEILING ACTUALLY PROTECTS (owner, 06-Sep-26).  The loader only
# has to survive while it is loading.  Once the tape is in, 07751-07774 is
# ordinary memory and a program may use it at runtime.  The restriction is
# narrower than "bank 0 stops at 07750": no STATICALLY LOADED word may
# land there, because the loader is still running when it would be
# deposited.  That is exactly what this script tests -- am1 -S reports the
# highest address the image occupies, which is a static figure -- so the
# check stands as written.  It is 24 words, and the owner's read is that
# reclaiming them at runtime is unlikely to be worth the machinery.
#
# Usage:  bank0check.sh <file holding the output of "am1 -S ...">
#
# Exits 0 when bank 0 is at or below the ceiling, 1 otherwise -- including
# when the input carries no bank-0 line at all, which is what an am1 that
# failed, or one too old to understand -S, leaves behind.
#
# Kept to POSIX sh on purpose: make runs /bin/sh, which is dash here, and
# dash has neither "8#" base literals nor arithmetic on octal strings.
# That is why the comparison uses the DECIMAL field am1 already prints
# and only carries the octal along for the message.

CEILING_OCTAL=07750
CEILING_DEC=4072

if [ $# -ne 1 ]; then
    echo "bank0check.sh: usage: bank0check.sh <am1 -S output file>" >&2
    exit 1
fi

usage_file="$1"

if [ ! -f "$usage_file" ]; then
    echo "bank0check.sh: no such file: $usage_file" >&2
    exit 1
fi

# am1 -S prints one line per bank used:  "Bank 0, 07734 (4060 decimal)"
high_octal=`sed -n 's/^Bank 0, \([0-7][0-7]*\) (.*$/\1/p' "$usage_file"`
high_dec=`sed -n 's/^Bank 0, [0-7][0-7]* (\([0-9][0-9]*\) decimal).*$/\1/p' "$usage_file"`

if [ -z "$high_dec" ]; then
    echo "bank0check.sh: no 'Bank 0' line in $usage_file -- did am1 fail," >&2
    echo "               or is it too old to understand -S?" >&2
    exit 1
fi

if [ "$high_dec" -gt "$CEILING_DEC" ]; then
    echo "bank0check.sh: BANK 0 OVERFLOWS am1's READ-IN LOADER." >&2
    echo "               highest word used: $high_octal ($high_dec decimal)" >&2
    echo "               usable ceiling:    $CEILING_OCTAL ($CEILING_DEC decimal)" >&2
    echo "               The loader occupies 07751-07774 and runs while it" >&2
    echo "               loads; this build would destroy it mid-tape.  Move" >&2
    echo "               something to bank 3 -- see Adventure/CompletedTasks/" >&2
    echo "               TASK-BANK0-SPACE-RECLAMATION.md." >&2
    exit 1
fi

free=`expr "$CEILING_DEC" - "$high_dec"`
echo "bank0check.sh: bank 0 at $high_octal ($high_dec decimal), $free words below the $CEILING_OCTAL ceiling"
exit 0
