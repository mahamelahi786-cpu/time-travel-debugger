// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code).
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not understand the messages. It only moves bytes and checks the checksum while moving them.
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
// Usage: ./load_balancer <server_ip> <server_port>
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the message until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload][checksum: 4 bytes], then closes the connection
//
// Socket ownership: exactly one place closes each client socket.
//   - the dispatcher closes it after relayOneClient() (or when the client waited too long)
//   - acceptLoop closes it when it refuses a client ("server busy")
//   relayOneClient() only closes the socket it opened to the server.

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
#include <stdexcept>
#include <cstdio>
#include <cerrno>
#include <ctime>
#include <poll.h>
#include <pthread.h>
#include <sys/time.h>
using namespace std;

// ---- Constants ----
const char *SERVER_PATH = "./server";      // the program we start when the server is down
// const int IO_BUFFER_SIZE = 64 * 1024;   // fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
const int RELAY_TIMEOUT_SECONDS = 30;      // longest wait for one recv() or send() while serving a client
const int QUEUE_WAIT_LIMIT_SECONDS = 30;   // a client that waited longer than this in the queue is dropped
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;

// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
    };

    Node *front, *rear;
    int count;

public:
    // Implement these functions:
    Queue()
    {
        front = nullptr;
        rear = nullptr;
        count = 0;
    }

    ~Queue()
    {
        while(front != nullptr){
            Node *old = front;
            front = front->next;
            delete old;
        }
    }

    void enqueue(const T &val)
    {
        Node *node = new Node{val, nullptr};

        if(rear == nullptr){
            front = node;
        }else{
            rear->next = node;
        }

        rear = node;
        count++;
    }

    T dequeue()
    {
        if(front == nullptr){
            throw underflow_error("cannot dequeue from an empty queue");
        }

        Node *old = front;
        T val = old->data;

        front = front->next;
        if(front == nullptr){
            rear = nullptr;
        }

        delete old;
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
volatile sig_atomic_t flag = 1;
bool stopAcceptor = false;

// ---- Basic socket helpers ----
bool sendError(int sock, const string &message)
{
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text.
    char status = 1; // matlab error
    uint32_t crc = crc_update(CRC_START, &status, 1);
    crc = crc_update(crc, message.data(), message.size());

    char trailer[4];
    write_u32(trailer, crc_finish(crc));

    if(!sendAll(sock, &status, 1)){
        return false;
    }

    if(!sendAll(sock, message.data(), message.size())){
        return false;
    }

    return sendAll(sock, trailer, sizeof(trailer));
}

int createListenSocket(int port)
{
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.
    if(port < 1 || port > 65535){
        cerr << "Error: invalid load balancer port" << endl;
        exit(EXIT_FAILURE);
    }

    int listen_sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if(listen_sock < 0){
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int reuseAddress = 1;
    if(setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
                  &reuseAddress, sizeof(reuseAddress)) < 0){
        perror("setsockopt SO_REUSEADDR");
        close(listen_sock);
        exit(EXIT_FAILURE);
    }

    sockaddr_in address{};
    address.sin_family = AF_INET; //kis type ka address hai
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);  // kis port pr connection ha

    //socket ko iss address or port k satg attactch kra ga
    if(bind(listen_sock, (sockaddr *)&address, sizeof(address)) < 0){
        perror("bind");
        close(listen_sock);
        exit(EXIT_FAILURE);
    }

    //ab socket incoming connections k leya ready hai
    if(listen(listen_sock, LISTEN_BACKLOG) < 0){
        perror("listen");
        close(listen_sock);
        exit(EXIT_FAILURE);
    }

    return listen_sock;
}

