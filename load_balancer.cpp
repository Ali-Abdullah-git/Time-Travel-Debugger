// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 


#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
using namespace std;


// ---- Constants ----
const char *SERVER_PATH = "./server";      // the program we start when the server is down
const int IO_BUFFER_SIZE = 64 * 1024;      // fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.0";
int serverPort = 5001;
//some constants I added
const int CLIENT_TIMEOUT_SEC = 30;
const int RELAY_TIMEOUT_SEC = 30;
const int IDLE_SLEEP_MS = 50;

// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
        Node(T val) { data = val; next = nullptr; }
    };
    Node *front, *rear;
    int count;


public:
    // Implement these functions:
    Queue()
    {
        front = rear = nullptr;
        count = 0;
    }
    ~Queue()
    {
        while (front) {
            Node* temp = front;
            front = front->next;
            delete temp;
        }
        rear = nullptr; 
        count = 0; //optional
    }
    void enqueue(const T &val)
    {
        Node* node = new Node(val);

        if (front == nullptr) {
            front = rear = node;
        }
        else {
            rear->next = node;
            rear = rear->next;
        }

        count++;
    }
    T dequeue()
    {
        if (count == 0) {
            throw runtime_error("Queue Underflow Error");
        }

        T val = front->data;
        Node* temp = front;
        front = front->next;
        delete temp;
        
        if (front == nullptr) {
            rear = nullptr;
        }

        count--;
        return val;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int size()
    {
        return count;
    }
};

struct ClientInfo
{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char *buf, size_t n) 
{
    // Reply format: [status: 1 byte][sending_material][checksum: 4 bytes].
    // send() may send fewer bytes than asked for, so keep calling it until all n bytes are sent.

    size_t sent = 0;
    while(sent < n){
      ssize_t r = send(sock, buf + sent, n - sent, MSG_NOSIGNAL);
      if(r<=0)
        return false;
      sent += r;
    }
    return true;
}
bool sendError(int sock, const string &message)
{
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text. 
    
    string reply(1, (char)1);
    reply += message;
 
    Crc32 crc;
    crc.update(reply.data(), reply.size());
    unsigned char trailer[4];
    crcToBytes(crc.value(), trailer);
    reply.append((const char *)trailer, 4);
 
    return sendAll(sock, reply.data(), reply.size());  
}
int createListenSocket(int port)
{ 
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.
    
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        cerr << "socket failed" << endl;
        exit(1);
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(sock, (sockaddr *)&addr, sizeof(addr)) < 0) {
        cerr << "bind failed" << endl;
        exit(1);
    }
    if (listen(sock, LISTEN_BACKLOG) < 0) {
        cerr << "listen failed" << endl;
        exit(1);
    }
    return sock;
}
int connectToServer(const char *host, int port)
{
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails 
    
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
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
 
// Two additional helper functions
void setTimeout(int sock, int seconds)
{
    timeval tv{};
    tv.tv_sec = seconds;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void closePolitely(int sock)
{
    shutdown(sock, SHUT_WR);
    setTimeout(sock, 1);
    char buf[4096];
    int rounds = 0;
    while (rounds++ < 256 && recv(sock, buf, sizeof(buf), 0) > 0);
    close(sock);
}


// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.
    
    string command = string(SERVER_PATH) + " " + to_string(port) + " &";
    return system(command.c_str()) == 0;
}
int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times 
    // 4. Return the first working socket, or -1 if the server never came up.
    
    int sock = connectToServer(serverHost.c_str(), serverPort);
    if (sock >= 0)
        return sock;
    if (!startServer(serverPort))
        return -1;
    for (int attempt = 0; attempt < STARTUP_MAX_ATTEMPTS && flag; attempt++) {
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
        sock = connectToServer(serverHost.c_str(), serverPort);
        if (sock >= 0)
            return sock;
    }
    return -1;

}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================

//additional helper function
void failClient(ClientInfo &c, int serverSock, const string &message)
{
    cout << "failed " << c.address << ":" << c.port << " - " << message << endl;
    if (serverSock >= 0)
        close(serverSock);
    sendError(c.sock, message);
    closePolitely(c.sock);
}

