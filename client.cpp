// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include "helper.h"

//additional headers
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

const int CLIENT_TIMEOUT_SEC = 30;        // give up if the load balancer sends nothing for this long
const int MAX_NAME_LEN = 1024;            // sanity limit for a name stored in a .tdbg file
const int64_t HEADER_SIZE = 20;           // 4 magic + 4 version + 4 stepCount + 8 indexOffset


// These two must be the same as in Phase 1

const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader 
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// ---- Structs only the client needs ----
struct TdbgReader
{
    FILE* file = nullptr;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.
    
    size_t rec = 0;
    while(rec < n){
      ssize_t r = recv(sock, buf + rec, n - rec, 0);
      if(r<=0){
        return false;
      }
      rec += r;
    }
    return true;
}
bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
    
    size_t sent = 0;
    while(sent < n){
      ssize_t r = send(sock, buf + sent, n - sent, MSG_NOSIGNAL);
      if(r<=0){
        return false;
      }
      sent += r;
    }
    return true;  
}

int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1 ||
        connect(sock, (sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}
bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
    
    FILE *f = fopen(sourcePath, "rb");
    if (!f)
        return false;
 
    Crc32 crc;
    char status = 0;
    crc.update(&status, 1);
    bool ok = sendAll(sock, &status, 1);
 
    char buf[IO_BUFFER_SIZE];
    while (ok) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0)
            break;
        crc.update(buf, n);
        ok = sendAll(sock, buf, n);
    }
    if (ferror(f))
        ok = false;
    fclose(f);
 
    if (ok) {
        unsigned char trailer[4];
        crcToBytes(crc.value(), trailer);
        ok = sendAll(sock, (const char *)trailer, 4);
    }
    if (ok)
        shutdown(sock, SHUT_WR);
    return ok;
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
    
    TrailerChecker checker;
    FILE* out = nullptr;
    int status = -1;
    string errorText;
    size_t received = 0;
    bool failed = false;
    char buf[IO_BUFFER_SIZE];
    
    while(true){
        ssize_t r = recv(sock, buf, sizeof(buf), 0);
        if (r < 0) {
            outError = "connection lost or timed out while receiving the reply";
            failed = true;
            break;
        }
        if (r == 0)
            break;
        received += (size_t)r;

        string data = checker.feed(buf, r);
        size_t start = 0;
        if(status < 0 && !data.empty()){
            status = (unsigned char) data[0];
            start = 1;
            
            if(status == 0){
                out = fopen(outTdbgPath, "wb");
                if(!out){
                    outError = "can not write the downloaded file";
                    failed = true;
                    break;
                }
            }
        }
    
        if (start < data.size()) {
            if (status == 0) {
                size_t len = data.size() - start;
                if (fwrite(data.data() + start, 1, len, out) != len) {
                    outError = "cannot write the downloaded file";
                    failed = true;
                    break;
                }
            }
            else
                errorText += data.substr(start);
        }
    }
 
    bool ok = false;
    if (!failed) {
        if (status < 0)
            outError = received == 0 ? "connection closed with no reply (server busy or waited too long)"
                                     : "incomplete reply from the server";
        else if (!checker.verify())
            outError = "checksum mismatch, response corrupted";
        else if (status == 1)
            outError = errorText;
        else if (status == 0)
            ok = true;
        else
            outError = "unknown reply status";
    }
    if (out != nullptr) {
        fclose(out);
        if (!ok)
            remove(outTdbgPath);
    }
    return ok;
}

