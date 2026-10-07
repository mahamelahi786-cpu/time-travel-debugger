// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { // initialize the stack
        top = nullptr;
        count = 0;

    }

    ~Stack() {
        while (top != nullptr) {
            Node* temp = top;
            top = top->next;
            delete temp;
        }
    }

    void push(const T& val)
    {// pushes the value on the stack if max limit is not reached yet.
        if (count == MAX_STACK_DEPTH) {
            throw (overflow_error("Stack is FULL! Can't push more elements"));
        }

        Node* newnode = new Node;
        newnode->data = val;
        newnode->next = top;
        top = newnode;
        count++;
    }

    T pop()
    {
        if (top == nullptr) {
            throw(underflow_error("Stack is empty, nothing can be popped"));
            return T();
        }
        Node* temp = top;
        T val = temp->data;
        top = top->next;
        delete temp;
        count--;
        return val;
        // pop the top value on the stack

    }
    T& peek()
    {
        // returns the top value on the stack
         if (top == nullptr) {
            throw(underflow_error("Stack is empty, nothing can be peeked"));
            
        }
        return top->data;
    }
    bool isEmpty()
    {
        if (top == nullptr) {
            return true;
        }
        return false;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        int32_t idx = 0;
        Node* current = top;
        while (current != nullptr && idx < maxLen) {
            out[idx] = current->data;
            current = current->next;
            idx++;
        }
        return idx;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        stepCount = 0;

    }

    ~Timeline();

    void record(Snapshot* s)
    {
        // add record in the timeline
        TimelineNode* newnode = new TimelineNode;
        newnode->data = s;
        newnode->next = nullptr;
        newnode->prev = tail;

        if (head == nullptr) {
            head = newnode;
            tail = newnode;
        }
        else {
            tail->next = newnode;
            tail = newnode;
        }
        stepCount++;
    }

    TimelineNode* begin()
    {
        if (stepCount==0 || head==nullptr){
            throw(underflow_error("It is empty"));
        }
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
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

Timeline::~Timeline() {
    TimelineNode* current = head;
    while(current != nullptr) {
        TimelineNode* nextnode = current->next;
        delete current->data;
        delete current;
        current = nextnode;
    }
}

struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool is_white_space(char c){
    if (c=='\t' || c==' ' || c=='\r'){
        return true;
    }
    return false;

}

bool readSourceLine(ifstream& in, string& out)
{
   // reads the next nonblank line
   string line;
   while(getline(in, line)){
    int size = line.length();
    int idx = 0;
    while(idx<size && is_white_space(line[idx])){
        idx++;
    }
    if (idx < size){
        out = line;
        return true;
    }
    
   }
   
   return false;
}
string firstWord(const string& line)
{
    // returns first word from the input string
    int size = line.length();
    int start = 0;
    while(start<size && is_white_space(line[start])){
        start++;
    }

    if(start == size){
        return "";
    }

    int end = start;

    while(end<size && !is_white_space(line[end])){
        end++;

    }

    return line.substr(start, end-start);

}

string secondWord(const string& line)
{
    int size = line.length();
    int start = 0;
    while(start<size && is_white_space(line[start])){
        start++;
    }

    while(start<size && !is_white_space(line[start])){
        start++;
    }

    while(start<size && is_white_space(line[start])){
        start++;
    }

    if(start == size){
        return "";
    }

    int end = start;

    while(end<size && !is_white_space(line[end])){
        end++;

    }

    return line.substr(start, end-start);

    // returns the second word
}

bool validateProgram(const char* sourcePath)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
    ifstream fin(sourcePath);
    if(!fin){
        cout<<"Error: Cannot Open " <<sourcePath <<endl;
        return false;
    }

    Stack<string> function_stack;
    string line;
    int line_no = 0;

    while(readSourceLine(fin, line)){
        line_no++;
        string word = firstWord(line);

        if (word == "func"){
            if(!function_stack.isEmpty()){
                cout <<"Error at line " << line_no << " : because nested func not allowed"<<endl;
                return false;
            }

            string function_name = secondWord(line);
            if (function_name == ""){
                cout <<"Error at line " << line_no << " : func has no name"<<endl;
                return false;
            }
            function_stack.push(function_name);
        }else if(word == "func_end"){
            if(function_stack.isEmpty()){
            cout <<"Error at line " <<line_no <<" : func_end without any func"<<endl;
            return false;
            }
            function_stack.pop();
        }  
    }

    if (!function_stack.isEmpty()){
            cout<<"Error: func "<< function_stack.peek() <<" has no func_end"<<endl;
            return false;
    }
    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    int64_t start = ftell(f);
    int32_t size = text.length();

    fwrite(&offsetField, sizeof(int64_t), 1,f);
    fwrite(&size, sizeof(int32_t), 1,f);
    fwrite(text.c_str(), 1, size, f);

    return start;
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    int64_t offsetfield;
    int32_t size;

    if(fread(&offsetfield, sizeof(int64_t), 1, f)!=1){
        return -1;
    }

    if(fread(&size, sizeof(int32_t), 1, f)!=1){
        return -1;
    }

    char* letters = new char[size+1];
    fread(letters, 1, size, f);
    letters[size] = '\0';
    outText = letters;
    delete[] letters;
    return offsetfield;


    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}

