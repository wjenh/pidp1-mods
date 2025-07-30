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
	"time"

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
		wsConn:     make(chan *websocket.Conn, 1),
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
	var data []byte
	var conn net.Conn
	var err error

	for {
		select {
		case data = <-s.readerData:
			if conn != nil {
				conn.Close()
			}
			if data == nil {
				log.Printf("Reader unmounted")
				continue
			}

			log.Printf("Reader mounted with %d bytes", len(data))

			conn, err = net.Dial("tcp", fmt.Sprintf("%s:1042", s.host))
			if err != nil {
				log.Printf("Failed to connect to reader: %v", err)
				continue
			}
			log.Printf("Connected to reader port 1042")

			// find beginning
			pos := 0
			for pos < len(data) && data[pos] == 0 {
				pos++
			}
			if pos > 20 {
				pos -= 10
			}
			s.sendToWeb(Message{
				Type:     "reader_position",
				Position: pos,
			})

			go s.readLoop(conn, data, pos)
		}
	}
}

func (s *PeriphServer) handlePunch() {
	buf := make([]byte, 1)
	for {
		conn, err := net.Dial("tcp", fmt.Sprintf("%s:1043", s.host))
		if err != nil {
			log.Printf("Failed to connect to punch: %v", err)
			time.Sleep(5 * time.Second)
			continue
		}

		log.Printf("Connected to punch port 1043")
		for {
			_, err := conn.Read(buf)
			if err != nil {
				log.Printf("Error reading from punch: %v", err)
				break
			}
			s.sendToWeb(Message{
				Type:  "punch_data",
				Value: buf[0],
			})
		}
		log.Printf("close punch")

		conn.Close()
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

func (s *PeriphServer) handleTypewriter() {
	buf := make([]byte, 1)
	dataChan := make(chan byte, 1)
	errChan := make(chan error, 1)
	for {
		conn, err := net.Dial("tcp", fmt.Sprintf("%s:1041", s.host))
		if err != nil {
			log.Printf("Failed to connect to typewriter: %v", err)
			time.Sleep(5 * time.Second)
			continue
		}

		log.Printf("Connected to typewriter")

		go readBytes(conn, dataChan, errChan)
	L:
		for {
			select {
			case c := <-dataChan:
				//				fmt.Printf("got char xxx %v\n", c)
				s.sendToWeb(Message{
					Type:  "char",
					Value: c,
				})

			case c := <-s.typData:
				//				fmt.Printf("typed: %v\n", c)
				buf[0] = c
				_, err := conn.Write(buf)
				if err != nil {
					log.Printf("Error writing to typewriter: %v", err)
					break L
				}

			case err := <-errChan:
				fmt.Printf("got err %v\n", err)
				break L
			}
		}
		log.Printf("close typewriter")

		conn.Close()
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
	defer func() {
		<-s.wsConn

		s.readerData <- nil
		s.dpyCtl <- 0
	}()

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

func main() {
	server := NewServer("localhost")

	go server.handleReader()
	go server.handlePunch()
	go server.handleDisplay()
	go server.handleTypewriter()

	http.HandleFunc("/ws", server.handleWebSocket)
	http.Handle("/", http.FileServer(http.Dir("srv")))

	log.Fatal(http.ListenAndServe(":8080", nil))
}
