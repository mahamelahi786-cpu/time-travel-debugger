// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <sstream>
#include <cerrno>
#include <cstdlib>
#include <netinet/in.h>
#include <sys/time.h>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include "helper.h"
using namespace std;

// ---- Constants ----
// const int32_t MAX_VARS_PER_FRAME = 16;
// const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_STEPS = 1000;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
// const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
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

Timeline::~Timeline() {
    TimelineNode* current = head;
    while(current != nullptr) {
        TimelineNode* nextnode = current->next;
        delete current->data;
        delete current;
        current = nextnode;
    }
}

// struct TTDBHeader
// {
//     char magic[4]; // "TTDB"
//     int32_t version;
//     int32_t stepCount;
//     int64_t indexOffset;
// };

void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t),1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping

// struct FuncEntry
// {
//     string funcName;
//     int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
// };
// struct PendingPatch
// {
//     int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
//     string targetFuncName;
// };



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

bool is_number(const string& word);

bool validateProgram(const char* sourcePath, ostream& error)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
    ifstream fin(sourcePath);
    if(!fin){
        error<<"Cannot Open " <<sourcePath <<endl;
        return false;
    }

    Stack<string> function_stack;
    string line;
    int line_no = 0;

    while(getline(fin, line)){
        line_no++;
        string word = firstWord(line);
        if(word.empty()){
            continue;
        }

        istringstream input(line);
        string words[MAX_TOKENS];
        string part;
        int count = 0;
        while(input >> part){
            if(count == MAX_TOKENS){
                error << "too many parameters or arguments at line " << line_no << endl;
                return false;
            }
            words[count] = part;
            count++;
        }

        if((word == "func" || word == "call") && count < 2){
            error << "missing function name at line " << line_no << endl;
            return false;
        }
        if((word == "set" || word == "add" || word == "sub" ||
            word == "mul" || word == "div") && count != 3){
            error << word << " needs exactly two operands at line " << line_no << endl;
            return false;
        }
        if(word == "func_end" && count != 1){
            error << "func_end takes no operands at line " << line_no << endl;
            return false;
        }
        if(word != "func" && word != "func_end" && word != "call" &&
           word != "set" && word != "add" && word != "sub" &&
           word != "mul" && word != "div"){
            error << "Unknown instruction at line " << line_no << ": " << word << endl;
            return false;
        }
        if(word != "func" && word != "func_end" && function_stack.isEmpty()){
            error << "instruction outside a function at line " << line_no << endl;
            return false;
        }
        if(word == "set" && is_number(words[1])){
            error << "variable name cannot be a number at line " << line_no << endl;
            return false;
        }
        if(word == "func"){
            for(int i = 2; i < count; i++){
                if(is_number(words[i])){
                    error << "parameter name cannot be a number at line " << line_no << endl;
                    return false;
                }
                for(int j = 2; j < i; j++){
                    if(words[i] == words[j]){
                        error << "duplicate parameter at line " << line_no << endl;
                        return false;
                    }
                }
            }
        }

        if (word == "func"){
            if(!function_stack.isEmpty()){
                error <<"at line " << line_no << " : because nested func not allowed"<<endl;
                return false;
            }

            string function_name = secondWord(line);
            if (function_name == ""){
                error <<"at line " << line_no << " : func has no name"<<endl;
                return false;
            }
            function_stack.push(function_name);
        }else if(word == "func_end"){
            if(function_stack.isEmpty()){
            error <<"at line " <<line_no <<" : func_end without any func"<<endl;
            return false;
            }
            function_stack.pop();
        }
    }

    if (!function_stack.isEmpty()){
            error<<"func "<< function_stack.peek() <<" has no func_end"<<endl;
            return false;
    }
    if(fin.bad()){
        error << "could not read the source file" << endl;
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

    if(start < 0 ||
       fwrite(&offsetField, sizeof(int64_t), 1, f) != 1 ||
       fwrite(&size, sizeof(int32_t), 1, f) != 1 ||
       fwrite(text.c_str(), 1, size, f) != static_cast<size_t>(size)){
        return -1;
    }

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

    if(size < 0 || static_cast<uint64_t>(size) > MAX_SOURCE_BYTES){
        return -1;
    }

    outText.resize(size);
    if(size > 0 &&
       fread(&outText[0], 1, size, f) != static_cast<size_t>(size)){
        return -1;
    }
    return offsetfield;


    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}

