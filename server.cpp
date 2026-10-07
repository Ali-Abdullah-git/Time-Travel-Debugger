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
#include <stdexcept>
//#include <unistd.h>
//#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
#include<vector>
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
    Stack() : top{ nullptr }, count{ 0 }
    { // initialize the stack
    }
    ~Stack() { 
        while (top != nullptr) {
            Node* temp = top;
            top = top->next;
            delete temp;
        }
    } // this was missing in original document
    void push(const T& val)
    {
        if (count < MAX_STACK_DEPTH) {
            Node* temp = new Node{ val,top };
            top = temp;
            count++;
            temp = nullptr;
        }
        else
            throw std::overflow_error("Function Stack OverflowError");
        // pushes the value on the stack if max limit is not reached yet.
    }
    T pop()
    {
        if (count == 0)
            throw std::underflow_error("Function Stack Underflow Error");
        Node* temp = top;
        T val = top->data;
        top = top->next;
        delete temp;
        temp = nullptr;
        count--;
        return val;
        // pop the top value on the stack
    }
    T& peek()
    {
        return top->data;
        // returns the top value on the stack
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        Node* temp = top;
        int ct = 0;
        while (temp != nullptr && ct < maxLen) {
            out[ct++] = temp->data;
            temp = temp->next;
        }
        return ct;
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
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
    Timeline() : head{ nullptr }, tail{ nullptr }, stepCount{ 0 }
    {
    }
    void record(Snapshot* s)
    {
        TimelineNode* temp = new TimelineNode{ s,nullptr,nullptr };
        if (head == nullptr) {
            head = tail = temp;
            stepCount++;
            return;
        }
        temp->prev = tail;
        tail->next = temp;
        tail = temp;
        temp = nullptr;
        stepCount++;
        // add record in the timeline
    }
    TimelineNode* begin()
    {
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
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
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
bool readSourceLine(ifstream& in, string& out)
{
    int size = 0;
    while (in.read((char*)&size, sizeof(int))) {
        if (size < 0)
            return false;
        if (size > 0) {
            out.resize(size);
            in.read(out.data(), size);
            return true;
        }
    }
    return false;
    // reads the next nonblank line
}
string firstWord(const string& line)
{
    size_t idx = line.find_first_of(' ');
    if (idx == string::npos)
        return line;
    return line.substr(0, idx);
    // returns first word from the input string
}
string secondWord(const string& line)
{
    size_t first = line.find_first_of(' ');
    if (first == string::npos)
        return "";
    size_t last = line.find_first_of(' ', first + 1);
    if (last == string::npos)
        return line.substr(first + 1);
    return line.substr(first + 1, last - first - 1);
    // returns the second word
}
bool validateProgram(const char* sourcePath)
{
    //using stack here, but I think using a bool is better
    ifstream fin(sourcePath, ios::binary);
    string line;
    Stack<string> func_stack;
    while (readSourceLine(fin, line)) {
        if (firstWord(line) == "func") {
            if (!func_stack.isEmpty())
                return false;
            else
                func_stack.push("func" + secondWord(line));
        }
        else if (firstWord(line) == "func_end") {
            if (func_stack.isEmpty())
                return false;
            else
                func_stack.pop();
        }
    }
    return func_stack.isEmpty();
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    int size = text.size();
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int), 1, f);
    fwrite(text.data(), sizeof(char), size, f);
    return offsetField;
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    int64_t offset = 0;
    int size = 0;
    fread(&offset, sizeof(int64_t), 1, f);
    fread(&size, sizeof(int), 1, f);
    outText.resize(size);
    fread(outText.data(), sizeof(char), size, f);
    return offset;
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    int64_t main_offset = -1;

    ifstream fin(sourcePath, ios::binary);
    FILE* fout = fopen(resolveBinPath, "wb");
    string line;
    int64_t offset = 0;
    vector<string> keywords{"func", "func_end", "call", "set", "add", "mul", "sub", "div"};
    string keyword, func_name;

    while (readSourceLine(fin, line)) {
        keyword = firstWord(line);
        bool flag = false;
        for (const string& key : keywords)
            if (key == keyword) {
                flag = true;
                break;
            }
        if (!flag)
            throw runtime_error("Invalid keyword '" + keyword + "' used");
        func_name = secondWord(line);
        if (keyword == "func") {
            if (funcCount == MAX_FUNCS)
                throw runtime_error("Maximum functions limit exceded");

            for (int i = 0; i < funcCount; i++)
                if (funcArray[i].funcName == func_name)
                    throw runtime_error("Function: " + func_name + " already defined earlier");

            if (func_name == "main")
                main_offset = offset;

            funcArray[funcCount++] = FuncEntry{ func_name, offset };
        }
        else if (keyword == "call") {
            if (patchCount == MAX_PATCHES)
                throw runtime_error("Maximum patch limit exceded");

            patches[patchCount++] = PendingPatch{ offset + 8 + 4 + 5, func_name };
            // 8 bytes for offset, 4 for size, 5 for 'call '
            line = "call 00000000" + line.substr(5 + func_name.size());
        }
        writeResolveRecord(fout, offset, line);
        offset = offset + 8 + 4 + line.size();
    }
    fin.close();
    fclose(fout);

    //Patching functions
    FILE* fout_res = fopen(resolveBinPath, "r+b");
    for (int i = 0; i < patchCount; i++) { //checking for undefined function calls
        string func_name = patches[i].targetFuncName;
        bool func_found = false;
        for (int j = 0; j < funcCount; j++) {
            if (funcArray[j].funcName == func_name) {
                func_found = true;
                fseek(fout_res, patches[i].byteOffsetOfOffsetField, 0);
                string temp = to_string(funcArray[j].byteOffsetInResolveBin);
                temp = string(8 - temp.size(), '0') + temp;
                fwrite(temp.data(), sizeof(char), 8, fout_res);
                break;
            }
        }
        if (!func_found) {
            throw runtime_error("Called an undefined function " + func_name);
        }
    }
    fclose(fout_res);


    if (main_offset == -1)
        throw runtime_error("main() not found");
    return main_offset;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
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
    int32_t ct = 0;
    size_t idx = 0, max_idx = line.size() - 1;
    while (idx <= max_idx) {
        while (line[idx] == ' ')
            idx++;
        if (idx > max_idx)
            break;
        if (ct == maxTokens)
            throw runtime_error("Maximum token limit exceeded in line: " + line);
        size_t start = idx;
        while (line[j] != ' ')
            j++;
        tokens[ct].text = line.substr(idx, start - idx);
        if (ct == 0)
            tokens[ct].type = KEYWORD;
        else if (ct == 1)
            tokens[ct].type = IDENTIFIER;
        else
            tokens[ct].type = PARAM;
        ct++;
        idx = start;
    }
    return ct;
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    Snapshot* snapshot = new Snapshot();
    snapshot->stackDepth = callStack.snapshot_into(snapshot->callStack, MAX_STACK_DEPTH);
    return snapshot;
    // build the snapshot based on the callStack given
}
bool isNum(const string& text) {
    if (text.empty())
        return false;
    size_t idx = 0, max = text.size();

    while (idx < max)
        if (!(text[idx] >= '0' && text[idx] <= '9') && !(idx == 0 && text[idx] == '-'))
            return false;
    return true;
}
Variable* getvariable(Frame& f, const string& text) {
    for (int i = 0; i < f.argc; i++)
        if (f.argv[i].name == text)
            return &f.argv[i];
    for (int i = 0; i < f.localCount; i++)
        if (f.locals[i].name == text)
            return &f.locals[i];
    return nullptr;
}
int getValue(Frame& f, const string& text) {
    if (isNum(text))
        return stoi(text);
    Variable* var = getVariable(f, text);
    if (v != nullptr)
        return var->value;
    throw runtime_error("Undefined variable " + text);
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    FILE* fin = fopen(resolveBinPath, "rb");
    fseek(fin, mainOffset, 0);
    Stack<Frame> callStack;
    vector<string> references[MAX_STACK_DEPTH];
    string line;
    int32_t number_of_tokens = 0;
    Token tokens[MAX_TOKENS];

    readResolveRecord(fin, line);
    Frame main;
    main.name = "main";
    main.returnLine = -1;
    callStack.push(main);

    while (!callStack.isEmpty()) {
        readResolveRecord(fin, line);
        number_of_tokens = tokenizeLine(line, tokens, MAX_TOKENS);
        
        string& keyword = tokens[0].text;
        Frame& current_frame = callStack.peek();

        if (keyword == "set" || keyword == "add" || keyword == "sub" || keyword == "mul" || keyword = "div") {
            if (number_of_tokens != 3)
                throw runtime_error("Invalid line structure in : " + line);
            int val = getValue(current_frame, tokens[2].text);
            Variable* var = getVariable(current_frame, tokens[1].text);

            if (keyword == "set") {
                if (var == nullptr) {
                    var = &curr.locals[curr.localCount++];
                    var->name = tokens[1].text;
                }
                var->value = val;
            }
            else {
                if (var == nullptr)
                    throw runtime_error("Undefined Variable: " + tokens[1].text + " used in line: " + line);
                if (keyword == "add")
                    var->value += val;
                else if (keyword == "sub")
                    var->value -= val;
                else if (keyword == "mul")
                    var->value *= val;
                else if (val != 0)
                    var->value /= val;
                else
                    throw runtime_error("Division by zero error");
            }
        }
        else if (keyword == "call") {
            if (number_of_tokens == 1)
                throw runtime_error("Did not specify which function to call");
            int32_t return_offset = ftell(fin);
            fseek(fin, stoll(tokens[1].text), 0);
            string header;
            readResolveRecord(fin, header);
            Token ht[MAX_TOKENS];
            int32_t ht_count = tokenizeLine(header, ht, MAX_TOKENS);
            if (ht_count != number_of_tokens)
                throw runtime_error("Argument count mismatch calling " + ht[1].text);
            Frame next_frame{};
            next_frame.func_name = ht[1].text;
            next_frame.argc = ht_count - 2;
            next_frame.returnLine = return_offset;
            vector<string> ref;
            for (int i = 0; i < next_frame.argc; i++) {
                next_frame.argv[i].name = ht[i + 2].text;
                next_frame.argv[i].value = getValue(current_frame, tokens[i + 2].text);
                ref.push_back(tokens[i + 2].text);
            }
            references[callStack.depth()] = ref;
            callStack.push(next_frame);
        }
        else if (keyword == "func_end") {
            int32_t depth = callStack.depth() - 1;
            Frame exited_frame = callStack.pop();
            if (!callStack.isEmpty()) {
                Frame& caller = callStack.peek();
                for (int i = 0; i < exited_frame.argc; i++) {
                    Variable* var = getVariable(caller, refs[depth][i]);
                    if (var)
                        var->value = exited_frame.argv[i].value;
                }
                fseek(fin, exited_frame.returnLine, 0);
            }
        }
        else
            throw runtime_error("Invalid instruction: " + keyword + " in line: " + line);

        //build snapshot and record to timeline
    }

    fclose(fin);
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

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}