int connectToServer(const char *host, int port)
{
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails
    if(port < 1 || port > 65535){
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if(inet_pton(AF_INET, host, &address.sin_addr) != 1){
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(sock < 0){
        return -1;
    }

    if(connect(sock, (sockaddr *)&address, sizeof(address)) < 0){
        close(sock);
        return -1;
    }

    return sock;
}

void reportToClient(int sock, const string &message)
{
    if(!sendError(sock, message)){
        cerr << "Could not send error to client: " << message << endl;
        return;
    }

    if(shutdown(sock, SHUT_WR) < 0){
        return;
    }

    timeval timeout{};
    timeout.tv_usec = 100000;
    if(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0){
        return;
    }

    char buffer[IO_BUFFER_SIZE];
    size_t drained = 0;
    while(drained < 16ULL * 1024 * 1024){
        ssize_t got = recv(sock, buffer, sizeof(buffer), 0);
        if(got < 0 && errno == EINTR){
            continue;
        }
        if(got <= 0){
            break;
        }
        drained += static_cast<size_t>(got);
    }
}

bool setTimeouts(int sock)
{
    timeval timeout{};
    timeout.tv_sec = RELAY_TIMEOUT_SECONDS;

    if(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                  &timeout, sizeof(timeout)) < 0){
        perror("setsockopt SO_RCVTIMEO");
        return false;
    }

    if(setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                  &timeout, sizeof(timeout)) < 0){
        perror("setsockopt SO_SNDTIMEO");
        return false;
    }

    return true;
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
    if(port < 1 || port > 65535){
        return false;
    }

    string command = string(SERVER_PATH) + " " + to_string(port) + " &";
    return system(command.c_str()) == 0;
}

int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times
    // 4. Return the first working socket, or -1 if the server never came up.
    if(!flag){
        return -1;
    }

    int sock = connectToServer(serverHost.c_str(), serverPort);
    if(sock >= 0){
        return sock;
    }

    if(!flag || !startServer(serverPort)){
        return -1;
    }

    for(int i = 0; i < STARTUP_MAX_ATTEMPTS && flag; i++){
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
        if(!flag){
            break;
        }

        sock = connectToServer(serverHost.c_str(), serverPort);
        if(sock >= 0){
            return sock;
        }
    }

    return -1;
}

// =============================================================================================
// PART 2: PASSING THE MESSAGE AND THE REPLY (one client, called by the main thread only)
// =============================================================================================

// Every message is [status: 1 byte][body][checksum: 4 bytes].
// forwardChecked() moves one message from fromSock to toSock and checks its checksum on the way.
// TrailerChecker holds back the last 4 bytes (they may be the checksum), so only the rest is forwarded.
enum RelayResult
{
    RELAY_OK,
    RELAY_BAD_CHECKSUM,
    RELAY_READ_FAILED,
    RELAY_TIMEOUT,
    RELAY_WRITE_FAILED,
    RELAY_STOPPED
};

RelayResult forwardChecked(int fromSock, int toSock, bool &sentBody)
{
    TrailerChecker checker;
    char buffer[IO_BUFFER_SIZE];
    sentBody = false;

    while(flag){
        ssize_t got = recv(fromSock, buffer, sizeof(buffer), 0);
        if(got < 0){
            if(errno == EINTR){
                continue;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                return RELAY_TIMEOUT;
            }
            return RELAY_READ_FAILED;
        }

        if(got == 0){
            if(!sentBody || !checker.verify()){
                return RELAY_BAD_CHECKSUM;
            }

            char trailer[4];
            write_u32(trailer, checker.checksum());
            if(!sendAll(toSock, trailer, sizeof(trailer))){
                return RELAY_WRITE_FAILED;
            }

            return RELAY_OK;
        }

        string bytes = checker.feed(buffer, static_cast<size_t>(got));
        if(!bytes.empty()){
            if(!sendAll(toSock, bytes.data(), bytes.size())){
                return RELAY_WRITE_FAILED;
            }
            sentBody = true;
        }
    }

    return RELAY_STOPPED;
}