int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath, ostream& error)
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
        error <<"Cannot open file "<<sourcePath<<endl;
        return -1;
    }

    FILE* f = fopen(resolveBinPath, "wb");
    if (f==nullptr){
        error <<"Cannot open resolve file "<<resolveBinPath<<endl;
        return -1;
    }

    string line;
    int64_t offset = 0;

    while(readSourceLine(fin, line)){
        string word = firstWord(line);
        int64_t recordoffset_pos = writeResolveRecord(f, offset, line);
        if(recordoffset_pos == -1){
            error << "could not write resolve record" << endl;
            fclose(f);
            return -1;
        }

        if (word == "func"){
            string function_name = secondWord(line);

            if(function_name==""){
                error << "function has no name"<<endl;
                fclose(f);
                return -1;
            }

            for(int32_t j = 0; j < funcCount; j++){
                if(funcArray[j].funcName == function_name){
                    error << "function " << function_name << " is defined more than once" << endl;
                    fclose(f);
                    return -1;
                }
            }

            if(funcCount>=MAX_FUNCS){
                error <<"Too many functions"<<endl;
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
                error <<"call has no target"<<endl;
                fclose(f);
                return -1;
            }

            if(patchCount>=MAX_PATCHES){
                error <<"Too many call patch"<<endl;
                fclose(f);
                return -1;
            }

            patches[patchCount].targetFuncName = target_func;
            patches[patchCount].byteOffsetOfOffsetField = recordoffset_pos;
            patchCount++;
        }

        offset = offset + 8 + 4 + line.length();
    }

    if(fin.bad()){
        error << "could not read the source file" << endl;
        fclose(f);
        return -1;
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
            error <<"Function " << patches[i].targetFuncName <<" was not found"<<endl;
            fclose(f);
            return -1;
        }

        if (fseek(f, patches[i].byteOffsetOfOffsetField, SEEK_SET)!=0){
            error <<"Could not seek patch location"<<endl;
            fclose(f);
            return -1;
        }

        if(fwrite(&targetoffset, sizeof(int64_t), 1, f) != 1){
            error << "could not write call target" << endl;
            fclose(f);
            return -1;
        }
    }

    if(fclose(f) != 0){
        error << "could not save resolve file" << endl;
        return -1;
    }

    if(mainoffset==-1){
        error <<"main function was not found"<<endl;
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
    Snapshot* snap = new Snapshot{};
    snap->stackDepth = callStack.snapshot_into(snap->callStack, MAX_STACK_DEPTH);
    return snap;
}

bool is_number(const string& word){
    if (word==""){
        return false;
    }
    size_t idx = 0;
    if (word[0]=='-'){
        if(word.length()==1){
            return false;
        }
        idx = 1;
    }
    while(idx<word.length()){
        if (word[idx]<'0' || word[idx]>'9'){
            return false;
        }
        idx++;
    }
    return true;
}

bool string_to_num(const string& word, int32_t& out){
    if(!is_number(word)){
        return false;
    }
    size_t idx = 0;
    int64_t number = 0;
    bool negative = false;
    if(word[0]=='-'){
        negative = true;
        idx = 1;
    }

    while(idx < word.length()){
        number = number * 10 + (word[idx]-'0');
        if(number > 2147483648LL){
            return false;
        }
        idx++;
    }

    if(negative){
        number = -number;
    }
    if(number < INT32_MIN || number > INT32_MAX){
        return false;
    }
    out = static_cast<int32_t>(number);
    return true;
}

