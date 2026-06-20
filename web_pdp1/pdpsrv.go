package main

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"io/ioutil"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"strings"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool {
		return true // Allow all origins for development
	},
}

type Message struct {
	Type string `json:"type"`
	Err  string `json:"err"`
	// emulator command
	Cmd string `json:"cmd"`
	// reader
	Data     []byte `json:"data,omitempty"`
	Position int    `json:"position,omitempty"`
	// punch, typewriter
	Value byte `json:"value,omitempty"`
	// display
	Points []uint32 `json:"points,omitempty"`
	// assembly
	Source  string `json:"source"`
	Listing string `json:"listing"`
	RIM     []byte `json:"rim"`
}

type PeriphServer struct {
	host string

	readerData chan []byte
	punchData  chan byte
	dpyCtl     chan int
	typData    chan byte
	// 20-Jun-2026 wje: typCtl/punchCtl gate the port-1041/1043 connections the
	// same way dpyCtl already gates the display connection -- see
	// handleTypewriter()/handlePunch() and the matching connect/cleanup
	// handling in handleWebSocket().
	typCtl   chan int
	punchCtl chan int

	ptrbuf []byte
	ptrpos int

	// active connection is stored in channel
	// for synchronization
	wsConn chan *websocket.Conn
}

func NewServer(host string) *PeriphServer {
	return &PeriphServer{
		host:       host,
		readerData: make(chan []byte, 1),
		punchData:  make(chan byte, 1000),
		dpyCtl:     make(chan int, 1),
		typData:    make(chan byte, 1),
		typCtl:     make(chan int, 1),
		punchCtl:   make(chan int, 1),
		wsConn:     make(chan *websocket.Conn, 1),
		ptrbuf:     nil,
	}
}