void relayOneClient(ClientInfo &c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", return.
    // 2. Client -> server (checked), then shutdown(serverSock, SHUT_WR).
    // 3. Server -> client (checked).
    // 4. Close serverSock. The client socket is closed by the dispatcher, NOT here.
    cout << "Serving client " << c.address << ":" << c.port << endl;

    if(!setTimeouts(c.sock)){
        return;
    }

    int serverSock = connectToServerWithWakeup();
    if(serverSock < 0){
        if(flag){
            reportToClient(c.sock, "backend server failed to start");
        }
        return;
    }

    if(!setTimeouts(serverSock)){
        close(serverSock);
        reportToClient(c.sock, "backend connection setup failed");
        return;
    }

    // ---- client -> server ----
    bool sentUpload;
    RelayResult up = forwardChecked(c.sock, serverSock, sentUpload);

    if(up != RELAY_OK){
        close(serverSock);

        if(up == RELAY_BAD_CHECKSUM){
            reportToClient(c.sock, "upload checksum mismatch");
        }else if(up == RELAY_TIMEOUT){
            reportToClient(c.sock, "timed out while waiting for the upload");
        }else if(up == RELAY_WRITE_FAILED){
            reportToClient(c.sock, "backend server failed while receiving the upload");
        }else if(up == RELAY_READ_FAILED){
            cerr << "Client disconnected during upload" << endl;
        }
        return;
    }

    // Tell the server the message is complete (it waits for recv() == 0).
    // We can still read its reply on the same socket.
    if(shutdown(serverSock, SHUT_WR) < 0){
        perror("shutdown backend");
        close(serverSock);
        reportToClient(c.sock, "could not finish sending the upload to the backend");
        return;
    }

    // ---- server -> client ----
    bool sentReply;
    RelayResult down = forwardChecked(serverSock, c.sock, sentReply);
    close(serverSock);

    if(down == RELAY_OK || down == RELAY_STOPPED){
        return;
    }

    if(down == RELAY_WRITE_FAILED){
        cerr << "Could not finish sending the reply to the client" << endl;
        return;
    }

    if(!sentReply){
        reportToClient(c.sock, "backend server failed before sending a reply");
    }else if(down == RELAY_BAD_CHECKSUM){
        // The client already has part of the reply and we did not send the trailer,
        // so the client rejects it ("response corrupted"). We cannot add a second message after it.
        cerr << "Backend reply checksum mismatch" << endl;
    }else{
        cerr << "Backend failed in the middle of the reply" << endl;
    }
}

// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================

// Used when the queue is full. We send the error, then read and throw away whatever the client
// already sent. If we closed with unread data in the socket, program would reset the connection
// and the client could lose the server busy message. The caller (acceptLoop) closes the socket.
void refuseClient(int sock)
{
    timeval timeout{};
    timeout.tv_usec = 100000;

    if(setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0){
        perror("setsockopt busy socket");
        return;
    }

    reportToClient(sock, "server busy, try again later");
}

bool acceptorShouldStop()
{
    lock_guard<mutex> lock(queueMutex);
    return stopAcceptor;
}

void requestAcceptorStop()
{
    lock_guard<mutex> lock(queueMutex);
    stopAcceptor = true;
}

void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client.
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: sendError "server busy, try again later",
    //  close the client, and go on.
    //  Otherwise enqueue the client (inside queueMutex).
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).
    while(!acceptorShouldStop()){
        // i wait at most 100 ms for a client, so we notice Ctrl+C quickly
        pollfd watched{};
        watched.fd = listenSock;
        watched.events = POLLIN;

        int ready = poll(&watched, 1, 100);
        if(ready < 0){
            if(errno == EINTR){
                continue;
            }
            perror("poll listener");
            requestAcceptorStop();
            return;
        }

        if(watched.revents & (POLLERR | POLLHUP | POLLNVAL)){
            cerr << "Listening socket stopped working" << endl;
            requestAcceptorStop();
            return;
        }

        if(ready == 0 || !(watched.revents & POLLIN) || acceptorShouldStop()){
            continue;
        }

        sockaddr_in peer{};
        socklen_t peerSize = sizeof(peer);
        int clientSock = accept4(listenSock, (sockaddr *)&peer, &peerSize, SOCK_CLOEXEC);

        if(clientSock < 0){
            if(errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK){
                perror("accept");
                usleep(100 * 1000);
            }
            continue;
        }

        ClientInfo c{};
        c.sock = clientSock;
        c.port = ntohs(peer.sin_port);
        c.arrivedAt = time(nullptr);

        char ip[INET_ADDRSTRLEN];
        if(inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip)) != nullptr){
            c.address = ip;
        }else{
            c.address = "unknown";
        }

        bool queued = false;
        {
            lock_guard<mutex> lock(queueMutex);
            if(!stopAcceptor && pendingClients.size() < MAX_PENDING_CLIENTS){
                pendingClients.enqueue(c);
                queued = true;
            }
        }

        if(!queued){
            if(!acceptorShouldStop()){
                refuseClient(clientSock);
            }
            close(clientSock);
        }
    }
}