Variable* find_variable(Frame& frame, const string& name){
    for(int i=0; i<frame.argc; i++){
        if(frame.argv[i].name == name){
            return &frame.argv[i];
        }
    }

    for(int i=0; i<frame.localCount; i++){
        if(frame.locals[i].name == name){
            return &frame.locals[i];
        }
    }
    return nullptr;
}

bool get_value(Frame& frame, const string& word, int32_t& result){
    if (is_number(word)){
        return string_to_num(word, result);
    }

    Variable* var = find_variable(frame, word);
    if(var == nullptr){
        return false;
    }
    result = var->value;
    return true;
}

bool executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline, ostream& error)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
    FILE* f = fopen(resolveBinPath, "rb");

    if (f==nullptr){
        error <<"Cannot open file "<< resolveBinPath<<endl;
        return false;
    }

    Stack<Frame> callStack;
    Token tokens[MAX_TOKENS];
    string line;

    if(fseek(f, mainOffset, SEEK_SET) != 0 ||
       readResolveRecord(f, line) == -1){
        error << "could not read main function" << endl;
        fclose(f);
        return false;
    }

    int token_count = tokenizeLine(line, tokens, MAX_TOKENS);
    if(token_count < 2 || tokens[0].text != "func"){
        error << "invalid main function record" << endl;
        fclose(f);
        return false;
    }

    //Create main frame
    Frame mainframe{};
    mainframe.func_name = tokens[1].text;
    mainframe.argc = token_count - 2;
    mainframe.localCount = 0;
    mainframe.returnLine = -1;

    //store main arguments
    for(int i=0; i<mainframe.argc; i++){
        mainframe.argv[i].name = tokens[i+2].text;
        mainframe.argv[i].value = 0;
    }
    callStack.push(mainframe);

    timeline.record(buildSnapshot(callStack));

    while(!callStack.isEmpty()){
        if(timeline.getStepCount() >= MAX_STEPS){
            error << "program has too many steps (limit " << MAX_STEPS << ")" << endl;
            break;
        }
        int64_t record_pos = ftell(f);
        int64_t offset_field = readResolveRecord(f, line);
        if(offset_field==-1){
            error <<"unexpected end of file"<<endl;
            break;
        }

        token_count = tokenizeLine(line, tokens, MAX_TOKENS);
        string keyword = tokens[0].text;

        Frame& current = callStack.peek();

        if(keyword=="set"){
            int32_t value = 0;

            if (token_count<3 || !get_value(current, tokens[2].text, value)){
                error <<"invalid value in set"<<endl;
                break;
            }

            Variable* var = find_variable(current, tokens[1].text);

            if(var == nullptr){
                if(current.localCount>=MAX_VARS_PER_FRAME){
                    error <<"Too many variable in "<<current.func_name<<endl;
                    break;
                }
                //create a new local variable bcz k doesnt exist
                var = &current.locals[current.localCount];

                var->name = tokens[1].text;
                current.localCount++;
            }
            var->value = value;

        }else if(keyword=="add" || keyword == "sub" || keyword == "mul" || keyword == "div"){

            if(token_count<3){
                error<<""<< keyword << " needs two operands"<<endl;
                break;
            }

            Variable* target = find_variable(current, tokens[1].text);
            int32_t operand = 0;

            if(target==nullptr || !get_value(current, tokens[2].text, operand)){
                error <<"undefined variable in: " << line << endl;
                break;
            }

            int64_t a = target->value;
            int64_t b = operand;
            int64_t result = 0;

            if(keyword == "add"){
                result = a + b;
            }else if(keyword == "sub"){
                result = a - b;
            }else if(keyword == "mul"){
                result = a * b;
            }else{
                if(b == 0){
                    error << "division by zero in: " << line << endl;
                    break;
                }
                result = a / b;
            }

            if(result < INT32_MIN || result > INT32_MAX){
                error << "number overflow in: " << line << endl;
                break;
            }
            target->value = static_cast<int32_t>(result);
        }else if(keyword=="call"){
            int32_t arg_count = token_count -2;
            int32_t arg_values[MAX_VARS_PER_FRAME];

            bool get_val = true;
            for(int i=0; i<arg_count; i++){
                if(!get_value(current, tokens[i+2].text, arg_values[i])){
                    error <<""<<tokens[i+2].text<<" is not defined"<<endl;
                    get_val= false;
                    break;
                }

            }

            if(get_val==false){
                break;
            }

            if(fseek(f, offset_field, SEEK_SET) != 0 || readResolveRecord(f, line) == -1){
                error << "could not read called function" << endl;
                break;
            }

            int header_ct = tokenizeLine(line, tokens, MAX_TOKENS);
            if(header_ct < 2 || tokens[0].text != "func"){
                error << "invalid called function record" << endl;
                break;
            }
            int param_count = header_ct-2;

            if(param_count != arg_count){
                error <<""<<tokens[1].text<<" needs "<<param_count<<" arguments but got "<< arg_count<<endl;
                break;
            }
            Frame newframe{};
            newframe.func_name = tokens[1].text;
            newframe.argc = param_count;
            newframe.localCount = 0;
            newframe.returnLine = record_pos;

            for(int i=0; i<param_count; i++){
                newframe.argv[i].name = tokens[i+2].text;
                newframe.argv[i].value = arg_values[i];
            }

            if(callStack.depth()>=MAX_STACK_DEPTH){
                error << "Stack is full (too many nested calls)" << endl;
                break;
            }

            callStack.push(newframe);



        }else if(keyword=="func_end"){
            Frame finished = callStack.pop();

            if(finished.returnLine!=-1){
                if(fseek(f, finished.returnLine, SEEK_SET) != 0 ||
                   readResolveRecord(f, line) == -1 ||
                   tokenizeLine(line, tokens, MAX_TOKENS) != finished.argc + 2){
                    error << "could not read return location" << endl;
                    fclose(f);
                    return false;
                }


                Frame& caller = callStack.peek();
                for(int i=0; i<finished.argc; i++){
                    Variable* var = find_variable(caller, tokens[i+2].text);
                    if(var != nullptr){
                        var->value = finished.argv[i].value;
                    }
                 }
            }

        }else{
            error<<"Unknown instruction: "<< line << endl;
            break;
        }

        timeline.record(buildSnapshot(callStack));
    }

    bool finished = callStack.isEmpty();
    if(fclose(f) != 0){
        error << "could not close the resolved program" << endl;
        return false;
    }

    return finished;
}

