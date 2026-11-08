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
     // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
     // Return false if the connection closes or fails before n bytes arrive.

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
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
     // Use the flag MSG_NOSIGNAL. Return false if the connection fails.

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

inline uint32_t crc_update(uint32_t crc, const char* data, size_t n){
    static const CrcTable table;
    for(size_t i=0; i<n; i++){
        unsigned char byte = static_cast<unsigned char>(data[i]);

        uint32_t table_index = (crc^byte) & 0xFFu;
        crc = table.values[table_index] ^ (crc >> 8);
    }

    return crc;
}

inline uint32_t crc_finish(uint32_t crc){
    return crc ^ 0xFFFFFFFFu;
}

inline void write_u32(char* out, uint32_t value){
    out[0] = static_cast<char>((value >> 24) & 0xFFu);
    out[1] = static_cast<char>((value >> 16) & 0xFFu);
    out[2] = static_cast<char>((value >> 8) & 0xFFu);
    out[3] = static_cast<char>(value & 0xFFu);
}

inline uint32_t read_u32(const char* in){
    uint32_t first = static_cast<unsigned char>(in[0]);
    uint32_t second = static_cast<unsigned char>(in[1]);
    uint32_t third = static_cast<unsigned char>(in[2]);
    uint32_t fourth = static_cast<unsigned char>(in[3]);

    return(first << 24) | (second << 16) | (third << 8) | fourth;

}

class TrailerChecker{
    string pending;
    uint32_t crc;

 public:
    TrailerChecker(){
        crc = CRC_START;
    }

    string feed(const char* data, size_t n){
        pending.append(data, n);

        if(pending.size() <= 4){
            return "";
        }

        size_t bodySize = pending.size() - 4;
        string body = pending.substr(0, bodySize);

        crc = crc_update(crc, body.data(), body.size());
        pending.erase(0, bodySize);
        return body;
    }

    bool verify()const{
        if (pending.size() != 4){
            return false;
        }

        uint32_t receivedCrc = read_u32(pending.data());
        uint32_t calculatedCrc = crc_finish(crc);

        return receivedCrc == calculatedCrc;

    }

    uint32_t checksum() const{
        return crc_finish(crc);
    }



  
};