void dispatcherLoop()
{
    // This runs in the main thread until Ctrl+C.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.
    while(flag && !acceptorShouldStop()){
        ClientInfo c{};
        bool gotClient = false;

        {
            lock_guard<mutex> lock(queueMutex);
            if(!pendingClients.isEmpty()){
                c = pendingClients.dequeue();
                gotClient = true;
            }
        }

        if(!gotClient){
            usleep(10 * 1000);
            continue;
        }

        if(difftime(time(nullptr), c.arrivedAt) > QUEUE_WAIT_LIMIT_SECONDS){
            // the client has given up already so close the socket, no reply
            cerr << "Dropping client " << c.address << ":" << c.port << " (waited too long in the queue)" << endl;
        }else if(flag && !acceptorShouldStop()){
            relayOneClient(c);   // runs outside the lock, so the acceptor can keep queueing
        }

        close(c.sock);
    }
}

void handler(int sg)
{
    (void)sg;
    flag = 0;
}

int main(int argc, char *argv[])
{
    if(argc != 3){
        cerr << "Usage: " << argv[0] << " <server_ip> <server_port>" << endl;
        return 1;
    }

    in_addr testAddress{};
    if(inet_pton(AF_INET, argv[1], &testAddress) != 1){
        cerr << "Error: server ip must be a valid IPv4 address, for example 127.0.0.1" << endl;
        return 1;
    }

    char *end = nullptr;
    errno = 0;
    long port = strtol(argv[2], &end, 10);
    if(errno != 0 || end == argv[2] || *end != '\0' || port < 1 || port > 65535){
        cerr << "Error: server port must be a number from 1 to 65535" << endl;
        return 1;
    }

    serverHost = argv[1];
    serverPort = static_cast<int>(port);

    // Ctrl+C sets flag = 0 so the loops can finish. i use sigaction without SA_RESTART,
    // so a recv() that is waiting returns with EINTR and checks the flag.
    struct sigaction action{};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    if(sigaction(SIGINT, &action, nullptr) < 0){
        perror("sigaction");
        return 1;
    }

    int listenSock = createListenSocket(lbPort);

    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);

    if(pthread_sigmask(SIG_BLOCK, &blocked, nullptr) != 0){
        cerr << "Could not block SIGINT before starting acceptor" << endl;
        close(listenSock);
        return 1;
    }

    thread acceptor;
    try{
        acceptor = thread(acceptLoop, listenSock);
    }catch(const exception &e){
        cerr << "Could not start acceptor thread: " << e.what() << endl;
        close(listenSock);
        return 1;
    }

    int result = 0;
    if(pthread_sigmask(SIG_UNBLOCK, &blocked, nullptr) != 0){
        cerr << "Could not unblock SIGINT in main thread" << endl;
        result = 1;
    }else{
        cout << "Load balancer listening on port " << lbPort << ", server at " << serverHost << ":" << serverPort << endl;
        dispatcherLoop();
        if(flag && acceptorShouldStop()){
            result = 1;
        }
    }

    requestAcceptorStop();
    acceptor.join();
    close(listenSock);

    {
        lock_guard<mutex> lock(queueMutex);
        while(!pendingClients.isEmpty()){
            ClientInfo c = pendingClients.dequeue();
            close(c.sock);
        }
    }

    cout << "Load balancer stopped" << endl;
    return result;
}