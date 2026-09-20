// Wire protocol between the emulator's debugger server (ad1server.c) and its clients (ad1,
// fastload). Constants only: every frame is serialized field by field, so the emulator's PDP1
// struct layout is not part of the contract and a client never includes pdp1.h.
//
// All integers are unsigned 32-bit little-endian words except the 16-bit type and status.
// A frame is a 12-byte header and a payload:
//
//   offset 0  u32 length   payload bytes after the header, 0 to AD1P_MAX_REQUEST
//   offset 4  u16 type     a request type; a reply is the request type | AD1P_REPLY;
//                          an event is AD1P_EVENT_BASE or above
//   offset 6  u16 status   requests and events 0; replies one of the AD1P_ST_ codes
//   offset 8  u32 id       chosen by the client, nonzero, echoed in the reply; events 0
//
// One request is in flight at a time: a client sends a request and waits for its reply.
// The first frame on a connection must be AD1P_HELLO.
//
// Payloads (word counts, all u32 unless noted; "n" is a count field in the payload):
//
//   HELLO        req  client protocol, subscribe bits, name length, name bytes
//                rep  server protocol, memory words, breakpoint slots, watch slots,
//                     max request bytes, max reply bytes, build length, build text
//   PING         req  up to 64 bytes; rep the same bytes
//   GET_STATE    req  none; rep 22 words, see AD1P_STATE_ below
//   SET_REG      req  register, operation, value; rep none
//   READ_MEM     req  address, count; rep count, then count words
//   WRITE_MEM    req  flags, block count, then per block: address, count, count words
//                rep  was running, words written
//   START        req  address; rep run, pc
//   STOP         req  none; rep run, pc
//   CONTINUE     req  none; rep run
//   STEP         req  count, flags; rep steps done, end reason, pc, word at pc, record count,
//                     then record count pairs of (pc, word) taken before each step
//   CLEAR_SINGLE req  none; rep none
//   BP_SET       req  address, count; rep number (1 to AD1P_NUM_BREAKPOINTS)
//   BP_DELETE    req  number, 0 for all; rep none
//   BP_ENABLE    req  number; rep none
//   BP_DISABLE   req  number; rep none
//   BP_LIST      req  none; rep entry count, then per entry: set, enabled, number, address,
//                     count, current count
//   WATCH_SET    req  address, on any change (0 or 1), value; rep number
//   WATCH_DELETE, WATCH_ENABLE, WATCH_DISABLE   as the breakpoint ones
//   WATCH_LIST   req  none; rep entry count, then per entry: set, enabled, on any, number,
//                     address, value, last value
//   DISABLE_ALL  req  none; rep none
//   ACK_HIT      req  mask of AD1P_HIT_ bits; rep none
//   SET_POLICY   req  AD1P_POLICY_ value; rep none
//
// Events:
//
//   HIT_BREAK    number (1 to 8), address, word at address, count
//   HIT_WATCH    number, address, value now at address
//   RUN_STATE    run, pc; sent only to a client that subscribed in HELLO
#ifndef AD1PROTO_H
#define AD1PROTO_H

#define AD1P_VERSION 1

#define AD1P_HEADER_SIZE 12
#define AD1P_MAX_REQUEST (1024 * 1024)
#define AD1P_MAX_REPLY 262400
#define AD1P_MEM_WORDS (64 * 1024)
#define AD1P_NUM_BREAKPOINTS 8
#define AD1P_NUM_WATCHES 8
#define AD1P_MAX_RECORDS 16384      // (pc, word) pairs in one STEP reply
#define AD1P_MAX_PING 64

#define AD1P_DEFAULT_LOCAL_PORT 1044
#define AD1P_DEFAULT_REMOTE_PORT 1045

#define AD1P_REPLY 0x8000
#define AD1P_EVENT_BASE 0xF000

