#!/usr/bin/env python3
import sys

# Unicode → FIODEC translation table (reverse of decode_fiodec.py)
# Create reverse mapping from the original FIODEC table
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

# Build reverse mapping: Unicode character → FIODEC code
# Separate lower case (0-77) and upper case (100-177) mappings
UNI_TO_FIODEC_LOWER = {}
UNI_TO_FIODEC_UPPER = {}

for code, char in enumerate(fio2uni):
    if char is not None and char not in ("<Blk>", "<Red>", "<Lcs>", "<Ucs>"):
        if code < 0o100:  # Lower case range
            UNI_TO_FIODEC_LOWER[char] = code
        else:  # Upper case range
            UNI_TO_FIODEC_UPPER[char] = code

# Special cases for encoding (apply to both mappings)
UNI_TO_FIODEC_LOWER["\n"] = 0o77  # Convert newline to carriage return (FIODEC 077)
UNI_TO_FIODEC_LOWER["\r"] = 0o77  # Also convert CR to FIODEC CR
UNI_TO_FIODEC_LOWER[" "] = 0o200  # Space character as FIODEC 200 (not 000)
UNI_TO_FIODEC_UPPER[" "] = 0o200  # Space available in both modes

# ASCII fallback mappings for characters not easily typed
# These provide common ASCII→FIODEC substitutions
ASCII_FALLBACKS = {
    "*": "×",     # ASCII asterisk → FIODEC multiply symbol
    "^": "↑",     # ASCII caret → FIODEC up arrow  
    "|": "|",     # ASCII pipe (already mapped)
    "~": "~",     # ASCII tilde (already mapped)
    "_": "_",     # ASCII underscore (already mapped)
    # Add more as needed based on your assembly source conventions
}