void show_timeline(Timeline& t){
    cout<<"Total steps = "<< t.getStepCount()<<endl;
    if(t.getStepCount()==0){
        return;
    }
    //first node of doubly list
    TimelineNode* step = t.begin();

    int step_ct = 1;

    while(step!=nullptr){
        Snapshot* snap = step->data;
        cout <<"Step: "<<step_ct<< " (Depth " << snap->stackDepth << ")" <<endl;

        for(int i = snap->stackDepth-1; i>=0; i--){
            Frame& f = snap->callStack[i];
            cout<<" "<<f.func_name<<" args: ";
            for(int j=0; j<f.argc; j++){
                cout <<" " << f.argv[j].name << " = " << f.argv[j].value;
            }
            cout <<" locals: ";
            for(int j=0; j< f.localCount; j++){
                cout << " " << f.locals[j].name<<" = "<<f.locals[j].value;
            }
            cout <<endl;
        }
        step = step->next;
        step_ct++;
    }

}


void write_string(FILE* f, const string& txt){
    int32_t size = txt.length();
    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(txt.c_str(), 1, size, f);

}

void writeVariable(FILE* f, const Variable& v){
    write_string(f, v.name);
    fwrite(&v.value, sizeof(int32_t), 1, f);

}

void writeFrame(FILE* f, const Frame& frame){
    write_string(f, frame.func_name);

    fwrite(&frame.argc, sizeof(int32_t), 1, f);

    for (int i=0; i<frame.argc; i++){
        writeVariable(f, frame.argv[i]);
    }

    fwrite(&frame.returnLine, sizeof(int32_t), 1, f);

    fwrite(&frame.localCount, sizeof(int32_t), 1, f);

    for(int i=0; i<frame.localCount; i++){
        writeVariable(f, frame.locals[i]);
    }
}