bool requestDebugFile(const char *host, int port, const char *sourcePath, const char *outPath, string &outError)
{
    if (access(sourcePath, R_OK) != 0) {
        outError = string("cannot read ") + sourcePath;
        return false;
    }
    int sock = connectToHost(host, port);
    if (sock < 0) {
        outError = "cannot connect to the load balancer";
        return false;
    }
    timeval tv{};
    tv.tv_sec = CLIENT_TIMEOUT_SEC;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
 
    bool ok = false;
    if (!uploadSource(sock, sourcePath))
        outError = "upload failed";
    else
        ok = receiveResponse(sock, outPath, outError);
    close(sock);
    return ok;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

 
bool readExact(FILE *f, void *buf, size_t n)
{
    return n == 0 || fread(buf, 1, n, f) == n;
}
bool readString(FILE *f, string &out)
{
    int32_t size = 0;
    if (!readExact(f, &size, sizeof(size)) || size < 0 || size > MAX_NAME_LEN)
        return false;
    out.resize(size);
    return readExact(f, &out[0], size);
}
bool readVariables(FILE *f, Variable vars[], int32_t count)
{
    for (int32_t i = 0; i < count; i++) {
        if (!readString(f, vars[i].name) || !readExact(f, &vars[i].value, sizeof(int32_t)))
            return false;
    }
    return true;
}
bool readFrame(FILE *f, Frame &fr)
{
    if (!readString(f, fr.func_name))
        return false;
    if (!readExact(f, &fr.argc, sizeof(int32_t)) || fr.argc < 0 || fr.argc > MAX_VARS_PER_FRAME)
        return false;
    if (!readVariables(f, fr.argv, fr.argc))
        return false;
    if (!readExact(f, &fr.returnLine, sizeof(int32_t)))
        return false;
    if (!readExact(f, &fr.localCount, sizeof(int32_t)) || fr.localCount < 0 || fr.localCount > MAX_VARS_PER_FRAME)
        return false;
    return readVariables(f, fr.locals, fr.localCount);
}

bool openTdbg(const char *path, TdbgReader &r)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
 
    TTDBHeader h{};
    bool ok = readExact(f, h.magic, 4) && readExact(f, &h.version, 4) &&
              readExact(f, &h.stepCount, 4) && readExact(f, &h.indexOffset, 8);
    if (ok) {
        fseek(f, 0, SEEK_END);
        long fileSize = ftell(f);
        ok = memcmp(h.magic, "TTDB", 4) == 0 && h.stepCount >= 0 && h.indexOffset >= HEADER_SIZE &&
             h.indexOffset + (int64_t)h.stepCount * 8 <= fileSize;
    }
    vector<int64_t> index;
    if (ok) {
        index.resize(h.stepCount);
        fseek(f, (long)h.indexOffset, SEEK_SET);
        ok = readExact(f, index.data(), index.size() * sizeof(int64_t));
    }
    for (size_t i = 0; ok && i < index.size(); i++)
        ok = index[i] >= HEADER_SIZE && index[i] < h.indexOffset;
 
    if (!ok) {
        fclose(f);
        return false;
    }
    r.file = f;
    r.header = h;
    r.index = index;
    return true;

}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
    
    if(r.file != nullptr)
        fclose(r.file);
    r.file = nullptr;
    r.index.clear();
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if (r.file == nullptr || step < 0 || step >= (int32_t)r.index.size())
        return nullptr;
    fseek(r.file, (long)r.index[step], SEEK_SET);
 
    Snapshot *s = new Snapshot();
    if (!readExact(r.file, &s->stackDepth, sizeof(int32_t)) || s->stackDepth < 0 || s->stackDepth > MAX_STACK_DEPTH) {
        delete s;
        return nullptr;
    }
    for (int32_t i = 0; i < s->stackDepth; i++) {
        if (!readFrame(r.file, s->callStack[i])) {
            delete s;
            return nullptr;
        }
    }
    return s;

}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
   s.active_flag = 0;
   s.reader = TdbgReader();
   s.currentStep = 0;
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    
    if(s.active_flag)
        closeTdbg(s.reader);
    s.active_flag = 0;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
    
    Snapshot* snap = readSnapshotAt(s.reader, s.currentStep);
    if(snap == nullptr){
        cout << "damaged .tdbg file" << endl;
        return false;
    }
    printSnapshot(s.currentStep + 1, s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
    if (!s.active_flag) {
        cout << "no active session, use: start <source file>" << endl;
        return false;
    }
    int64_t target = s.currentStep + delta;
    if (target < 0 || target >= s.reader.header.stepCount) {
        cout << "cannot move by " << delta << " from step " << s.currentStep + 1
             << " of " << s.reader.header.stepCount << endl;
        return false;
    }
    int64_t previous = s.currentStep;
    s.currentStep = target;
    if (!showCurrent(s)) {
        s.currentStep = previous;
        return false;
    }
    return true;
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
    
    string tempPath = string(TDBG_FILE) + ".part";
    string error;
 
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(), tempPath.c_str(), error)) {
        remove(tempPath.c_str());
        cout << error << endl;
        return false;
    }
    TdbgReader fresh;
    if (!openTdbg(tempPath.c_str(), fresh)) {
        remove(tempPath.c_str());
        cout << "server sent a damaged .tdbg file" << endl;
        return false;
    }
    if (fresh.header.stepCount == 0) {
        closeTdbg(fresh);
        remove(tempPath.c_str());
        cout << "the program executed no steps" << endl;
        return false;
    }
 
    if (s.active_flag)
        closeTdbg(s.reader);
    rename(tempPath.c_str(), TDBG_FILE);
    s.reader = fresh;
    s.currentStep = 0;
    s.active_flag = 1;
    return showCurrent(s);

}
bool cmdNext(DebugSession &s) { return moveBy(s, 1); }
bool cmdPrv(DebugSession &s) { return moveBy(s, -1); }
bool cmdJmpNext(DebugSession &s, int32_t n) { return moveBy(s, n); }
bool cmdJmpPrv(DebugSession &s, int32_t n) { return moveBy(s, -n); }

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

