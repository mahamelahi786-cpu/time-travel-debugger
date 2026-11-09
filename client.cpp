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
#include <sys/time.h>
#include "helper.h"
#include <arpa/inet.h>
#include <sstream>
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
// const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;
const char *TEMP_TDBG_FILE = "ttdb_new.tdbg";

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
    // ssize_t file_index;
    FILE* file = nullptr;
    TTDBHeader header{};
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
            if (errno == EAGAIN || errno == EWOULDBLOCK){
                outError = "timed out waiting for a response";
            }else{
                outError = "failed to receive response";
            }
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
            const size_t maxErrorSize = 4096;
            size_t remaining = maxErrorSize - serverError.size();
            size_t toKeep = payloadSize;

            if(toKeep > remaining){
                toKeep = remaining;
            }
            serverError.append(payload,toKeep);
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
    FILE* test = fopen(sourcePath, "rb");
    if(test == nullptr){
        outError = "could not open the source file";
        return false;
    }
    fclose(test);
    int sock = connectToHost(LB_HOST.c_str(), LB_PORT);
    if(sock<0){
        outError = "could not connect to the load balancer";
        return false;
    }

    timeval timeout{};
    timeout.tv_sec = 30;

    if(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))!=0){
        outError = "could not set the receive timeout";
        close(sock);
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
    const int64_t headerSize = 20;
    const int32_t supportedVersion = 1;

    if(r.file != nullptr){
        return false;
    }

    FILE *file = fopen(path, "rb");
    if(file==nullptr){
        return false;
    }

    TTDBHeader header{};
    if(fread(header.magic, 1, 4, file)!= 4 || fread(&header.version, sizeof(header.version), 1, file) != 1 || fread (&header.stepCount, sizeof(header.stepCount), 1, file)!= 1 || fread (&header.indexOffset, sizeof(header.indexOffset), 1, file)!= 1){
        fclose(file);
        return false;
    }
    
    if (memcmp(header.magic, "TTDB", 4)!=0 || header.version != supportedVersion || header.stepCount <= 0){
        fclose(file);
        return false;
    }

    if (fseek(file, 0, SEEK_END)!=0){
        fclose(file);
        return false;
    }

    long fileSize = ftell(file);
    int64_t indexSize = static_cast<int64_t>(header.stepCount)*8;

    if (fileSize < headerSize || header.indexOffset < headerSize || header.indexOffset > fileSize || indexSize != fileSize - header.indexOffset){
        fclose(file);
        return false;
    }

    if (fseek(file, static_cast<long> (header.indexOffset), SEEK_SET)!=0){
        fclose(file);
        return false;
    }

    vector<int64_t> offsets;
    int64_t previous = headerSize -1;

    for (int32_t i=0; i<header.stepCount; i++){
        int64_t offset;

        if(fread(&offset, sizeof(offset), 1, file)!=1 || offset <= previous || offset >= header.indexOffset){
            fclose(file);
            return false;
        }

        offsets.push_back(offset);
        previous = offset;


    }

    r.file = file;
    r.header = header;
    r.index.swap(offsets);
    return true;
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
    if(r.file != nullptr){
        fclose(r.file);
        r.file = nullptr;
    }

    r.header = TTDBHeader{};
    r.index.clear();
}


bool readString(FILE* file, string& text, int64_t end){
    int32_t length;
    if(fread(&length, sizeof(length), 1, file)!=1){
        return false;
    }

    long position = ftell(file);
    if(length < 0 || position == -1 || length > end - position){
        return false;
    }

    text.resize(length);
    if(length > 0 && fread(&text[0], 1, length, file)!= static_cast<size_t>(length)){
        return false;
    }
    return true;
}

bool readVariable(FILE* file, Variable& var, int64_t end){
    return readString(file, var.name, end) && fread(&var.value, sizeof(var.value), 1, file)==1;
 
}

