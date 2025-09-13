#!/usr/bin/env python3
import sys

# FIODEC → Unicode translation table (7-bit codes, but note 0o200 is special)
FIODEC = [None] * 256

fio2uni = [
    " ", "1", "2", "3", "4", "5", "6", "7", "8", "9", None, None, None, None, None, None,
    "0", "/", "s", "t", "u", "v", "w", "x", "y", "z", None, ",", "<Blk>", "<Red>", "\t", None,
    "·", "j", "k", "l", "m", "n", "o", "p", "q", "r", None, None, "-", ")", "‾", "(",
    None, "a", "b", "c", "d", "e", "f", "g", "h", "i", "<Lcs>", ".", "<Ucs>", "\b", None, "\n",
    " ", "\"", "'", "~", "⊃", "∨", "∧", "<", ">", "↑", None, None, None, None, None, None,
    "→", "?", "S", "T", "U", "V", "W", "X", "Y", "Z", None, "=", "<Blk>", "<Red>", "\t", None,
    "_", "J", "K", "L", "M", "N", "O", "P", "Q", "R", None, None, "+", "]", "|", "[",
    None, "A", "B", "C", "D", "E", "F", "G", "H", "I", "<Lcs>", "×", "<Ucs>", "\b", None, "\n",
]

for i, v in enumerate(fio2uni):
    if v is not None:
        FIODEC[i] = v

# === Special cases ===
FIODEC[0o13] = "\n"   # Page break → convert to newline
FIODEC[0o200] = " "   # FIODEC 200 = space


def decode_pdp1_fiodec(input_file, output_file):
    try:
        with open(input_file, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        print(f"Error: Cannot open {input_file}")
        sys.exit(1)

    output = []
    shift_state = "lower"  # Track current shift state (lower/upper case)
    
    for i, byte in enumerate(data):
        code = byte & 0xFF  # Use full 8-bit value to distinguish 0o200 from 0o000

        # Ignore tape feed (0o00)
        if code == 0o000:
            continue

        # Page break handling
        if code == 0o13:
            print("<converted 0o13 to newline>")
            output.append("\n")
            continue

        char = FIODEC[code & 0x7F]  # Use 7-bit index for normal table

        if char is None:
            output.append(f"?{oct(code)}!")
            continue

        # Handle shift state changes
        if char == "<Lcs>":
            shift_state = "lower"
            continue
        elif char == "<Ucs>":
            shift_state = "upper"
            continue
        elif char in ("<Blk>", "<Red>"):
            # Skip color codes but don't change shift state
            continue

        # Apply shift state to character
        if shift_state == "upper" and (code & 0x7F) < 0o100:
            # Character needs to be interpreted in upper case context
            upper_code = (code & 0x7F) | 0o100
            if upper_code < len(FIODEC) and FIODEC[upper_code] is not None:
                char = FIODEC[upper_code]

        output.append(char)

    # Normalize line endings → always LF
    text = "".join(output).replace("\r\n", "\n").replace("\r", "\n")

    with open(output_file, "w", encoding="utf-8", newline="\n") as out:
        out.write(text)

    print(f"Decoding complete. Output written to {output_file}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input_file> <output_file>")
        sys.exit(1)

    decode_pdp1_fiodec(sys.argv[1], sys.argv[2])