void write_snapshot(FILE* f, const Snapshot& s){
    fwrite(&s.stackDepth, sizeof(int32_t), 1, f);

    for(int i=0; i<s.stackDepth; i++){
        writeFrame(f, s.callStack[i]);
    }
}

// PASS 0x3: SERIALIZE TIMELINE
bool writeTdbg(Timeline& timeline, const char* tdbgPath, ostream& error)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
    FILE* f = fopen(tdbgPath, "wb");
    if(f == nullptr){
        error << "Cannot open "<< tdbgPath << endl;
        return false;
    }

    int32_t steps = timeline.getStepCount();

    TTDBHeader header;
    header.magic[0] = 'T';
    header.magic[1] = 'T';
    header.magic[2] = 'D';
    header.magic[3] = 'B';

    header.version = 1;
    header.stepCount = steps;
    header.indexOffset = 0;
    writeHeader(f, header);

    int64_t* index = new int64_t[steps];

    if(steps>0){
        TimelineNode* Node = timeline.begin();
        int32_t i = 0;

        while(Node != nullptr){
            index[i] = ftell(f);
            write_snapshot(f, *(Node->data));
            Node = Node->next;
            i++;
        }
    }

    int64_t index_start = ftell(f);
    bool saved = index_start >= 0 && !ferror(f);

    if(saved){
        saved = fwrite(index, sizeof(int64_t), steps, f) == static_cast<size_t>(steps);
    }

    delete[] index;

    if(saved){
        header.indexOffset = index_start;
        if(fseek(f, 0, SEEK_SET) != 0){
            saved = false;
        }else{
            writeHeader(f, header);
            saved = !ferror(f);
        }
    }

    if(fclose(f) != 0){
        saved = false;
    }

    if(!saved){
        error << "could not save the debug file" << endl;
    }

    return saved;
}

bool sendServerError(int sock, string message)
{
    while(!message.empty() && (message.back() == '\n' || message.back() == '\r')){
        message.pop_back();
    }
    char status = 1;
    uint32_t crc = crc_update(CRC_START, &status, 1);
    crc = crc_update(crc, message.data(), message.size());

    char trailer[4];
    write_u32(trailer, crc_finish(crc));

    return sendAll(sock, &status, 1) &&
           sendAll(sock, message.data(), message.size()) &&
           sendAll(sock, trailer, sizeof(trailer));
}

bool receiveServerUpload(int sock, string& error)
{
    FILE* file = fopen("source.bin", "wb");
    if(file == nullptr){
        error = "could not open source.bin";
    }

    TrailerChecker checker;
    char buffer[IO_BUFFER_SIZE];
    bool sawStatus = false;
    uint64_t sourceBytes = 0;

    while(true){
        ssize_t got = recv(sock, buffer, sizeof(buffer), 0);
        if(got < 0){
            if(errno == EINTR){
                continue;
            }
            if(error.empty()){
                if(errno == EAGAIN || errno == EWOULDBLOCK){
                    error = "timed out waiting for upload";
                }else{
                    error = "could not receive upload";
                }
            }
            break;
        }

        if(got == 0){
            if(error.empty()){
                if(!sawStatus || !checker.verify()){
                    error = "upload checksum mismatch";
                }else if(sourceBytes == 0){
                    error = "empty source file";
                }
            }
            break;
        }

        if(!error.empty()){
            continue;
        }

        string bytes = checker.feed(buffer, static_cast<size_t>(got));
        if(bytes.empty()){
            continue;
        }

        size_t start = 0;
        if(!sawStatus){
            sawStatus = true;
            start = 1;
            if(bytes[0] != 0){
                error = "invalid upload status";
                continue;
            }
        }

        size_t count = bytes.size() - start;
        if(count > MAX_SOURCE_BYTES - sourceBytes){
            error = "source file is too large";
        }else if(fwrite(bytes.data() + start, 1, count, file) != count){
            error = "could not write source.bin";
        }else{
            sourceBytes += count;
        }
    }

    if(file != nullptr && fclose(file) != 0 && error.empty()){
        error = "could not save source.bin";
    }

    return error.empty();
}