func assemble(src string) (string, []byte, string) {
	//	fmt.Printf("src: <%s>\n", src)

	tmpDir, err := ioutil.TempDir("", "macro1_")
	if err != nil {
		return "", nil, fmt.Sprintf("couldn't create tmp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	baseName := "source"
	sourcePath := fmt.Sprintf("%s/%s.mac", tmpDir, baseName)

	outFile, err := os.Create(sourcePath)
	if err != nil {
		return "", nil, fmt.Sprintf("couldn't create file: %v", err)
	}

	if _, err = io.Copy(outFile, strings.NewReader(src)); err != nil {
		outFile.Close()
		return "", nil, fmt.Sprintf("couldn't write file: %v", err)
	}
	outFile.Close()

	cmd := exec.Command("/usr/local/bin/macro1", sourcePath)
	err = cmd.Run()

	errf, errxx := ioutil.ReadFile(fmt.Sprintf("%s/%s.err", tmpDir, baseName))

	if err != nil {
		if errxx == nil {
			return "", nil, string(errf)
		} else {
			return "", nil, fmt.Sprintf("macro1: %v", err)
		}
	}

	lst, err := ioutil.ReadFile(fmt.Sprintf("%s/%s.lst", tmpDir, baseName))
	if err != nil {
		return "", nil, fmt.Sprintf("couldn't open listing: %v", err)
	}

	rim, err := ioutil.ReadFile(fmt.Sprintf("%s/%s.rim", tmpDir, baseName))
	if err != nil {
		return "", nil, fmt.Sprintf("couldn't open RIM file: %v", err)
	}

	return string(lst), rim, ""
}

func (s *PeriphServer) emuCommand(cmd string) (string, error) {
	conn, err := net.Dial("tcp", fmt.Sprintf("%s:1040", s.host))
	if err != nil {
		return "", fmt.Errorf("error connecting to emulator: %v", err)
	}
	defer conn.Close()

	if _, err := fmt.Fprintf(conn, "%s\n", cmd); err != nil {
		return "", fmt.Errorf("error sending command: %v", err)
	}

	reader := bufio.NewReader(conn)
	response, err := reader.ReadString('\n')
	if err != nil && err != io.EOF {
		return "", fmt.Errorf("error reading response: %v", err)
	}

	return strings.TrimSpace(response), nil
}

func (s *PeriphServer) readLoop(conn net.Conn, data []byte, startPos int) {
	defer conn.Close()

	pos := startPos
	buf := make([]byte, 1)

	for pos < len(data) {
		buf[0] = data[pos]
		pos++

		_, err := conn.Write(buf)
		if err != nil {
			log.Printf("Error writing to reader: %v", err)
			return
		}

		_, err = conn.Read(buf)
		if err != nil {
			log.Printf("Error reading from reader: %v", err)
			return
		}

		s.sendToWeb(Message{
			Type:     "reader_position",
			Position: pos,
		})
	}
	log.Printf("Finished reading tape")
}

func (s *PeriphServer) handleReader() {
	var conn net.Conn
	var err error

	for {
		select {
		case s.ptrbuf = <-s.readerData:
			if conn != nil {
				conn.Close()
			}
			if s.ptrbuf == nil {
				log.Printf("Reader unmounted")
				continue
			}

			log.Printf("Reader mounted with %d bytes", len(s.ptrbuf))

			conn, err = net.Dial("tcp", fmt.Sprintf("%s:1042", s.host))
			if err != nil {
				log.Printf("Failed to connect to reader: %v", err)
				continue
			}
			log.Printf("Connected to reader port 1042")

			s.ptrpos = 0
			for s.ptrpos < len(s.ptrbuf) && s.ptrbuf[s.ptrpos] == 0 {
				s.ptrpos++
			}
			if s.ptrpos > 20 {
				s.ptrpos -= 10
			}
			s.sendToWeb(Message{
				Type:     "reader_mounted",
				Position: s.ptrpos,
				Data: s.ptrbuf,
			})

			go s.readLoop(conn, s.ptrbuf, s.ptrpos)
		}
	}
}

// 20-Jun-2026 wje: same fix as handleTypewriter() below, for the identical
// "TODO: we should only connect to the emulator when the websocket is
// connected" this function used to have. Driven by punchCtl exactly like
// typCtl/dpyCtl -- only connects to port 1043 while a browser is attached,
// instead of dialing unconditionally at startup and retrying every 5 seconds
// forever regardless of whether anyone's watching. main.c's handleptp()
// replaces pdp->p_fd outright on each new connection (closing whatever was
// there before), so this connection is effectively exclusive the same way the
// typewriter's is -- no reason for pdpsrv to hold/steal it when no browser is
// open.
func (s *PeriphServer) handlePunch() {
	var conn net.Conn
	dataChan := make(chan byte, 1)
	errChan := make(chan error, 1)

	for {
		select {
		case msg := <-s.punchCtl:
			if conn != nil {
				conn.Close()
				conn = nil
			}
			if msg == 0 {
				continue
			}

			c, err := net.Dial("tcp", fmt.Sprintf("%s:1043", s.host))
			if err != nil {
				log.Printf("Failed to connect to punch: %v", err)
				continue
			}

			log.Printf("Connected to punch port 1043")
			conn = c
			go readBytes(conn, dataChan, errChan)

		case c := <-dataChan:
			s.sendToWeb(Message{
				Type:  "punch_data",
				Value: c,
			})

		case err := <-errChan:
			log.Printf("punch connection lost: %v", err)
			if conn != nil {
				conn.Close()
				conn = nil
			}
		}
	}
}

func (s *PeriphServer) displayCon(conn net.Conn) {
	defer conn.Close()

	buf := make([]byte, 128*4)
	cmds := make([]uint32, 128)
	for {
		n, err := conn.Read(buf)
		if err != nil {
			log.Printf("display: read error: %v\n", err)
			s.sendToWeb(Message{Type: "dpy_disconnected"})
			return
		}

		ncmds := n / 4
		for i := 0; i < ncmds; i++ {
			cmds[i] = uint32(binary.LittleEndian.Uint32(buf[i*4 : (i+1)*4]))
		}
		s.sendToWeb(Message{
			Type:   "points",
			Points: cmds[:ncmds],
		})
	}
}

func (s *PeriphServer) handleDisplay() {
	var msg int
	var conn net.Conn
	var err error

	for {
		select {
		case msg = <-s.dpyCtl:
			if conn != nil {
				s.sendToWeb(Message{Type: "dpy_disconnected"})
				conn.Close()
			}
			if msg == 0 {
				continue
			}
			conn, err = net.Dial("tcp", fmt.Sprintf("%s:3400", s.host))
			if err != nil {
				log.Printf("Failed to connect to display: %v", err)
				continue
			}

			s.sendToWeb(Message{Type: "dpy_connected"})

			go s.displayCon(conn)
		}
	}
}

func readBytes(conn net.Conn, data chan byte, errCh chan error) {
	buf := make([]byte, 1)
	for {
		n, err := conn.Read(buf)
		if err != nil {
			errCh <- err
			log.Printf("returning from readBytes\n")
			return
		}
		if n > 0 {
			data <- buf[0]
		}
	}
}

// 20-Jun-2026 wje: typtelnet.c's telthread() sends real telnet option-negotiation
// sequences (IAC WILL/WONT/DO/DONT <option>, 3 bytes each) at the start of every
// connection to port 1041 -- a real telnet client (telnet(1), PuTTY) consumes
// these as protocol negotiation and never displays them. pdpsrv used readBytes()
// (above) for the typewriter too, which has no idea what telnet is and just
// relays every raw byte to the browser -- including the negotiation bytes. Most
// of those are invisible control characters and went unnoticed, but the
// LINEEDIT option's value happens to be 34 (ASCII '"'), sent twice (WONT and
// DONT LINEEDIT), and the browser's byte decoder (index.html's processbyte())
// renders any plain byte <128 with its top bit clear as a literal character --
// hence two stray `"` characters appearing every time a browser opens the
// typewriter connection, never on a direct telnet connection. Fixed by giving
// the typewriter (only -- not punch, which has no telnet wrapper at all, see
// main.c's handleptp()) its own reader that strips IAC sequences before they
// ever reach the data channel, the same way typtelnet.c's own readiac() strips
// them in the other direction.
func readTypewriterBytes(conn net.Conn, data chan byte, errCh chan error) {
	buf := make([]byte, 1)
	const (
		iacWill = 251
		iacWont = 252
		iacDo   = 253
		iacDont = 254
		iacIAC  = 255
	)
	// state: 0 = normal, 1 = just saw IAC, 2 = saw IAC + WILL/WONT/DO/DONT,
	// expecting the option byte next.
	state := 0
	for {
		n, err := conn.Read(buf)
		if err != nil {
			errCh <- err
			log.Printf("returning from readTypewriterBytes\n")
			return
		}
		if n == 0 {
			continue
		}
		c := buf[0]

		switch state {
		case 0:
			if c == iacIAC {
				state = 1
				continue
			}
			data <- c

		case 1:
			switch c {
			case iacWill, iacWont, iacDo, iacDont:
				state = 2
			case iacIAC:
				// IAC IAC is telnet's escape for a literal 0xFF data byte.
				data <- c
				state = 0
			default:
				// other single-byte telnet commands (NOP, etc.) -- nothing
				// further to consume, just drop and resume.
				state = 0
			}

		case 2:
			// the option byte for WILL/WONT/DO/DONT -- discard it and resume.
			state = 0
		}
	}
}

// 20-Jun-2026 wje: rewritten to fix the "TODO: we should only connect to the
// emulator when the websocket is connected" left by the original implementation.
// The typewriter is an exclusive-use device on the emulator side (port 1041 only
// ever accepts one connection at a time -- see src/blincolnlights/common.c's
// serve1(), which closes its listening socket the moment it accepts one client).
// The old version of this function dialed port 1041 unconditionally from
// main(), the instant pdpsrv started, and retried every 5 seconds forever --
// completely independent of whether a browser was ever open. That meant pdpsrv
// permanently held (or was constantly trying to re-grab) the device's one
// connection slot, so a direct `telnet`/PuTTY session to port 1041 would almost
// always lose the race and get connection refused, even right after restarting
// the emulator with no browser open at all.
//
// Now this is driven entirely by s.typCtl, exactly mirroring how handleDisplay()
// is driven by s.dpyCtl: handleWebSocket() sends typCtl<-1 when a browser
// connects and typCtl<-0 when it disconnects (see below), so pdpsrv only holds
// the port-1041 slot while a browser tab actually has the page open -- freeing
// it the rest of the time for direct telnet/PuTTY access, and reclaiming it
// (closing out from under) a direct telnet session if a browser connects while
// one is active, matching "exclusive use, whoever's connected has it."
func (s *PeriphServer) handleTypewriter() {
	var conn net.Conn
	dataChan := make(chan byte, 1)
	errChan := make(chan error, 1)
	buf := make([]byte, 1)

	for {
		select {
		case msg := <-s.typCtl:
			if conn != nil {
				conn.Close()
				conn = nil
			}
			if msg == 0 {
				continue
			}

			c, err := net.Dial("tcp", fmt.Sprintf("%s:1041", s.host))
			if err != nil {
				log.Printf("Failed to connect to typewriter: %v", err)
				continue
			}

			log.Printf("Connected to typewriter")
			conn = c
			go readTypewriterBytes(conn, dataChan, errChan)

		case c := <-dataChan:
			s.sendToWeb(Message{
				Type:  "char",
				Value: c,
			})

		case c := <-s.typData:
			if conn == nil {
				// no active connection (no browser attached, or it raced with
				// a disconnect) -- nothing to send the keystroke to.
				continue
			}
			buf[0] = c
			_, err := conn.Write(buf)
			if err != nil {
				log.Printf("Error writing to typewriter: %v", err)
				conn.Close()
				conn = nil
			}

		case err := <-errChan:
			log.Printf("typewriter connection lost: %v", err)
			if conn != nil {
				conn.Close()
				conn = nil
			}
		}
	}
}

func (s *PeriphServer) sendToWeb(msg Message) {
	select {
	case ws := <-s.wsConn:
		data, _ := json.Marshal(msg)
		err := ws.WriteMessage(websocket.TextMessage, data)
		if err != nil {
			log.Printf("Error sending to web: %v", err)
		}
		s.wsConn <- ws

	default:
		// not connected
	}
}

func (s *PeriphServer) handleWebSocket(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("Failed to upgrade connection: %v", err)
		return
	}
	defer conn.Close()

	log.Printf("WebSocket connected")

	s.wsConn <- conn
	// 20-Jun-2026 wje: claim the typewriter's and punch's exclusive slots for the
	// duration of this browser session; released in the deferred cleanup below.
	s.typCtl <- 1
	s.punchCtl <- 1
	defer func() {
		<-s.wsConn

		//s.readerData <- nil
		s.dpyCtl <- 0
		s.typCtl <- 0
		s.punchCtl <- 0
	}()

	if s.ptrbuf != nil {
		s.sendToWeb(Message{
			Type:     "reader_mounted",
			Position: s.ptrpos,
			Data: s.ptrbuf,
		})
	}

	for {
		var msg Message
		err := conn.ReadJSON(&msg)
		if err != nil {
			log.Printf("Error reading message: %v", err)
			break
		}

		switch msg.Type {
		case "mount_reader":
			log.Printf("Mounting reader with %d bytes", len(msg.Data))
			s.readerData <- msg.Data

		case "unmount_reader":
			s.readerData <- nil

		case "connect_dpy":
			s.dpyCtl <- 1

		case "disconnect_dpy":
			s.dpyCtl <- 0

		case "key":
			// may not be connected and not receiving from chan
			select {
			case s.typData <- msg.Value:
			default:
				log.Printf("channel full\n")
			}

		case "cmd":
			reply, _ := s.emuCommand(msg.Cmd)
			s.sendToWeb(Message{
				Type: "reply",
				Cmd:  reply,
			})

		case "assemble":
			lst, rim, err := assemble(msg.Source)
			s.sendToWeb(Message{
				Type:    "assembly",
				Listing: lst,
				RIM:     rim,
				Err:     err,
			})
		}
	}
}

// do we really have to do this manually?
func tokenize(input string) []string {
	var tokens []string
	remaining := strings.TrimSpace(input)

	for len(remaining) > 0 {
		quoteIdx := strings.IndexAny(remaining, `"'`)

		if quoteIdx == -1 {
			tokens = append(tokens, strings.Fields(remaining)...)
			break
		}

		if quoteIdx > 0 {
			before := remaining[:quoteIdx]
			tokens = append(tokens, strings.Fields(before)...)
		}

		quoteChar := remaining[quoteIdx]
		closeIdx := strings.Index(remaining[quoteIdx+1:], string(quoteChar))
		if closeIdx == -1 {
			tokens = append(tokens, remaining[quoteIdx+1:])
			break
		}

		quotedContent := remaining[quoteIdx+1 : quoteIdx+1+closeIdx]
		tokens = append(tokens, quotedContent)

		remaining = strings.TrimSpace(remaining[quoteIdx+1+closeIdx+1:])
	}

	return tokens
}

func (s *PeriphServer) doCLI(conn net.Conn) {
	defer conn.Close()

	scanner := bufio.NewScanner(conn)
	fmt.Printf("waiting for input\n")
	for scanner.Scan() {
		line := scanner.Text()
		toks := tokenize(line)
		fmt.Printf("tokens: %d %v\n", len(toks), toks)
		switch toks[0] {
		case "r":
			if len(toks) < 2 {
				s.readerData <- nil
				break
			}
			content, err := ioutil.ReadFile(toks[1])
			if err != nil {
				log.Println("Error reading file:", err)
				s.readerData <- nil
			} else {
				fmt.Printf("File size: %d bytes\n", len(content))
				s.readerData <- content
			}

		case "p":
			fmt.Printf("file %s\n", toks[1])
		}
	}
	fmt.Printf("ending connection\n")
}

func (s *PeriphServer) handleCLI() {
	listener, err := net.Listen("tcp", ":1050")
	if err != nil {
		log.Fatal("Failed to listen on port 1050:", err)
	}
	defer listener.Close()

	for {
	fmt.Printf("accepting connection\n")
		conn, err := listener.Accept()
		if err != nil {
			log.Println("Failed to accept connection:", err)
			continue
		}
		s.doCLI(conn)
	}
}

func main() {
	server := NewServer("localhost")

	go server.handleReader()
	go server.handlePunch()
	go server.handleDisplay()
	go server.handleTypewriter()
	go server.handleCLI()

	http.HandleFunc("/ws", server.handleWebSocket)
	http.Handle("/", http.FileServer(http.Dir("srv")))

	log.Fatal(http.ListenAndServe(":8080", nil))
}
