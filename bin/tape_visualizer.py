#!/usr/bin/env python3
"""
PDP-1 Paper Tape Visualizer
Displays 9-hole paper tape format with FIODEC decoding

Usage: python3 tape_visualizer.py <tape_file>

Controls:
- Left/Right arrows: Scroll through tape
- Home/End: Jump to beginning/end
- Page Up/Down: Scroll faster
- Q/ESC: Quit
"""

import sys
import os
import curses
from typing import List, Tuple, Optional

# FIODEC character table (from decode_fiodec.py)
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

# Special cases
FIODEC[0o13] = "\n"   # Page break
FIODEC[0o200] = " "   # FIODEC 200 = space


class TapeData:
    """Represents decoded paper tape data for visualization"""
    def __init__(self, tape_bytes: bytes):
        self.raw_bytes = tape_bytes
        self.decoded_data = self._decode_tape()
    
    def _decode_tape(self) -> List[Tuple[int, str, str, int, str]]:
        """Decode tape into (byte, fiodec_char, fiodec_display, unicode_code, unicode_char)"""
        result = []
        shift_state = "lower"
        
        for i, byte_val in enumerate(self.raw_bytes):
            # Remove parity bit for FIODEC lookup
            fiodec_code = byte_val & 0x7F
            
            # Get base FIODEC character
            fiodec_char = FIODEC[fiodec_code] if fiodec_code < len(FIODEC) else None
            
            if fiodec_char is None:
                fiodec_display = f"?{oct(fiodec_code)}?"
                unicode_char = "?"
                unicode_code = ord('?')
            elif fiodec_char == "<Lcs>":
                shift_state = "lower"
                fiodec_display = "Lcs"
                unicode_char = ""  # Shift commands don't produce output
                unicode_code = 0
            elif fiodec_char == "<Ucs>":
                shift_state = "upper"  
                fiodec_display = "Ucs"
                unicode_char = ""
                unicode_code = 0
            elif fiodec_char in ("<Blk>", "<Red>"):
                fiodec_display = fiodec_char[1:-1]  # Remove < >
                unicode_char = ""
                unicode_code = 0
            else:
                # Regular character - apply shift state
                display_char = fiodec_char
                
                # Apply shift state if needed
                if shift_state == "upper" and fiodec_code < 0o100:
                    upper_code = fiodec_code | 0o100
                    if upper_code < len(FIODEC) and FIODEC[upper_code] is not None:
                        display_char = FIODEC[upper_code]
                
                fiodec_display = display_char if display_char.isprintable() and display_char not in '\n\t\b' else repr(display_char)
                unicode_char = display_char
                unicode_code = ord(display_char) if len(display_char) == 1 else 0
            
            result.append((byte_val, fiodec_char or "?", fiodec_display, unicode_code, unicode_char))
        
        return result