void relayOneClient(ClientInfo &c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.
    
    int serverSock = connectToServerWithWakeup();
    if (serverSock < 0) {
        failClient(c, -1, "backend server failed to start");
        return;
    }
    setTimeout(c.sock, RELAY_TIMEOUT_SEC);
    setTimeout(serverSock, RELAY_TIMEOUT_SEC);
 
    char buf[IO_BUFFER_SIZE];
 
    // client -> server
    TrailerChecker upload;
    bool uploadFinished = false;
    bool serverOk = true;
    while (true) {
        ssize_t r = recv(c.sock, buf, sizeof(buf), 0);
        if (r < 0)
            break;
        if (r == 0) {
            uploadFinished = true;
            break;
        }
        string part = upload.feed(buf, (size_t)r);
        if (!sendAll(serverSock, part.data(), part.size())) {
            serverOk = false;
            break;
        }
    }
    if (!serverOk) {
        failClient(c, serverSock, "backend server connection lost");
        return;
    }
    if (!uploadFinished) {
        failClient(c, serverSock, "upload failed");
        return;
    }
    if (!upload.verify()) {
        failClient(c, serverSock, "upload checksum mismatch");
        return;
    }
    if (!sendAll(serverSock, upload.trailer().data(), 4)) {
        failClient(c, serverSock, "backend server connection lost");
        return;
    }
    shutdown(serverSock, SHUT_WR);
 
    // server -> client
    TrailerChecker reply;
    size_t replyBytes = 0;
    bool replyFinished = false;
    bool clientOk = true;
    while (true) {
        ssize_t r = recv(serverSock, buf, sizeof(buf), 0);
        if (r < 0)
            break;
        if (r == 0) {
            replyFinished = true;
            break;
        }
        replyBytes += (size_t)r;
        string part = reply.feed(buf, (size_t)r);
        if (!sendAll(c.sock, part.data(), part.size())) {
            clientOk = false;
            break;
        }
    }
    close(serverSock);
 
    if (!clientOk) {
        cout << "client " << c.address << ":" << c.port << " left before the reply was finished" << endl;
        close(c.sock);
        return;
    }
    if (replyBytes == 0) {
        sendError(c.sock, "backend server did not reply");
    }
    else if (replyFinished && reply.verify()) {
        sendAll(c.sock, reply.trailer().data(), 4);
    }
    // otherwise the trailer is not sent, so the client sees a bad checksum and rejects the reply
    close(c.sock);
}
 




// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).
    
    while (flag) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int sock = accept4(listenSock, (sockaddr *)&addr, &len, SOCK_CLOEXEC);
        if (sock < 0) {
            cerr << "accept failed" << endl;
            continue;
        }
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        ClientInfo client{ sock, ip, ntohs(addr.sin_port), time(nullptr) };
 
        bool full = false;
        {
            lock_guard<mutex> lock(queueMutex);
            if (pendingClients.size() >= MAX_PENDING_CLIENTS)
                full = true;
            else
                pendingClients.enqueue(client);
        }
        if (full) {
            cout << "refused " << client.address << ":" << client.port << " - queue is full" << endl;
            sendError(sock, "server busy, try again later");
            closePolitely(sock);
        }
    }
}

void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.

    while (flag) {
        ClientInfo client{};
        bool found = false;
        {
            lock_guard<mutex> lock(queueMutex);
            if (!pendingClients.isEmpty()) {
                client = pendingClients.dequeue();
                found = true;
            }
        }
        if (!found) {
            usleep(IDLE_SLEEP_MS * 1000);
            continue;
        }
        if (difftime(time(nullptr), client.arrivedAt) > CLIENT_TIMEOUT_SEC) {
            cout << "dropped " << client.address << ":" << client.port << " - waited too long" << endl;
            closePolitely(client.sock);
            continue;
        }
        cout << "serving " << client.address << ":" << client.port << endl;
        relayOneClient(client);
    }
}
 



void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{

    if (argc == 3) {
        serverHost = argv[1];
        serverPort = atoi(argv[2]);
    }
 
    signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
 
    int listenSock = createListenSocket(lbPort);
    cout << "load balancer listening on port " << lbPort << ", server at " << serverHost << ":" << serverPort << endl;
 
    thread acceptor(acceptLoop, listenSock);
    acceptor.detach();
    dispatcherLoop();
    return 0;

}