string variablesToString(const Variable vars[], int32_t count)
{
    string text;
    for (int32_t i = 0; i < count; i++) {
        if (i > 0)
            text += ", ";
        text += vars[i].name + "=" + to_string(vars[i].value);
    }
    return text;
}

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "--- step " << step << " of " << totalSteps << " ---" << endl;
    for (int32_t i = 0; i < s.stackDepth; i++) {
        const Frame &f = s.callStack[i];
        string locals = f.localCount > 0 ? variablesToString(f.locals, f.localCount) : "-";
        cout << "#" << i << " " << f.func_name << "(" << variablesToString(f.argv, f.argc) << ")"
             << "  locals: " << locals << endl;
    }

}
void printHelp()
{
    // Print the commands
    
        cout << "Commands:\n"
         << "  start <source file>   send a program and open its debug session\n"
         << "  next                  go forward one step\n"
         << "  prv                   go back one step\n"
         << "  jmp next <n>          go forward n steps\n"
         << "  jmp prv <n>           go back n steps\n"
         << "  help                  show this text\n"
         << "  quit, exit            leave" << endl;

}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================


vector<string> splitWords(const string &line)
{
    vector<string> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
            i++;
        if (i >= line.size())
            break;
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
            i++;
        words.push_back(line.substr(start, i - start));
    }
    return words;
}
bool parseCount(const string &text, int32_t &out)
{
    if (text.empty() || text.size() > 10)
        return false;
    long long value = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] < '0' || text[i] > '9')
            return false;
        value = value * 10 + (text[i] - '0');
    }
    if (value < 1 || value > MAX_JUMP)
        return false;
    out = (int32_t)value;
    return true;
}


bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
    
    vector<string> words = splitWords(line);
    if (!words.empty() && words[0] == PROGRAM_NAME)
        words.erase(words.begin());
    if (words.empty())
        return true;
 
    const string &command = words[0];
    if (command == "start" && words.size() == 2)
        return cmdStart(s, words[1]);
    if (command == "next" && words.size() == 1)
        return cmdNext(s);
    if (command == "prv" && words.size() == 1)
        return cmdPrv(s);
    if (command == "jmp" && words.size() == 3) {
        int32_t n = 0;
        if (words[1] == "next" && parseCount(words[2], n))
            return cmdJmpNext(s, n);
        if (words[1] == "prv" && parseCount(words[2], n))
            return cmdJmpPrv(s, n);
    }
    if (command == "help" && words.size() == 1) {
        printHelp();
        return true;
    }
    if ((command == "quit" || command == "exit") && words.size() == 1) {
        quitRequested = true;
        return true;
    }
    cout << "unknown command" << endl;
    printHelp();
    return false;

}
int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.

    if (argc == 3) {
        LB_HOST = argv[1];
        LB_PORT = atoi(argv[2]);
    }
 
    DebugSession session;
    initSession(session);
    cout << "Time-Travel Debugger client (load balancer " << LB_HOST << ":" << LB_PORT << ")" << endl;
    printHelp();
 
    bool quitRequested = false;
    string line;
    while (!quitRequested) {
        cout << PROMPT << flush;
        if (!getline(cin, line))
            break;
        try {
            runCommand(session, line, quitRequested);
        }
        catch (exception &e) {
            cout << "error: " << e.what() << endl;
        }
    }
    closeSession(session);
    return 0;

}