bool sendServerDebugFile(int sock, FILE* file)
{
    char status = 0;
    uint32_t crc = crc_update(CRC_START, &status, 1);

    if(!sendAll(sock, &status, 1)){
        return false;
    }

    char buffer[IO_BUFFER_SIZE];
    size_t got;
    while((got = fread(buffer, 1, sizeof(buffer), file)) > 0){
        crc = crc_update(crc, buffer, got);
        if(!sendAll(sock, buffer, got)){
            return false;
        }
    }

    if(ferror(file)){
        return false;
    }

    char trailer[4];
    write_u32(trailer, crc_finish(crc));
    return sendAll(sock, trailer, sizeof(trailer));
}

bool serveServerClient(int sock)
{
    string uploadError;
    if(!receiveServerUpload(sock, uploadError)){
        return sendServerError(sock, uploadError);
    }

    ostringstream error;

    if(!validateProgram("source.bin", error)){
        return sendServerError(sock, error.str());
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin", error);
    if(mainOffset == -1){
        return sendServerError(sock, error.str());
    }

    Timeline timeline;
    if(!executeProgram("resolve.bin", mainOffset, timeline, error)){
        if(error.str().empty()){
            return sendServerError(sock, "program execution failed");
        }
        return sendServerError(sock, error.str());
    }

    if(remove("session.tdbg") != 0 && errno != ENOENT){
        return sendServerError(sock, "could not clear the previous debug file");
    }

    if(!writeTdbg(timeline, "session.tdbg", error)){
        return sendServerError(sock, error.str());
    }

    FILE* file = fopen("session.tdbg", "rb");
    if(file == nullptr){
        return sendServerError(sock, "could not open the generated debug file");
    }

    bool sent = sendServerDebugFile(sock, file);
    fclose(file);
    return sent;
}

// main section
int main(int argc, char* argv[])
{
    if(argc != 2){
        cerr << "Usage: " << argv[0] << " <port>" << endl;
        return 1;
    }

    char* end = nullptr;
    errno = 0;
    long port = strtol(argv[1], &end, 10);
    if(errno != 0 || end == argv[1] || *end != '\0' ||
       port < 1 || port > 65535){
        cerr << "Error: invalid server port" << endl;
        return 1;
    }

    int listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if(listenSock < 0){
        perror("socket");
        return 1;
    }

    int reuse = 1;
    if(setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
                  &reuse, sizeof(reuse)) < 0){
        perror("setsockopt SO_REUSEADDR");
        close(listenSock);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));

    if(bind(listenSock, (sockaddr*)&address, sizeof(address)) < 0){
        perror("bind");
        close(listenSock);
        return 1;
    }

    if(listen(listenSock, 8) < 0){
        perror("listen");
        close(listenSock);
        return 1;
    }

    cout << "Server listening on port " << port << endl;

    while(true){
        int sock = accept(listenSock, nullptr, nullptr);
        if(sock < 0){
            if(errno == EINTR || errno == ECONNABORTED){
                continue;
            }
            perror("accept");
            break;
        }

        timeval timeout{};
        timeout.tv_sec = SOCKET_TIMEOUT_SEC;
        if(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 || setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0){
            perror("setsockopt client timeout");
            close(sock);
            continue;
        }

        // send an error response instead of a .tdbg file
        try{
            if(!serveServerClient(sock)){
                cerr << "Could not finish sending the reply" << endl;
            }
        }catch(const exception& e){
            if(!sendServerError(sock, e.what())){
                cerr << "Could not send the server error" << endl;
            }
        }

        close(sock);
        remove("source.bin");
        remove("resolve.bin");
        remove("session.tdbg");
    }

    close(listenSock);
    return 1;
}