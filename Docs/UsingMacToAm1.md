# Using the mactoam1 converter

This document describes the mactoam1 converter and how to use it

## What is mactoam1?

Mactoam1 is a simple conversion utility to convert macro or macro1 files to am1 files.
While the differences are fairly tivial, it makes it very easy to start using am1 for classic sources.

It could have been implemented as a sed script, but what's the fun in that?

## What exactly does it do?

Comments are converted from a single / to // for cpp compatibility.

Location assignments, xxx/, are converted to xxx:.

Constant specifiers (const and (const) are converted to [const and [const] respectively.

That's it.

## Why those changes?

Two reasons.

First, remove the ambibuity with the use of / for both a location assignment and a comment.
It isn't actually handled very well in macro.

Second, am1 provides more functionality where those symbols make more sense as it uses them, specifically
parentheses in expressions and / for division, neither of which are supported in macro.

## Building mactoam1

Just type make.

## Usage

mactoam1 [-o outfile] [sourcefile]

If no input file is given, stdin is used.

If no output file is given, stdout is used.
