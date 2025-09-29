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
from enum import Enum

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


class LineType(Enum):
    """Paper tape line types based on holes 7 and 8"""
    LEADER = 0      # Dark yellow - null bytes (leader/trailer)
    LABEL = 1       # Light yellow - human-readable labels (hole 7 punched, 8 empty)
    BINARY = 2      # White - generic binary data (unclassified)
    ALPHANUM = 3    # Green - alphanumeric data (hole 7 empty, 8 varies/empty)
    RIM = 4         # Cyan - RIM format binary (6 lines per word: DIO+data pairs)
    BIN = 5         # Light red - BIN format binary (structured blocks)


class TapeData:
    """Represents decoded paper tape data for visualization"""
    def __init__(self, tape_bytes: bytes):
        self.raw_bytes = tape_bytes
        self.decoded_data = self._decode_tape()

    def _count_parity(self, value: int) -> int:
        """Count number of 1-bits in a value"""
        count = 0
        while value:
            count += value & 1
            value >>= 1
        return count

    def _reconstruct_18bit_words(self, start_idx: int) -> List[Tuple[int, int]]:
        """Reconstruct 18-bit words from groups of 3 binary lines

        Returns list of (line_index, word_value) tuples
        """
        words = []
        i = start_idx

        while i < len(self.raw_bytes) - 2:
            b1, b2, b3 = self.raw_bytes[i], self.raw_bytes[i+1], self.raw_bytes[i+2]

            # All three must have bit 7 set (binary format marker)
            if (b1 & 0x80) and (b2 & 0x80) and (b3 & 0x80):
                # Reconstruct 18-bit word from three 6-bit chunks
                bits_17_12 = b1 & 0x3F
                bits_11_6 = b2 & 0x3F
                bits_5_0 = b3 & 0x3F

                word = (bits_17_12 << 12) | (bits_11_6 << 6) | bits_5_0
                words.append((i, word))
                i += 3
            else:
                break

        return words

    def _detect_binary_format(self, words: List[Tuple[int, int]]) -> dict:
        """Deterministically detect RIM vs BIN format by parsing block structures

        Returns dict mapping line_index -> LineType (RIM or BIN)
        """
        if len(words) < 2:
            return {}

        line_classifications = {}
        word_idx = 0

        # Iteratively parse blocks until all words are classified
        while word_idx < len(words):
            remaining_words = words[word_idx:]

            # Try to parse as BIN block first
            bin_length = self._try_parse_bin_block(remaining_words)
            if bin_length > 0:
                # Classify this BIN block
                for i in range(word_idx, min(word_idx + bin_length, len(words))):
                    line_idx, _ = words[i]
                    line_classifications[line_idx] = LineType.BIN
                    line_classifications[line_idx + 1] = LineType.BIN
                    line_classifications[line_idx + 2] = LineType.BIN
                word_idx += bin_length
                continue

            # Try to parse as RIM block
            rim_length = self._try_parse_rim_block(remaining_words)
            if rim_length > 0:
                # Classify this RIM block
                for i in range(word_idx, min(word_idx + rim_length, len(words))):
                    line_idx, _ = words[i]
                    line_classifications[line_idx] = LineType.RIM
                    line_classifications[line_idx + 1] = LineType.RIM
                    line_classifications[line_idx + 2] = LineType.RIM
                word_idx += rim_length
                continue

            # Neither BIN nor RIM, skip this word (leave as generic BINARY)
            word_idx += 1

        return line_classifications

    def _try_parse_bin_block(self, words: List[Tuple[int, int]]) -> int:
        """Try to parse a BIN block starting at words[0]

        Returns: length of BIN block in words, or 0 if not a BIN block
        """
        if len(words) < 2:
            return 0

        # Check if starts with two consecutive DIOs (32xxxx)
        word0_octal = f"{words[0][1]:06o}"
        word1_octal = f"{words[1][1]:06o}"

        if not (word0_octal.startswith("32") and word1_octal.startswith("32")):
            return 0

        # Extract addresses from DIO instructions
        start_addr = words[0][1] & 0o7777  # Last 12 bits
        end_addr = words[1][1] & 0o7777

        # Calculate expected data words
        data_words = end_addr - start_addr + 1

        # BIN block length = 2 DIOs + data words (ignoring checksum)
        bin_length = 2 + data_words

        # Make sure we don't exceed available words
        return min(bin_length, len(words))

    def _try_parse_rim_block(self, words: List[Tuple[int, int]]) -> int:
        """Try to parse a RIM block starting at words[0]

        Returns: length of RIM block in words, or 0 if not a RIM block
        """
        if len(words) < 2:
            return 0

        # Check if word[0] is DIO but word[1] is NOT DIO
        # (This distinguishes RIM from BIN)
        word0_octal = f"{words[0][1]:06o}"
        word1_octal = f"{words[1][1]:06o}"

        if not word0_octal.startswith("32"):
            return 0

        if word1_octal.startswith("32"):
            # Both are DIO, this is BIN not RIM
            return 0

        # This looks like RIM format (DIO-data pairs)
        # Scan forward while even-indexed words are DIO (32xxxx) or JMP (60xxxx)
        rim_length = 0
        for i in range(0, len(words), 2):
            word_octal = f"{words[i][1]:06o}"

            if word_octal.startswith("32"):  # DIO
                rim_length = i + 2  # Include this DIO and next data word
            elif word_octal.startswith("60"):  # JMP (end of RIM block)
                rim_length = i + 1  # Include the JMP
                break
            else:
                # Not DIO or JMP, RIM block ends here
                break

        return rim_length

    def _classify_line_type(self, byte_val: int, context_alphanum_ratio: float) -> LineType:
        """Classify tape line type based on holes 7 and 8 (bits 6 and 7) with context

        Args:
            byte_val: The byte value to classify
            context_alphanum_ratio: Ratio of alphanumeric lines seen so far (0.0-1.0)
        """
        if byte_val == 0x00:
            return LineType.LEADER

        holes_7_8 = byte_val & 0xC0  # Mask bits 6 and 7

        # Hole 7 punched, 8 empty = human-readable label
        if holes_7_8 == 0x40:
            return LineType.LABEL

        # Hole 7 empty, hole 8 empty = definitely alphanumeric (no parity needed)
        if holes_7_8 == 0x00:
            return LineType.ALPHANUM

        # Hole 7 empty, hole 8 punched = ambiguous (parity bit OR binary marker)
        if holes_7_8 == 0x80:
            # Check parity of lower 6 bits (bits 0-5)
            lower_6_bits = byte_val & 0x3F
            parity_count = self._count_parity(lower_6_bits)

            # If lower 6 bits already have ODD parity, then bit 7=1 makes EVEN parity
            # This is INVALID for alphanumeric (which needs odd parity)
            # Therefore: definitely BINARY
            if parity_count % 2 == 1:  # odd parity in lower 6 bits
                return LineType.BINARY

            # If lower 6 bits have EVEN parity, bit 7=1 creates odd parity
            # This is VALID for alphanumeric, but could also be binary
            # Use context: if 70%+ alphanumeric so far, assume alphanumeric
            if context_alphanum_ratio >= 0.70:
                return LineType.ALPHANUM
            else:
                return LineType.BINARY

        # Default fallback
        return LineType.ALPHANUM

    def _decode_tape(self) -> List[Tuple[int, str, str, int, str, LineType]]:
        """Decode tape into (byte, fiodec_char, fiodec_display, unicode_code, unicode_char, line_type)

        Uses three-pass approach:
        1. First pass: Analyze tape to determine if it's primarily alphanumeric or binary
        2. Second pass: For binary sections, detect RIM vs BIN format
        3. Third pass: Classify each byte with global context
        """
        result = []
        shift_state = "lower"

        # FIRST PASS: Determine tape's overall nature
        # Count lines that are definitely alphanumeric (bit 7=0, non-zero)
        definite_alphanum = sum(1 for b in self.raw_bytes if b != 0x00 and (b & 0x80) == 0)

        # Count lines that are definitely binary (bit 7=1 AND lower 6 bits have odd parity)
        definite_binary = 0
        binary_start_idx = None
        for i, b in enumerate(self.raw_bytes):
            if b != 0x00 and (b & 0x80) == 0x80:
                if binary_start_idx is None:
                    binary_start_idx = i
                lower_6 = b & 0x3F
                if self._count_parity(lower_6) % 2 == 1:  # odd parity in lower 6 bits
                    definite_binary += 1

        # Calculate global context ratio
        total_definite = definite_alphanum + definite_binary
        global_alphanum_ratio = definite_alphanum / total_definite if total_definite > 0 else 0.0

        # SECOND PASS: Detect RIM/BIN format in all binary sections
        binary_format_map = {}
        if global_alphanum_ratio < 0.70:
            # This is a binary tape, find and analyze all binary sections
            i = 0
            sections_found = 0
            while i < len(self.raw_bytes):
                # Skip non-binary bytes
                while i < len(self.raw_bytes) and (self.raw_bytes[i] & 0x80) == 0:
                    i += 1

                if i >= len(self.raw_bytes):
                    break

                # Found start of binary section, reconstruct words
                words = self._reconstruct_18bit_words(i)
                if len(words) > 0:
                    sections_found += 1
                    # Detect format for this section
                    section_format = self._detect_binary_format(words)
                    binary_format_map.update(section_format)

                    # Skip past this section
                    last_line = words[-1][0] + 2  # Last line of last word
                    i = last_line + 1
                else:
                    i += 1

        # THIRD PASS: Classify each byte with global context
        for i, byte_val in enumerate(self.raw_bytes):
            # Check if this line has a specific binary format classification
            if i in binary_format_map:
                line_type = binary_format_map[i]
            else:
                # Use standard classification
                line_type = self._classify_line_type(byte_val, global_alphanum_ratio)

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

            result.append((byte_val, fiodec_char or "?", fiodec_display, unicode_code, unicode_char, line_type))

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
        # Store as list of (text, color_pair) tuples for each column
        tape_rows = [[] for _ in range(9)]  # 9 hole rows
        marker_row = []  # Marker row for RIM/BIN block starts
        fiodec_rows = [[] for _ in range(3)]  # 3 rows for FIODEC octal digits
        char_rows = [[] for _ in range(3)]  # 3 rows for FIODEC character

        # Track previous line type to detect block starts
        prev_line_type = None

        for idx, (byte_val, fiodec_char, fiodec_display, unicode_code, unicode_char, line_type) in enumerate(visible_data):
            # Determine color pair based on line type
            if line_type == LineType.LEADER:
                color_pair = 1
            elif line_type == LineType.LABEL:
                color_pair = 2 | curses.A_BOLD  # Bold yellow for labels
            elif line_type == LineType.BINARY:
                color_pair = 3
            elif line_type == LineType.ALPHANUM:
                color_pair = 4
            elif line_type == LineType.RIM:
                color_pair = 5
            elif line_type == LineType.BIN:
                color_pair = 6
            else:
                color_pair = 3  # Default to generic binary
            # Vertical 9-hole display (each byte gets 1 char wide + 1 space)
            hole_pattern = self.format_9_hole_display(byte_val)
            for i, hole in enumerate(hole_pattern):
                tape_rows[i].append((f"{hole} ", color_pair))  # Store as (text, color) tuple

            # Marker row - show 'R' at start of RIM block, 'B' at start of BIN block
            if line_type == LineType.RIM and prev_line_type != LineType.RIM:
                marker_row.append(("R ", curses.A_REVERSE))  # Inverted 'R'
            elif line_type == LineType.BIN and prev_line_type != LineType.BIN:
                marker_row.append(("B ", curses.A_REVERSE))  # Inverted 'B'
            else:
                marker_row.append(("  ", 0))  # Empty space

            # FIODEC octal vertically (3 digits stacked)
            fiodec_oct = f"{byte_val:03o}"
            for fiodec_i, digit in enumerate(fiodec_oct):
                fiodec_rows[fiodec_i].append((f"{digit} ", color_pair))

            # FIODEC character vertically (max 3 chars stacked)
            char_display = fiodec_display[:3] if fiodec_display else "?"
            # Pad with spaces to ensure we have exactly 3 characters
            char_display = f"{char_display:<3}"
            for char_i, char in enumerate(char_display):
                if char_i < 3:  # Only use first 3 characters
                    char_rows[char_i].append((f"{char if char.strip() else ' '} ", color_pair))

            prev_line_type = line_type
        
        # Helper function to draw colored row
        def draw_colored_row(row_num: int, label: str, segments: List[Tuple[str, int]]):
            """Draw a row with colored segments"""
            col = 0
            stdscr.addstr(row_num, col, label)
            col += len(label)
            for text, color in segments:
                if col + len(text) > screen_width:
                    break
                try:
                    stdscr.addstr(row_num, col, text, curses.color_pair(color & 0xFF) | (color & ~0xFF))
                    col += len(text)
                except curses.error:
                    break

        # Display the rows
        try:
            # Header
            stdscr.addstr(0, 0, f"Paper Tape: {self.tape_file} | Position: {start_col}-{end_pos-1} | Total: {self.tape_width} lines")
            stdscr.addstr(1, 0, "Use ←→ arrows to scroll, Home/End to jump, Page Up/Down for fast scroll, Q to quit")

            # Vertical tape visualization (9 rows for holes)
            bit_labels = ["1", "2", "3", "s", "4", "5", "6", "7", "8"]  # s = sync
            row_num = 3
            for i, (tape_row, label) in enumerate(zip(tape_rows, bit_labels)):
                draw_colored_row(row_num + i, f"{label}: ", tape_row)

            # FIODEC octal (3 rows)
            row_num += 10  # Skip past holes + 1 blank
            draw_colored_row(row_num, "F: ", fiodec_rows[0])
            draw_colored_row(row_num + 1, "   ", fiodec_rows[1])
            draw_colored_row(row_num + 2, "   ", fiodec_rows[2])

            # FIODEC character (3 rows)
            row_num += 4  # Skip past FIODEC octal + 1 blank
            draw_colored_row(row_num, "C: ", char_rows[0])
            draw_colored_row(row_num + 1, "   ", char_rows[1])
            draw_colored_row(row_num + 2, "   ", char_rows[2])

            # Legend
            row_num += 4  # Skip past FIODEC char + 1 blank
            stdscr.addstr(row_num, 0, "Legend: ● = punched hole, ○ = unpunched, . = sync hole")
            stdscr.addstr(row_num + 1, 0, "Colors: Yellow=Leader, Bold Yellow=Label, Green=Alphanumeric")
            stdscr.addstr(row_num + 2, 0, "        Cyan=RIM format, Red=BIN format, White=Generic binary")
            
        except curses.error:
            # Handle screen too small
            pass
    
    def _init_colors(self):
        """Initialize color pairs for line types"""
        if curses.has_colors():
            curses.start_color()
            # Color pairs for line types
            curses.init_pair(1, curses.COLOR_YELLOW, curses.COLOR_BLACK)   # LEADER (yellow)
            curses.init_pair(2, curses.COLOR_YELLOW, curses.COLOR_BLACK)   # LABEL (bold yellow)
            curses.init_pair(3, curses.COLOR_WHITE, curses.COLOR_BLACK)    # BINARY (white)
            curses.init_pair(4, curses.COLOR_GREEN, curses.COLOR_BLACK)    # ALPHANUM (green)
            curses.init_pair(5, curses.COLOR_CYAN, curses.COLOR_BLACK)     # RIM (cyan)
            curses.init_pair(6, curses.COLOR_RED, curses.COLOR_BLACK)      # BIN (light red)

    def run(self, stdscr):
        """Main TUI loop"""
        # Setup curses
        curses.curs_set(0)  # Hide cursor
        stdscr.timeout(100)  # Non-blocking input
        self._init_colors()

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

    for byte_val, fiodec_char, fiodec_display, unicode_code, unicode_char, line_type in visible_data:
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

    print()
    print("Legend: ● = punched hole, ○ = unpunched, . = sync hole")
    print("F = FIODEC octal, C = FIODEC char")


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