int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
    
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    int64_t mainoffset = -1;
    ifstream fin(sourcePath);
    if(!fin){
        cout <<"Error: Cannot open file "<<sourcePath<<endl;
        return -1;
    }

    FILE* f = fopen(resolveBinPath, "wb");
    if (f==nullptr){
        cout <<"Error: Cannot open resolve file "<<resolveBinPath<<endl;
        return -1;
    }

    string line;

    while(readSourceLine(fin, line)){
        string word = firstWord(line);
        int64_t pos = ftell(f);
        int64_t recordoffset_pos = writeResolveRecord(f, pos, line);

        if (word == "func"){
            string function_name = secondWord(line);

            if(function_name==""){
                cout << "Error: function has no name"<<endl;
                fclose(f);
                return -1;
            }

            if(funcCount>=MAX_FUNCS){
                cout <<"Error: Too many functions"<<endl;
                fclose(f);
                return -1;
            }

            funcArray[funcCount].funcName = function_name;
            funcArray[funcCount].byteOffsetInResolveBin = recordoffset_pos;
            funcCount++;

            if (function_name == "main"){
                mainoffset = recordoffset_pos;
            }

        }else if (word == "call"){
            string target_func = secondWord(line);

            if (target_func==""){
                cout <<"Error: call has no target"<<endl;
                fclose(f);
                return -1;
            }

            if(patchCount>=MAX_PATCHES){
                cout <<"Error: Too many call patch"<<endl;
                fclose(f);
                return -1;
            }

            patches[patchCount].targetFuncName = target_func;
            patches[patchCount].byteOffsetOfOffsetField = recordoffset_pos;
            patchCount++;
        }
    }

    for(int32_t i =0; i<patchCount; i++){
        int64_t targetoffset = -1;

        for(int32_t j = 0; j<funcCount; j++){
            if (funcArray[j].funcName == patches[i].targetFuncName){
                targetoffset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }

        if (targetoffset==-1){
            cout <<"Error: Function " << patches[i].targetFuncName <<" was not found"<<endl;
            fclose(f);
            return -1;
        }

        if (fseek(f, patches[i].byteOffsetOfOffsetField, SEEK_SET)!=0){
            cout <<"Error: Could not seek patch location"<<endl;
            fclose(f);
            return -1;
        }

        fwrite(&targetoffset, sizeof(int64_t), 1, f);
    }
    fclose(f);

    if(mainoffset==-1){
        cout <<"Error: main function was not found"<<endl;
        return -1;
    }

    return mainoffset;

}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated

    int size = line.length();
    int idx = 0;
    int32_t token_count = 0;

    while (idx < size && token_count < maxTokens){
        while(idx<size && is_white_space(line[idx])){
            idx++;
        }

        if (idx == size){
            break;
        }

        int start = idx;
        while (idx<size && !is_white_space(line[idx])){
            idx++; 
        }

        tokens[token_count].text = line.substr(start, idx-start);

        if(token_count==0){
            tokens[token_count].type = KEYWORD;
        }else if (token_count==1){
            tokens[token_count].type = IDENTIFIER;
        }else{
            tokens[token_count].type = PARAM;
        }
        token_count++;
    }
    return token_count;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{
   
    Stack<int> s;
    s.push(1);
    s.push(2);
    s.push(3);
    cout << s.pop() << endl;
    cout << s.pop() << endl;
    cout << s.pop() << endl;
    cout << s.isEmpty() << endl;
    try {
        s.pop();
    }
    catch (underflow_error& e) {
        cout << "Caught: " << e.what() << endl;
    }

    cout <<"--------------------------------------------"<<endl;
    Timeline t;
    for (int i = 1; i <= 3; i++) {
        Snapshot* snap = new Snapshot;
        snap->stackDepth = i * 10;
        t.record(snap);
    }
    cout << "steps = " << t.getStepCount() << endl;

    TimelineNode* current = t.begin();
    while (current->next != nullptr) {
        cout << current->data->stackDepth << " ";
        current = current->next;
    }
    cout << current->data->stackDepth << endl;
    while (current != nullptr) {
        cout << current->data->stackDepth << " ";
        current = current->prev;
    }
    cout << endl;
    cout <<"--------------------------------------------"<<endl;
    cout<<"validation testing"<<endl;
    bool validate;
    validate = validateProgram("tests/valid.bin");
    cout <<"valid: " << validate << endl;
    validate = validateProgram("tests/bad_missing_end.bin");
    cout <<"missing end: " << validate << endl;
    validate = validateProgram("tests/bad_extra_end.bin");
    cout <<"extra end: " << validate << endl;
    
    FILE* binary_writing = fopen("binary_testing/test_resolve.bin", "wb");
    writeResolveRecord(binary_writing, 0, "func foo b");
    writeResolveRecord(binary_writing, 22, "set a 10");
    writeResolveRecord(binary_writing, 42, "func_end");

    fclose(binary_writing);

    FILE* reading_binary = fopen("binary_testing/test_resolve.bin", "rb");

    if (reading_binary==nullptr){
        cout <<"Cannot open file"<<endl;
    }
    string text;
    int64_t offset_values;

    offset_values = readResolveRecord(reading_binary,text);

    while(offset_values != -1){
        cout<<"Offset: "<<offset_values<<endl;
        cout <<"Text: " << text<<endl;
        cout<<endl;
        
        offset_values = readResolveRecord(reading_binary, text);
    }

    fclose(reading_binary);

    cout <<"--------------------------------------------"<<endl;
    cout <<"Program resolve test"<<endl;
    int64_t main_pos = resolveProgram("tests/resolve_test.bin", "binary_testing/resolve.bin");
    cout <<"main is at:"<<main_pos<<endl;

    FILE* rf = fopen("binary_testing/resolve.bin", "rb");

    if (rf == nullptr){
        cout <<"Cannot open resolve.bin"<<endl;
    }
    string rtext;
    int64_t roff = readResolveRecord(rf, rtext);
    
    while(roff!=-1){
        cout <<"Offset: "<<roff<<" text=" << rtext << endl;
        roff = readResolveRecord(rf, rtext);
    }

    fclose(rf);

    cout << resolveProgram("tests/no_main.bin", "binary_testing/resolve2.bin")<<endl;

    cout <<"--------------------------------------------"<<endl;
    cout <<"tokensize test"<<endl;
    Token tokens[MAX_TOKENS];
    int32_t n = tokenizeLine ("func foo b", tokens, MAX_TOKENS);
    cout <<"count= " <<n<<endl;
    for (int i=0; i<n; i++){
        cout <<tokens[i].type <<" " << tokens[i].text<<endl;
    } 
    n= tokenizeLine("func_end", tokens, MAX_TOKENS);
    cout <<"count= " << n << endl;
    cout <<"--------------------------------------------"<<endl;
   

    return 0;

    // if (!validateProgram("source.bin"))
    // {
    //     // send an error response instead of a .tdbg file
    //     return 1;
    // }

    // int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    // Timeline timeline;
    // executeProgram("resolve.bin", mainOffset, timeline);

    // writeTdbg(timeline, "session.tdbg");

    // return 0;
}