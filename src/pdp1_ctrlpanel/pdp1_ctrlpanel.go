package main

import (
    "bufio"
    "encoding/json"
    "fmt"
    "io"
    "io/ioutil"
    "net"
    "net/http"
    "os"
    "os/exec"
    "strings"
)

const (
    readerPath = "/tmp/reader"
    punchPath  = "/tmp/punch"
    emulatorPort = "1040"
)

// Common response writer for JSON responses
func writeJSON(w http.ResponseWriter, data interface{}) {
    w.Header().Set("Content-Type", "application/json")
    json.NewEncoder(w).Encode(data)
}

// Common error response writer
func writeError(w http.ResponseWriter, err error, status int) {
    if status >= 500 {
        fmt.Printf("Server error: %v\n", err)
    }
    http.Error(w, err.Error(), status)
}

func sendToEmulator(cmd string) (string, error) {
    conn, err := net.Dial("tcp", "localhost:"+emulatorPort)
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

// Reader handlers
func handleReaderMount(w http.ResponseWriter, r *http.Request) {
    if r.Method != "POST" {
        writeError(w, fmt.Errorf("method not allowed"), http.StatusMethodNotAllowed)
        return
    }

    file, _, err := r.FormFile("tape")
    if err != nil {
        writeError(w, fmt.Errorf("error reading file: %v", err), http.StatusBadRequest)
        return
    }
    defer file.Close()

    out, err := os.Create(readerPath)
    if err != nil {
        writeError(w, fmt.Errorf("error creating file: %v", err), http.StatusInternalServerError)
        return
    }

    if _, err = io.Copy(out, file); err != nil {
        out.Close()
        writeError(w, fmt.Errorf("error copying file: %v", err), http.StatusInternalServerError)
        return
    }
    out.Close()

    response, err := sendToEmulator(fmt.Sprintf("r %s", readerPath))
    if err != nil {
        writeError(w, fmt.Errorf("error mounting tape: %v", err), http.StatusInternalServerError)
        return
    }

    w.Header().Set("Content-Type", "text/plain")
    w.Write([]byte(response))
}

func handleReaderUnmount(w http.ResponseWriter, r *http.Request) {
    if r.Method != "POST" {
        writeError(w, fmt.Errorf("method not allowed"), http.StatusMethodNotAllowed)
        return
    }

    response, err := sendToEmulator("r")
    if err != nil {
        writeError(w, fmt.Errorf("error unmounting tape: %v", err), http.StatusInternalServerError)
        return
    }

    w.Header().Set("Content-Type", "text/plain")
    w.Write([]byte(response))
}

// Punch handlers
func initPunch() error {
    response, err := sendToEmulator(fmt.Sprintf("p %s", punchPath))
    if err != nil {
        return fmt.Errorf("error initializing punch: %v", err)
    }

    if !strings.Contains(response, "ok") {
        return fmt.Errorf("unexpected response: %s", response)
    }
    return nil
}

func handlePunchInit(w http.ResponseWriter, r *http.Request) {
    if r.Method != "POST" {
        writeError(w, fmt.Errorf("method not allowed"), http.StatusMethodNotAllowed)
        return
    }

    if err := initPunch(); err != nil {
        writeError(w, err, http.StatusInternalServerError)
        return
    }

    w.Header().Set("Content-Type", "text/plain")
    w.Write([]byte("ok"))
}

func handlePunchGet(w http.ResponseWriter, r *http.Request) {
    if r.Method != "GET" {
        writeError(w, fmt.Errorf("method not allowed"), http.StatusMethodNotAllowed)
        return
    }

    punchData, err := ioutil.ReadFile(punchPath)
    if err != nil {
        writeError(w, fmt.Errorf("error reading punch file: %v", err), http.StatusInternalServerError)
        return
    }

    w.Header().Set("Content-Type", "application/octet-stream")
    w.Header().Set("Content-Disposition", `attachment; filename="punched_tape.bin"`)
    w.Header().Set("Content-Length", fmt.Sprintf("%d", len(punchData)))
    w.Write(punchData)
}

// Hardware option handlers
func handleMulDivQuery(w http.ResponseWriter, r *http.Request) {
    if r.Method != "GET" {
        writeError(w, fmt.Errorf("method not allowed"), http.StatusMethodNotAllowed)
        return
    }

    response, err := sendToEmulator("muldiv ?")
    if err != nil {
        writeError(w, fmt.Errorf("error querying mul-div state: %v", err), http.StatusInternalServerError)
        return
    }

    w.Header().Set("Content-Type", "text/plain")
    w.Write([]byte(response))
}

func handleMulDivSet(w http.ResponseWriter, r *http.Request) {
    if r.Method != "POST" {
        writeError(w, fmt.Errorf("method not allowed"), http.StatusMethodNotAllowed)
        return
    }

    state := r.FormValue("state")
    if state != "on" && state != "off" {
        writeError(w, fmt.Errorf("invalid state"), http.StatusBadRequest)
        return
    }

    response, err := sendToEmulator(fmt.Sprintf("muldiv %s", state))
    if err != nil {
        writeError(w, fmt.Errorf("error setting mul-div state: %v", err), http.StatusInternalServerError)
        return
    }

    w.Header().Set("Content-Type", "text/plain")
    w.Write([]byte(response))
}

// Assembler handlers
func handleAssemble(w http.ResponseWriter, r *http.Request) {
    if r.Method != "POST" {
        writeJSON(w, map[string]string{"error": "Method not allowed"})
        return
    }

    w.Header().Set("Content-Type", "application/json")
    
    file, header, err := r.FormFile("source")
    if err != nil {
        writeJSON(w, map[string]string{"error": fmt.Sprintf("Error reading file: %v", err)})
        return
    }
    defer file.Close()

    tmpDir, err := ioutil.TempDir("", "macro1_")
    if err != nil {
        writeJSON(w, map[string]string{"error": fmt.Sprintf("Error creating temp directory: %v", err)})
        return
    }
    defer os.RemoveAll(tmpDir)

    baseName := strings.TrimSuffix(header.Filename, ".mac")
    sourcePath := fmt.Sprintf("%s/%s.mac", tmpDir, baseName)
    
    outFile, err := os.Create(sourcePath)
    if err != nil {
        writeJSON(w, map[string]string{"error": fmt.Sprintf("Error saving source file: %v", err)})
        return
    }

    if _, err = io.Copy(outFile, file); err != nil {
        outFile.Close()
        writeJSON(w, map[string]string{"error": fmt.Sprintf("Error copying file: %v", err)})
        return
    }
    outFile.Close()

    cmd := exec.Command("/usr/local/bin/macro1", sourcePath)
    err = cmd.Run()

    rimPath := fmt.Sprintf("%s/%s.rim", tmpDir, baseName)
    lstPath := fmt.Sprintf("%s/%s.lst", tmpDir, baseName)

    response := map[string]string{
        "output": "Assembly completed",
    }

    if _, err := os.Stat(rimPath); err == nil {
        if err := os.Rename(rimPath, fmt.Sprintf("/tmp/%s.rim", baseName)); err != nil {
            response["error"] = fmt.Sprintf("Error moving RIM file: %v", err)
        }
    } else {
        response["error"] = "Assembly failed: no RIM file produced"
    }

    if _, err := os.Stat(lstPath); err == nil {
        if err := os.Rename(lstPath, fmt.Sprintf("/tmp/%s.lst", baseName)); err != nil {
            if response["error"] != "" {
                response["error"] += fmt.Sprintf("\nError moving LST file: %v", err)
            }
        }
    }

    writeJSON(w, response)
}

func handleGetLst(w http.ResponseWriter, r *http.Request) {
    if r.Method != "GET" {
        writeError(w, fmt.Errorf("method not allowed"), http.StatusMethodNotAllowed)
        return
    }

    filename := r.URL.Query().Get("file")
    if filename == "" {
        writeError(w, fmt.Errorf("no filename specified"), http.StatusBadRequest)
        return
    }

    content, err := ioutil.ReadFile(fmt.Sprintf("/tmp/%s.lst", filename))
    if err != nil {
        writeError(w, fmt.Errorf("error reading LST file: %v", err), http.StatusInternalServerError)
        return
    }

    w.Header().Set("Content-Type", "text/plain")
    w.Write(content)
}

func handleGetRim(w http.ResponseWriter, r *http.Request) {
    if r.Method != "GET" {
        writeError(w, fmt.Errorf("method not allowed"), http.StatusMethodNotAllowed)
        return
    }

    filename := r.URL.Query().Get("file")
    if filename == "" {
        writeError(w, fmt.Errorf("no filename specified"), http.StatusBadRequest)
        return
    }

    content, err := ioutil.ReadFile(fmt.Sprintf("/tmp/%s.rim", filename))
    if err != nil {
        writeError(w, fmt.Errorf("error reading RIM file: %v", err), http.StatusInternalServerError)
        return
    }

    w.Header().Set("Content-Type", "application/octet-stream")
    w.Header().Set("Content-Disposition", fmt.Sprintf(`attachment; filename="%s.rim"`, filename))
    w.Write(content)
}

func main() {
    mux := http.NewServeMux()

    // Static files
    mux.Handle("/", http.FileServer(http.Dir("static")))
    
    // Reader endpoints
    mux.HandleFunc("/api/reader/mount", handleReaderMount)
    mux.HandleFunc("/api/reader/unmount", handleReaderUnmount)
    
    // Punch endpoints
    mux.HandleFunc("/api/punch/init", handlePunchInit)
    mux.HandleFunc("/api/punch/get", handlePunchGet)
    
    // Hardware options endpoints
    mux.HandleFunc("/api/muldiv/query", handleMulDivQuery)
    mux.HandleFunc("/api/muldiv/set", handleMulDivSet)
    
    // Assembler endpoints
    mux.HandleFunc("/api/assemble", handleAssemble)
    mux.HandleFunc("/api/lst", handleGetLst)
    mux.HandleFunc("/api/rim", handleGetRim)

    fmt.Println("Server starting on :8080")
    http.ListenAndServe(":8080", mux)
}