def encode_pdp1_fiodec(input_file, output_file, add_leader=True, leader_length=5, add_stop_code=None):
    try:
        with open(input_file, "r", encoding="utf-8") as f:
            text = f.read()
    except FileNotFoundError:
        print(f"Error: Cannot open {input_file}")
        sys.exit(1)

    output = []
    unmapped_chars = set()
    current_shift = "lower"  # Track current FIODEC shift state
    
    # Add leader nulls (tape feed) if requested
    if add_leader:
        output.extend([0x00] * leader_length)
    
    for char in text:
        # Determine which shift state is needed for this character
        char_found = False
        required_shift = None
        code = None
        
        # Try lower case first
        if char in UNI_TO_FIODEC_LOWER:
            required_shift = "lower"
            code = UNI_TO_FIODEC_LOWER[char]
            char_found = True
        # Try upper case
        elif char in UNI_TO_FIODEC_UPPER:
            required_shift = "upper" 
            code = UNI_TO_FIODEC_UPPER[char]
            char_found = True
        
        if char_found:
            # Generate shift sequence if needed
            if required_shift != current_shift:
                if required_shift == "upper":
                    output.append(0o74)   # <Ucs> - Upper case shift (lower case range)
                else:
                    output.append(0o72)   # <Lcs> - Lower case shift 
                current_shift = required_shift
            
            # Output the character code (relative to current shift state)
            if required_shift == "upper":
                # Use lower-case-relative code for upper case characters
                output.append(code & 0o77)  # Remove upper bit, send relative code
            else:
                output.append(code)
            continue
            
        # Try ASCII fallback mapping
        if char in ASCII_FALLBACKS:
            fallback_char = ASCII_FALLBACKS[char]
            # Recursively handle fallback character (may need shift too)
            # For simplicity, check both shift states for fallback
            if fallback_char in UNI_TO_FIODEC_LOWER:
                required_shift = "lower"
                code = UNI_TO_FIODEC_LOWER[fallback_char] 
            elif fallback_char in UNI_TO_FIODEC_UPPER:
                required_shift = "upper"
                code = UNI_TO_FIODEC_UPPER[fallback_char]
            else:
                # Fallback failed
                unmapped_chars.add(char)
                continue
                
            # Generate shift if needed for fallback
            if required_shift != current_shift:
                if required_shift == "upper":
                    output.append(0o74)   # <Ucs> 
                else:
                    output.append(0o72)   # <Lcs>
                current_shift = required_shift
            
            # Output fallback character
            if required_shift == "upper":
                output.append(code & 0o77)
            else:
                output.append(code)
            print(f"Converted '{char}' → '{fallback_char}' (FIODEC {oct(code)})")
            continue
        
        # Handle some common control characters
        if char == '\t':
            # Tab → space for simplicity (space doesn't need shift)
            output.append(0o200)  # FIODEC space
            continue
            
        if ord(char) < 32 or ord(char) > 126:
            # Skip other control characters
            print(f"Skipping control character: {repr(char)}")
            continue
            
        # Character cannot be mapped
        unmapped_chars.add(char)
        print(f"Warning: Cannot map character '{char}' (Unicode {ord(char)}) to FIODEC")
        # For debugging, could add a placeholder or skip
        continue

    # Add trailer nulls (tape feed) if requested
    if add_leader:
        output.extend([0x00] * leader_length)

    # Report unmapped characters
    if unmapped_chars:
        print(f"Unmapped characters found: {sorted(unmapped_chars)}")
        print("Consider adding ASCII fallback mappings for these characters.")

    # Handle stop code addition
    if add_stop_code is None:
        # Ask user interactively
        try:
            response = input("Add a stop code to the end of the tape (y/n)? ").lower().strip()
            add_stop_code = response in ['y', 'yes']
        except (EOFError, KeyboardInterrupt):
            # Handle Ctrl+C or EOF gracefully
            print("\nNo stop code added.")
            add_stop_code = False
    
    if add_stop_code:
        # Add 10 empty lines (nulls)
        output.extend([0x00] * 10)
        # Add stop code (octal 013)
        output.append(0o13)
        # Add 5 more empty lines (nulls)
        output.extend([0x00] * 5)
        print("Added stop code sequence: 10 nulls + STOP (013) + 5 nulls")

    # Add parity bits (8th hole) - odd parity for paper tape
    # BUT: leader/trailer nulls (0x00) should remain 0x00 (no parity)
    parity_output = []
    for code in output:
        # Skip parity for tape feed holes (leader/trailer nulls)
        if code == 0x00:
            parity_output.append(code)
            continue
            
        # Count 1-bits in the 7-bit FIODEC code
        bit_count = bin(code & 0x7F).count('1')
        # Add parity bit (0x80) if even number of bits to make odd parity
        if bit_count % 2 == 0:
            code |= 0x80  # Set bit 7 (parity bit)
        parity_output.append(code)

    # Write binary output
    try:
        with open(output_file, "wb") as out:
            # Convert FIODEC codes with parity to bytes
            byte_data = bytes(parity_output)
            out.write(byte_data)
        
        # Calculate content vs leader/trailer for reporting
        leader_trailer_count = (2 * leader_length if add_leader else 0)
        stop_code_count = 16 if add_stop_code else 0  # 10 + 1 + 5
        content_chars = len(output) - leader_trailer_count - stop_code_count
        
        if add_stop_code:
            print(f"Encoding complete. {content_chars} content + {leader_trailer_count} leader/trailer + {stop_code_count} stop sequence = {len(output)} total bytes written to {output_file}")
        else:
            print(f"Encoding complete. {content_chars} content + {leader_trailer_count} leader/trailer = {len(output)} total bytes written to {output_file}")
        
    except Exception as e:
        print(f"Error writing output file: {e}")
        sys.exit(1)


def show_mapping_table():
    """Display the FIODEC encoding table for reference"""
    print("FIODEC Character Encoding Table:")
    print("================================")
    print("Octal | Char | Description")
    print("------|------|------------")
    
    for char, code in sorted(UNI_TO_FIODEC.items(), key=lambda x: x[1]):
        if char == '\n':
            desc = "Newline→CR"
        elif char == '\t':
            desc = "Tab"  
        elif char == ' ':
            desc = "Space"
        elif char == '\b':
            desc = "Backspace"
        elif char.isprintable():
            desc = f"'{char}'"
        else:
            desc = repr(char)
        print(f"{oct(code):>5} | {desc:>4} |")


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--table":
        show_mapping_table()
        sys.exit(0)
        
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input_file> <output_file> [options]")
        print(f"       {sys.argv[0]} --table    (show encoding table)")
        print(f"")
        print(f"Options:")
        print(f"  --no-leader    Don't add leader/trailer null bytes")
        print(f"  --stop         Add stop code sequence automatically (no prompt)")
        print(f"  --no-stop      Don't add stop code sequence (no prompt)")
        sys.exit(1)
    
    # Parse options
    add_leader = True
    add_stop_code = None  # None = ask user, True = add, False = don't add
    
    for arg in sys.argv[3:]:
        if arg == "--no-leader":
            add_leader = False
        elif arg == "--stop":
            add_stop_code = True
        elif arg == "--no-stop":
            add_stop_code = False
        else:
            print(f"Unknown option: {arg}")
            sys.exit(1)

    encode_pdp1_fiodec(sys.argv[1], sys.argv[2], add_leader, 5, add_stop_code)