bool fillSnapshot(FILE* file, Snapshot& snap, int64_t end){
    if(fread(&snap.stackDepth, sizeof(int32_t), 1, file)!=1 || snap.stackDepth < 0 || snap.stackDepth > MAX_STACK_DEPTH){
        return false;
    }

    for(int32_t i = 0; i< snap.stackDepth; i++){
        Frame& frame = snap.callStack[i];

        if(!readString(file, frame.func_name, end) || fread(&frame.argc, sizeof(int32_t), 1, file)!=1 || frame.argc < 0 || frame.argc > MAX_VARS_PER_FRAME){
            return false;
        }

        for(int32_t j = 0;j<frame.argc; j++){
            if(!readVariable(file, frame.argv[j], end)){
                return false;
            }
        }

        if(fread(&frame.returnLine, sizeof(int32_t), 1, file)!=1 || fread(&frame.localCount, sizeof(int32_t), 1, file)!= 1 || frame.localCount < 0 || frame.localCount > MAX_VARS_PER_FRAME){
            return false;
        }

        for (int32_t j = 0; j< frame.localCount; j++){
            if(!readVariable(file, frame.locals[j], end)){
                return false;
            }
        }

    }
    return ftell(file) == end;
}

Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if (r.file == nullptr || step < 0 || step >= static_cast<int32_t>(r.index.size())){
        return nullptr;
    }

    int64_t start = r.index[step];
    int64_t end = r.header.indexOffset;
    if(step + 1 < static_cast<int32_t>(r.index.size())){
        end = r.index[step + 1];
    }

    if (fseek(r.file, static_cast<long>(start), SEEK_SET)!=0){
        return nullptr;
    }

    Snapshot* snap = new Snapshot;
    if(!fillSnapshot(r.file, *snap, end)){
        delete snap;
        return nullptr;
    }
    return snap;

}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
    s.active_flag = 0; // koi session start nahi hua
    s.currentStep = 0; // jab session start hoga, pehla step code ma zero hoga
    s.reader = TdbgReader{}; // blank reader
   
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    if(s.active_flag){ // if session active so close k function sa usse close krdo
        closeTdbg(s.reader);
    }
    s.active_flag =0; //next, prev will know that pehla start chhaiya
    s.currentStep = 0; //step reset ho jata hai. Dobara closeSession call ho tou flag 0 hai
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
    
    // session not active
    if (!s.active_flag){
        cout <<"No session yet. Use: start <source file>"<<endl;
        return false;
    }

    // Snapshot read
    Snapshot* snap = readSnapshotAt(s.reader, static_cast<int32_t> (s.currentStep));
    if (snap == nullptr){
        cout <<"Error: damaged .tdbg file"<<endl;
        return false;
    }

    printSnapshot(static_cast<int32_t>(s.currentStep)+1, s.reader.header.stepCount, *snap);
    delete snap;
    return true;


}
bool moveBy(DebugSession &s, int32_t delta)
{
    // delta means kitna step move krna hai or kis direction ma
    if (!s.active_flag){
        cout <<"No session yet. Use: start <source file>"<<endl;
        return false;
    }

    int64_t target = s.currentStep + delta;
    int64_t lastStep = s.reader.header.stepCount - 1;

    if (target < 0 || target > lastStep){
        cout <<"Cannot move there: you are at step " << s.currentStep + 1 << " of " << s.reader.header.stepCount <<endl;
        return false;

    }

    int64_t oldStep = s.currentStep;
    s.currentStep = target;

    // if new snapshot damaged so poorani position wapas set krdo
    if(!showCurrent(s)){
        s.currentStep = oldStep;
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

    string error;
    if(!requestDebugFile(sourcePath.c_str(), TEMP_TDBG_FILE, error)){
        cout <<"Error: " << error << endl;
        return false;
    }

    TdbgReader newReader;
    if(!openTdbg(TEMP_TDBG_FILE, newReader)){
        cout <<"Error: server sent a damaged .tdbg file" << endl;
        remove(TEMP_TDBG_FILE);
        return false;
    }

    Snapshot* first = readSnapshotAt(newReader, 0);
    if(first == nullptr){
        cout <<"Error: server sent a damaged .tdbg file"<<endl;
        closeTdbg(newReader);
        remove(TEMP_TDBG_FILE);
        return false;
    }
    delete first;

    // pehla new file ko final naam da ga. Agar fail ho so it means the old session is safe
    if (rename(TEMP_TDBG_FILE, TDBG_FILE)!=0){
        cout <<"Error: could not save the download as " << TDBG_FILE << endl;
        closeTdbg(newReader);
        remove(TEMP_TDBG_FILE);
        return false;
    }

    // now the new file is ready so we can close old reader and use new reader
    if(s.active_flag){
        closeTdbg(s.reader);
    }

    s.reader = newReader;
    s.currentStep = 0;
    s.active_flag = 1;

    return showCurrent(s);
}
bool cmdNext(DebugSession &s) { return moveBy(s,1); }
bool cmdPrv(DebugSession &s) { return moveBy(s, -1); }
bool cmdJmpNext(DebugSession &s, int32_t n) { 
    if (n < 1 || n>MAX_JUMP){
        cout <<"Error: the jump must be a number from 1 to "<< MAX_JUMP << endl;
        return false;
    }

    return moveBy(s, n);
}
bool cmdJmpPrv(DebugSession &s, int32_t n) {
    if (n < 1 || n>MAX_JUMP){
        cout <<"Error: the jump must be a number from 1 to "<< MAX_JUMP << endl;
        return false;
    }

    return moveBy(s, -n);
 }

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout <<"Step " << step << " of " <<totalSteps<<endl;
    if (s.stackDepth == 0){
        cout <<" (Program finished, the call stack is empty)" << endl;
        return;
    }

    for(int32_t i = s.stackDepth - 1; i>=0; i--){
        const Frame &fr = s.callStack[i];
        cout <<" " << fr.func_name << "(";
        for (int32_t j = 0; j<fr.argc; j++){
            if(j>0){
                cout <<", ";
            }

            cout << fr.argv[j].name <<"=" << fr.argv[j].value;
        }

        cout <<") locals:";
        for (int32_t j = 0; j< fr.localCount; j++){
            cout <<" " << fr.locals[j].name <<"=" << fr.locals[j].value;
        }
        cout << endl;
    }
}
void printHelp()
{
    // Print the commands
    cout <<"Commands:\n" << " start <source file>\n" <<" next\n"<<" prv\n" <<" jmp next <number>\n" << " jmp prv <number>\n" <<" help\n" <<" quit or exit\n";
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================
bool readPositiveNumber(const string &text, int32_t limit, int32_t &number){
    //check jump and port both
    if(text.empty()){
        return false;
    }

    int64_t value = 0;
    for(size_t i = 0; i < text.size(); i++){
        char ch = text[i];
        if (ch < '0' || ch > '9'){
            return false;
        }

        value = value * 10 + (ch - '0');
        if(value > limit){
            return false;
        }
    }

    if (value == 0){
        return false;
    }

    number = static_cast<int32_t> (value);
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

   vector<string> words;
   string word;
  // if space comes, ju word bana hai usse vector ma daal da ga
   for (char ch:line){
    if (ch == ' ' || ch == '\t' || ch== '\r' || ch == '\n' || ch == '\v' || ch == '\f'){
        if (!word.empty()){
            words.push_back(word);
            word.clear();
        }
    }else{
        word = word + ch;
    }

   }
   // last word k baad space nahi honi chahiya
   if(!word.empty()){
    words.push_back(word);
   }

   if(words.empty()){
    return true;
   }

   // Agar pehla word ttdb hai so actual command agla word hai 
   // first batata hai command kis index pr hai and count batata hai command k sath kitna or words hai
   size_t first = 0;
   if(words[0] == PROGRAM_NAME){
    first = 1;
   }

   size_t count = words.size() - first;

   if(count == 1){
    const string& command = words[first];

    if(command == "next"){
        return cmdNext(s);
    }

    if (command == "prv"){
        return cmdPrv(s);
    }
    if(command == "help"){
        printHelp();
        return true;
    }
    if(command == "quit" || command == "exit"){
        quitRequested = true;
        return true;
    }
   }else if(count == 2 && words[first] == "start"){
    return cmdStart(s, words[first+1]);
   }else if(count == 3 && words[first]=="jmp"){
    const string& direction = words[first+1];

    if(direction == "next" || direction == "prv"){
        int32_t n;
        if(!readPositiveNumber(words[first+2], MAX_JUMP, n)){
            cout <<"Error: the jump must be a number from 1 to " << MAX_JUMP << endl;
            return false;
        }

        if(direction == "next"){
            return cmdJmpNext(s, n);
        }
        return cmdJmpPrv(s, n);
    }
   
   }

   cout <<"Unknown command"<<endl;
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

    if(argc != 3){
        cout <<"Usage: " << argv[0] <<" <lb_ip> <lb_port>" << endl;
        return 1;
    }

    sockaddr_in address{};
    if(inet_pton(AF_INET, argv[1], &address.sin_addr)!=1){
        cout <<"Error: invalid IPv4 address" << endl;
        return 1;
    }

    int32_t port;
    if(!readPositiveNumber(argv[2], 65535, port)){
        cout <<"Error: port must be a number from 1 to 65535" << endl;
        return 1;
    }

    LB_HOST = argv[1];
    LB_PORT = port;

    DebugSession session;
    initSession(session);

    cout <<"Time-Travel Debugger" << endl;
    printHelp();

    bool quitRequested = false;
    string line;

    while (!quitRequested){
        cout << PROMPT << flush;
        if(!getline(cin, line)){
            break;
        }

        runCommand(session, line, quitRequested);
    }
    closeSession(session);
    return 0;
}