class TapeVisualizer:
    """TUI Paper Tape Visualizer"""
    
    def __init__(self, tape_file: str):
        self.tape_file = tape_file
        self.tape_data = None
        self.scroll_pos = 0
        self.tape_width = 0
        
    def load_tape(self):
        """Load tape file"""
        try:
            with open(self.tape_file, 'rb') as f:
                tape_bytes = f.read()
            self.tape_data = TapeData(tape_bytes)
            self.tape_width = len(tape_bytes)
            
            # Set initial scroll position to skip leading empty lines
            self.scroll_pos = self._find_initial_display_position()
            
            return True
        except FileNotFoundError:
            print(f"Error: Cannot open {self.tape_file}")
            return False
        except Exception as e:
            print(f"Error loading tape: {e}")
            return False
    
    def _find_initial_display_position(self):
        """Find the initial display position, skipping leading empty lines but keeping 5"""
        if not self.tape_data or not self.tape_data.raw_bytes:
            return 0
        
        # Count leading null bytes (empty lines)
        leading_nulls = 0
        for byte_val in self.tape_data.raw_bytes:
            if byte_val == 0:
                leading_nulls += 1
            else:
                break
        
        # If we have more than 5 leading nulls, skip all but the last 5
        if leading_nulls > 5:
            return leading_nulls - 5
        else:
            # If 5 or fewer leading nulls, start at the beginning
            return 0
    
    def format_9_hole_display(self, byte_val: int) -> List[str]:
        """Format byte as 9-hole paper tape display (vertical)
        Returns list of 9 strings, one for each hole row
        Bit layout (top to bottom): [0 1 2 sync 3 4 5 6 7] (code bits)
        Display labels (top to bottom): [1 2 3 sync 4 5 6 7 8] (convention)
        """
        holes = []
        
        # Data holes 0-2 (first three bits)
        for bit in range(0, 3):
            if byte_val & (1 << bit):
                holes.append('●')  # Punched hole
            else:
                holes.append('○')  # Unpunched hole
        
        # Sync hole (always punched, represented as dot)
        holes.append('.')
        
        # Data holes 3-7 (remaining five bits) 
        for bit in range(3, 8):
            if byte_val & (1 << bit):
                holes.append('●')  # Punched hole
            else:
                holes.append('○')  # Unpunched hole
        
        return holes
    
    def draw_tape_section(self, stdscr, start_col: int, width: int):
        """Draw a section of the tape"""
        if not self.tape_data:
            return
        
        height, screen_width = stdscr.getmaxyx()
        
        # Calculate visible range with bounds checking
        max_data_len = len(self.tape_data.decoded_data)
        start_col = max(0, min(start_col, max_data_len - 1))
        end_pos = min(start_col + width, max_data_len)
        visible_data = self.tape_data.decoded_data[start_col:end_pos]
        
        if not visible_data:
            return
        
        # Prepare display rows
        hole_row = ""      # 9-hole pattern
        fiodec_row = ""    # FIODEC octal
        char_row = ""      # FIODEC character  
        unicode_row = ""   # Unicode octal
        ascii_row = ""     # Unicode character
        
        # Prepare vertical tape display (9 rows of holes + info rows)
        tape_rows = [""] * 9  # 9 hole rows
        fiodec_rows = ["", "", ""]     # 3 rows for FIODEC octal digits
        char_rows = ["", "", ""]       # 3 rows for FIODEC character
        unicode_rows = ["", "", ""]    # 3 rows for Unicode octal digits  
        ascii_rows = [""]              # 1 row for Unicode character
        
        for byte_val, fiodec_char, fiodec_display, unicode_code, unicode_char in visible_data:
            # Vertical 9-hole display (each byte gets 1 char wide + 1 space)
            hole_pattern = self.format_9_hole_display(byte_val)
            for i, hole in enumerate(hole_pattern):
                tape_rows[i] += f"{hole} "  # Hole + 1 space
            
            # FIODEC octal vertically (3 digits stacked)
            fiodec_oct = f"{byte_val:03o}"
            for fiodec_i, digit in enumerate(fiodec_oct):
                fiodec_rows[fiodec_i] += f"{digit} "
            
            # FIODEC character vertically (max 3 chars stacked)
            char_display = fiodec_display[:3] if fiodec_display else "?"
            # Pad with spaces to ensure we have exactly 3 characters
            char_display = f"{char_display:<3}"
            for char_i, char in enumerate(char_display):
                if char_i < 3:  # Only use first 3 characters
                    char_rows[char_i] += f"{char if char.strip() else ' '} "
            
            # Unicode octal vertically (3 digits stacked)
            if unicode_code > 0:
                # Limit to 3 octal digits maximum (0-777 octal = 0-511 decimal)
                unicode_oct = f"{unicode_code:03o}"[-3:]  # Take only last 3 digits
            else:
                unicode_oct = "---"
            for unicode_i, digit in enumerate(unicode_oct):
                unicode_rows[unicode_i] += f"{digit} "
            
            # Unicode character (1 char)
            display_unicode = unicode_char if unicode_char.isprintable() and unicode_char not in '\n\t\b' else '·'
            if not display_unicode:
                display_unicode = '·'
            ascii_rows[0] += f"{display_unicode} "
        
        # Display the rows
        try:
            # Header
            stdscr.addstr(0, 0, f"Paper Tape: {self.tape_file} | Position: {start_col}-{end_pos-1} | Total: {self.tape_width} lines")
            stdscr.addstr(1, 0, "Use ←→ arrows to scroll, Home/End to jump, Page Up/Down for fast scroll, Q to quit")
            
            # Vertical tape visualization (9 rows for holes)
            bit_labels = ["1", "2", "3", "s", "4", "5", "6", "7", "8"]  # s = sync
            row_num = 3
            for i, (tape_row, label) in enumerate(zip(tape_rows, bit_labels)):
                stdscr.addstr(row_num + i, 0, f"{label}: " + tape_row[:screen_width-3])
            
            # FIODEC octal (3 rows)
            row_num += 10  # Skip past holes + 1 blank
            stdscr.addstr(row_num, 0, "F: " + fiodec_rows[0][:screen_width-3])
            stdscr.addstr(row_num + 1, 0, "   " + fiodec_rows[1][:screen_width-3])
            stdscr.addstr(row_num + 2, 0, "   " + fiodec_rows[2][:screen_width-3])
            
            # FIODEC character (3 rows)  
            row_num += 4  # Skip past FIODEC octal + 1 blank
            stdscr.addstr(row_num, 0, "C: " + char_rows[0][:screen_width-3])
            stdscr.addstr(row_num + 1, 0, "   " + char_rows[1][:screen_width-3])
            stdscr.addstr(row_num + 2, 0, "   " + char_rows[2][:screen_width-3])
            
            # Unicode octal (3 rows)
            row_num += 4  # Skip past FIODEC char (now 3 rows) + 1 blank  
            stdscr.addstr(row_num, 0, "U: " + unicode_rows[0][:screen_width-3])
            stdscr.addstr(row_num + 1, 0, "   " + unicode_rows[1][:screen_width-3])
            stdscr.addstr(row_num + 2, 0, "   " + unicode_rows[2][:screen_width-3])
            
            # Unicode character (1 row)
            row_num += 4  # Skip past Unicode octal + 1 blank
            stdscr.addstr(row_num, 0, "T: " + ascii_rows[0][:screen_width-3])
            
            # Legend
            row_num += 2
            stdscr.addstr(row_num, 0, "Legend: ● = punched hole, ○ = unpunched, . = sync hole")
            stdscr.addstr(row_num + 1, 0, "F = FIODEC octal, C = FIODEC char, U = Unicode octal, T = Unicode char")
            
        except curses.error:
            # Handle screen too small
            pass
    
    def run(self, stdscr):
        """Main TUI loop"""
        # Setup curses
        curses.curs_set(0)  # Hide cursor
        stdscr.timeout(100)  # Non-blocking input
        
        if not self.load_tape():
            return
        
        # Track state to avoid unnecessary redraws
        last_scroll_pos = -1
        last_screen_size = (-1, -1)
        
        while True:
            # Calculate display width (each byte takes 2 char columns in vertical mode)
            height, screen_width = stdscr.getmaxyx()
            chars_per_byte = 2  # hole + 1 space
            visible_bytes = max(1, (screen_width - 3) // chars_per_byte)
            
            # Only redraw if something changed
            current_screen_size = (height, screen_width)
            if (self.scroll_pos != last_scroll_pos or 
                current_screen_size != last_screen_size):
                
                stdscr.clear()
                self.draw_tape_section(stdscr, self.scroll_pos, visible_bytes)
                stdscr.refresh()
                
                # Update tracking variables
                last_scroll_pos = self.scroll_pos
                last_screen_size = current_screen_size
            
            # Handle input
            key = stdscr.getch()
            
            if key in [ord('q'), ord('Q'), 27]:  # Q or ESC
                break
            elif key == curses.KEY_LEFT:
                new_pos = max(0, self.scroll_pos - 1)
                if new_pos != self.scroll_pos:
                    self.scroll_pos = new_pos
            elif key == curses.KEY_RIGHT:
                max_pos = max(0, len(self.tape_data.decoded_data) - 1)
                new_pos = min(max_pos, self.scroll_pos + 1)
                if new_pos != self.scroll_pos:
                    self.scroll_pos = new_pos
            elif key == curses.KEY_HOME:
                if self.scroll_pos != 0:
                    self.scroll_pos = 0
            elif key == curses.KEY_END:
                max_pos = max(0, len(self.tape_data.decoded_data) - visible_bytes)
                new_pos = max(0, max_pos)
                if new_pos != self.scroll_pos:
                    self.scroll_pos = new_pos
            elif key == curses.KEY_PPAGE:  # Page Up
                new_pos = max(0, self.scroll_pos - visible_bytes)
                if new_pos != self.scroll_pos:
                    self.scroll_pos = new_pos
            elif key == curses.KEY_NPAGE:  # Page Down
                max_pos = max(0, len(self.tape_data.decoded_data) - visible_bytes)
                new_pos = min(max_pos, self.scroll_pos + visible_bytes)
                if new_pos != self.scroll_pos:
                    self.scroll_pos = new_pos


def test_mode(tape_file: str, width: int = 8):
    """Test mode - print visualization to stdout instead of TUI"""
    visualizer = TapeVisualizer(tape_file)
    if not visualizer.load_tape():
        return
    
    print(f"Paper Tape: {tape_file} | Total: {visualizer.tape_width} lines")
    print(f"Starting at position {visualizer.scroll_pos} (skipped {visualizer.scroll_pos} leading empty lines)")
    print("=" * 80)
    
    # Show bytes starting from the calculated initial position
    start_pos = visualizer.scroll_pos
    end_pos = min(start_pos + width, len(visualizer.tape_data.decoded_data))
    visible_data = visualizer.tape_data.decoded_data[start_pos:end_pos]
    
    # Build vertical tape display
    tape_rows = [""] * 9  # 9 hole rows
    fiodec_rows = ["", "", ""]     # 3 rows for FIODEC octal digits
    char_rows = ["", "", ""]       # 3 rows for FIODEC character
    unicode_rows = ["", "", ""]    # 3 rows for Unicode octal digits  
    ascii_rows = [""]              # 1 row for Unicode character
    
    for byte_val, fiodec_char, fiodec_display, unicode_code, unicode_char in visible_data:
        # Vertical 9-hole display (each byte gets 1 char wide + 1 space)
        hole_pattern = visualizer.format_9_hole_display(byte_val)
        for i, hole in enumerate(hole_pattern):
            tape_rows[i] += f"{hole} "  # Hole + 1 space
        
        # FIODEC octal vertically (3 digits stacked)
        fiodec_oct = f"{byte_val:03o}"
        for fiodec_i, digit in enumerate(fiodec_oct):
            fiodec_rows[fiodec_i] += f"{digit} "
        
        # FIODEC character vertically (max 3 chars stacked)
        char_display = fiodec_display[:3] if fiodec_display else "?"
        # Pad with spaces to ensure we have exactly 3 characters
        char_display = f"{char_display:<3}"
        for char_i, char in enumerate(char_display):
            if char_i < 3:  # Only use first 3 characters
                char_rows[char_i] += f"{char if char.strip() else ' '} "
        
        # Unicode octal vertically (3 digits stacked)
        if unicode_code > 0:
            # Limit to 3 octal digits maximum (0-777 octal = 0-511 decimal)
            unicode_oct = f"{unicode_code:03o}"[-3:]  # Take only last 3 digits
        else:
            unicode_oct = "---"
        for unicode_i, digit in enumerate(unicode_oct):
            unicode_rows[unicode_i] += f"{digit} "
        
        # Unicode character (1 char)
        display_unicode = unicode_char if unicode_char.isprintable() and unicode_char not in '\n\t\b' else '·'
        if not display_unicode:
            display_unicode = '·'
        ascii_rows[0] += f"{display_unicode} "
    
    # Print vertical tape display
    bit_labels = ["1", "2", "3", "s", "4", "5", "6", "7", "8"]  # s = sync
    for tape_row, label in zip(tape_rows, bit_labels):
        print(f"{label}: {tape_row}")
    
    print()  # Blank line
    print("F: " + fiodec_rows[0])
    print("   " + fiodec_rows[1])
    print("   " + fiodec_rows[2])
    
    print()  # Blank line
    print("C: " + char_rows[0])
    print("   " + char_rows[1])
    print("   " + char_rows[2])
    
    print()  # Blank line
    print("U: " + unicode_rows[0])
    print("   " + unicode_rows[1])
    print("   " + unicode_rows[2])
    
    print()  # Blank line
    print("T: " + ascii_rows[0])
    print()
    print("Legend: ● = punched hole, ○ = unpunched, . = sync hole")
    print("F = FIODEC octal, C = FIODEC char, U = Unicode octal, T = Unicode char")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    
    # Check for test mode
    if len(sys.argv) == 3 and sys.argv[2] == "--test":
        test_mode(sys.argv[1])
        return
    
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    
    tape_file = sys.argv[1]
    if not os.path.exists(tape_file):
        print(f"Error: File {tape_file} not found")
        sys.exit(1)
    
    visualizer = TapeVisualizer(tape_file)
    
    try:
        curses.wrapper(visualizer.run)
    except KeyboardInterrupt:
        print("\nVisualization interrupted")
    except Exception as e:
        print(f"Error: {e}")


if __name__ == "__main__":
    main()