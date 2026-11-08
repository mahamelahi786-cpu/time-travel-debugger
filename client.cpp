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
#include <arpa/inet.h>
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
// const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;

// These two must be the same as in Phase 1

// const int MAX_VARS_PER_FRAME = 16;
// const int MAX_STACK_DEPTH = 64;


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
// struct Variable
// {
//     string name;
//     int32_t value;
// };
// struct Frame
// {
//     string func_name;
//     int32_t argc;
//     Variable argv[MAX_VARS_PER_FRAME];
//     int32_t returnLine;
//     Variable locals[MAX_VARS_PER_FRAME];
//     int32_t localCount;
// };
// struct Snapshot
// {
//     Frame callStack[MAX_STACK_DEPTH];
//     int32_t stackDepth;
// };
// struct TTDBHeader 
// {
//     char magic[4]; // "TTDB"
//     int32_t version;
//     int32_t stepCount;
//     int64_t indexOffset;
// };

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
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

//bool recvAll and sendAll as common moved to helper.h

int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
    if (port < 1 || port > 65535){
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if(inet_pton(AF_INET, host, &address.sin_addr)!=1){
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock<0){
        return -1;
    }

    if(connect(sock, (struct sockaddr*)&address, sizeof(address))<0){
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
    FILE* file = fopen(sourcePath, "rb");
    if(file==nullptr){
        return false;
    }

    char buffer[IO_BUFFER_SIZE];
    char status = 0;
    uint32_t crc = crc_update(CRC_START, &status, 1);
    if(!sendAll(sock, &status, 1)){
        fclose(file);
        return false;
    }
    size_t bytesRead;
    
    while((bytesRead=fread(buffer,1,sizeof(buffer), file))>0){
        crc = crc_update(crc, buffer, bytesRead);

        if(!sendAll(sock, buffer, bytesRead)){
            fclose(file);
            return false;
        }

    }

    if (ferror(file)){
        fclose(file);
        return false;
    }
    
    fclose(file);

    char trailer[4];
    write_u32(trailer, crc_finish(crc));

    if(!sendAll(sock, trailer, sizeof(trailer))){
        return false;
    }

    return shutdown(sock, SHUT_WR) == 0;
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.

    TrailerChecker checker;
    char buffer[IO_BUFFER_SIZE];

    int status = -1;
    bool sawByte = false;
    FILE* file= nullptr;
    string serverError;
    outError.clear();

    while(true){
        ssize_t received = recv(sock, buffer, sizeof(buffer), 0);

        if(received<0){
            if(errno==EINTR){
                continue;
            }
            outError ="failed to receive response";
            break;
        }

        if(received==0){
            if(!sawByte){
                outError = "no response from the server";
            }else if (!checker.verify()){
                outError = "checksum mismatch, response corrupted";
            }else if(status == -1){
                outError = "response is missing a status byte";
            }
    
            break;
        }

        sawByte = true;

        string body = checker.feed(buffer, static_cast<size_t>(received));
        if(body.empty()){
            continue;
        }

        size_t payloadStart = 0;
        if(status == -1){
            status = static_cast<unsigned char>(body[0]);
            payloadStart = 1;

            if(status ==0){
                file = fopen(outTdbgPath,"wb");
                if(file==nullptr){
                    outError ="could not open the download file";
                    break;
                }
            }else if(status != 1){
                outError = "invalid response status";
                break;
            }

        }

        size_t payloadSize = body.size() - payloadStart;
        const char* payload = body.data() + payloadStart;

        if (status == 0){
            if(fwrite(payload, 1, payloadSize, file)!=payloadSize){
                outError = "failed to write the download file";
                break;
            }

        }else{
            serverError.append(payload,payloadSize);
        }
    }

    if(file!=nullptr){
        if(fclose(file)!=0 && outError.empty()){
            outError = "failed to save the download file";
        }

        if(!outError.empty()){
            remove(outTdbgPath);
        }
    }

    if(!outError.empty()){
        return false;
    }

    if(status == 1){
       if(serverError.empty()){
        outError = "server returned an error";
       }else{
        outError = serverError;
       }

       return false;
    }

    return status ==0;
}

bool requestDebugFile(const char* sourcePath, const char* tempPath, string& outError){
    outError.clear();
    int sock = connectToHost(LB_HOST.c_str(), LB_PORT);
    if(sock<0){
        outError = "could not connect to the load balancer";
        return false;
    }

    if(uploadSource(sock, sourcePath)==false){
        outError = "could not upload the source file";
        close(sock);
        return false;
    }

    bool downloaded = receiveResponse(sock, tempPath, outError);
    close(sock);
    return downloaded;

}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

bool openTdbg(const char *path, TdbgReader &r)
{
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
   
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
}
bool moveBy(DebugSession &s, int32_t delta)
{
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
}
bool cmdNext(DebugSession &s) {}
bool cmdPrv(DebugSession &s) {}
bool cmdJmpNext(DebugSession &s, int32_t n) {}
bool cmdJmpPrv(DebugSession &s, int32_t n) { }

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
}
void printHelp()
{
    // Print the commands
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

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

}
