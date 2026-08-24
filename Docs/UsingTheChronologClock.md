
## Using the Chrono-Log series 2100 time-of-day clock

This is version 1.0\
Edit date 24-Aug-2026

# What is it?

The Chrono-Log clock was a time-of-day clock that was used in 1960's computers, the Model 20,000 to be precise.
It is vintage 1965.

It provided the month, day, hour, minute, and second in BCD-encoded values via a parallel output.

While it was never officially supported by DEC, it's highly likely a PDP-1 somewhere had hacked one in.
This is a tribute to that hacking spirit.

Besides, the PDP-1 Adventure game needs it for authenticity.

# The IOT

It uses a single IOT, IOT 70.\
When issued, the hour/minute/second is returned in AC, the day/month/year in IO in a packed form.
The packing is also historically correct, bit-packing was frequently used to compress parallel data like BCD
into a format that could be represented by the machine's word size, 18 bits for the PDP-1.

# The packed format

The AC register will contain:
- hours as a binary number in bits 0-4, 5 bits with a range of 0-23
- minutes as a binary number in bits 5-10, 6 bits with a range of 0-59
- seconds as a binary number in bits 11-17, 6 bits with a range of 0-59

The IO register will contain:
- month as a binary number in bits 0-3, 4 bits with a range of 1-12
- day as a binary number in bits 4-8, 5 bits with a range of 1-31
- year as a binary number in bits 9-17, 9 bits with a range of 0-511

The year is origined at 2000, so 26 corresponds to 2026, 511 to 2511.\
I'm optimistic.\
Why not origin to 1959 or 1960, just for fun?
Well, it's suppsed to be a time-of-day clock, not a pre-Unix epoch clock.
Sorry.
