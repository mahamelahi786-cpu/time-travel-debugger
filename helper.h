#pragma once
#include<cstdint>
#include<string>
#include <sys/socket.h>
#include <cerrno>
#include<cstddef>
using namespace std;

const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t IO_BUFFER_SIZE = 64*1024;

//program states shared by both our client and server
struct Variable{
  string name;
  int32_t value;  
};

struct Frame{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};

struct Snapshot{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;

};

struct TTDBHeader{
    char magic[4];
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files. 

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

inline bool sendAll(int sock, const char* buf, size_t n){
    size_t sent = 0;

    while (sent < n){
        ssize_t result = send(sock, buf + sent, n - sent, MSG_NOSIGNAL);

       
        if (result == -1 && errno == EINTR){
            continue;
        }

        if (result <= 0){
            return false;
        }

        sent = sent + result;

    }

    return true;
}

inline bool recvAll(int sock, char* buf, size_t n){
    size_t got = 0;

    while (got < n){
        ssize_t bytes = recv(sock, buf + got, n - got, 0);

        if (bytes == -1 && errno == EINTR){
            continue;
        }

        if(bytes<=0){
            return false;
        }

        got = got+bytes;

    }

    return true;
}

struct CrcTable{
    uint32_t values[256];

    //constructor
    CrcTable(){
        for (uint32_t i =0; i<256; i++){
            uint32_t value = i;

            for(int bit =0; bit<8; bit++){
                if((value & 1u)!=0){
                    value = (value >> 1) ^ 0xEDB88320u;
                }else{
                    value = value >> 1;
                }
            }
            values[i] = value;
        }
    }
};

const uint32_t  CRC_START = 0xFFFFFFFFu;

inline uint32_t table_index(uint32_t crc, const char* data, size_t n){
    static const CrcTable table;
    for(size_t i=0; i<n; i++){
        unsigned char byte = static_cast<unsigned char>(data[i]);

        uint32_t index = (crc^byte) & 0xFFu;
        crc = table.values[index] ^ (crc >> 8);
    }

    return crc;
}

inline uint32_t crc_finish(uint32_t crc){
    return crc ^ 0xFFFFFFFFu;
}