// Request types
#define AD1P_HELLO 0x0001
#define AD1P_PING 0x0002
#define AD1P_GET_STATE 0x0010
#define AD1P_SET_REG 0x0011
#define AD1P_READ_MEM 0x0020
#define AD1P_WRITE_MEM 0x0021
#define AD1P_START 0x0030
#define AD1P_STOP 0x0031
#define AD1P_CONTINUE 0x0032
#define AD1P_STEP 0x0033
#define AD1P_CLEAR_SINGLE 0x0034
#define AD1P_BP_SET 0x0040
#define AD1P_BP_DELETE 0x0041
#define AD1P_BP_ENABLE 0x0042
#define AD1P_BP_DISABLE 0x0043
#define AD1P_BP_LIST 0x0044
#define AD1P_WATCH_SET 0x0050
#define AD1P_WATCH_DELETE 0x0051
#define AD1P_WATCH_ENABLE 0x0052
#define AD1P_WATCH_DISABLE 0x0053
#define AD1P_WATCH_LIST 0x0054
#define AD1P_DISABLE_ALL 0x0060
#define AD1P_ACK_HIT 0x0061
#define AD1P_SET_POLICY 0x0062

// Event types
#define AD1P_EVT_HIT_BREAK 0xF001
#define AD1P_EVT_HIT_WATCH 0xF002
#define AD1P_EVT_RUN_STATE 0xF003

// Status codes in a reply header
#define AD1P_ST_OK 0
#define AD1P_ST_BAD_REQUEST 1       // unknown type or a payload of the wrong size
#define AD1P_ST_BAD_ARG 2           // a value out of range; nothing was changed
#define AD1P_ST_BAD_STATE 3         // needs the machine stopped, or powered on
#define AD1P_ST_VERSION 4           // protocol mismatch; the payload is a text message
#define AD1P_ST_BUSY 5              // another client is connected; the payload is a text message
#define AD1P_ST_TIMEOUT 6           // an operation did not finish; the normal payload follows
#define AD1P_ST_TOOBIG 7            // frame too large
#define AD1P_ST_NOT_SET 8           // breakpoint or watch is not set
#define AD1P_ST_ALREADY 9           // already enabled or already disabled
#define AD1P_ST_NO_SLOT 10          // no free breakpoint or watch entry

// HELLO subscribe bits
#define AD1P_SUB_RUN_STATE 0x1

// SET_REG registers and operations
#define AD1P_REG_AC 1
#define AD1P_REG_IO 2
#define AD1P_REG_PC 3
#define AD1P_REG_PF 5
#define AD1P_OP_ASSIGN 0
#define AD1P_OP_OR 1
#define AD1P_OP_CLEAR 2

// WRITE_MEM flags
#define AD1P_WRITE_STOP_FIRST 0x1

// STEP flags and end reasons
#define AD1P_STEP_RECORD 0x1
#define AD1P_END_DONE 0
#define AD1P_END_HIT 1
#define AD1P_END_TIMEOUT 2

// ACK_HIT mask
#define AD1P_HIT_BREAK 0x1
#define AD1P_HIT_WATCH 0x2

// SET_POLICY: what the server does with the breakpoint and watch tables when the client goes
#define AD1P_POLICY_KEEP 0
#define AD1P_POLICY_DELETE_ALL 1
#define AD1P_POLICY_DISABLE_ALL 2

// GET_STATE reply word indexes
#define AD1P_STATE_AC 0
#define AD1P_STATE_IO 1
#define AD1P_STATE_PC 2             // full address, extension bits included
#define AD1P_STATE_TW 3
#define AD1P_STATE_PF 4
#define AD1P_STATE_SS 5
#define AD1P_STATE_TA 6
#define AD1P_STATE_MA 7
#define AD1P_STATE_MB 8
#define AD1P_STATE_EXD 9
#define AD1P_STATE_RUN 10
#define AD1P_STATE_RUN_ENABLE 11
#define AD1P_STATE_POWER 12
#define AD1P_STATE_SINGLE 13        // the sticky single-step state is set
#define AD1P_STATE_BRK_HIT 14
#define AD1P_STATE_BRK_NO 15        // 0-based
#define AD1P_STATE_WATCH_HIT 16
#define AD1P_STATE_WATCH_NO 17      // 0-based
#define AD1P_STATE_BRK_ENABLED 18
#define AD1P_STATE_WATCH_ENABLED 19
#define AD1P_STATE_DROPPED 20       // events dropped because the queue was full
#define AD1P_STATE_WORD_AT_PC 21
#define AD1P_STATE_WORDS 22

#endif
