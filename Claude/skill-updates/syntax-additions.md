# Additions for am1-pdp1-assembler/references/am1-syntax.md
#
# Append this section near the existing "Usage" section.
#
# Target file location (Windows):
#   %AppData%\Claude\local-agent-mode-sessions\skills-plugin\
#   ...\skills\am1-pdp1-assembler\references\am1-syntax.md
#
# ─────────────────────────────────────────────────────────────────────────────

## Command-line note: don't bother with `-b`, and rim is preferred over bin

`-b` ("generate binary tape image code") is already **the default action** -- it's listed
that way in the Usage section, but it's easy to read the flag list and reflexively add `-b`
to every invocation out of habit. It's harmless (not wrong), just redundant. Plain:
```
am1 program.am1
```
is sufficient to produce a binary, executable tape image.

The actual rim-vs-bin choice is controlled by `-r` ("don't output an initial rim loader"),
not `-b`. Historically, **rim format (the default, no `-r`) is the preferred format for
actual executable programs** -- it's read-in-mode loadable directly. `.bin` (via `-r`,
suppressing the rim loader) was historically more for data tables or other non-executable
payloads meant to be loaded *after* an already-running program (see the "Files with no
loader" section above), though some programs did legitimately use bin for executables too --
both are technically valid, but default to rim (no flags at all) unless there's a specific
reason to suppress the loader.
