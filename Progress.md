# Time-Travel-Debugger

## 2nd October
### Completed
- Set up git and server template file on git 
- Gitignore file was created to ignore the unnecessary files
- Implemented stack data structure in server.cpp
- Implemented doubly linked list data structure

## 3rd October
### completed
- Set up vs code and installed linux through wsl 
- Installed extensions in vs code

## 4th October 
### Completed 
- Implemented the complete 0x0 and tested it using test files.
- Started working on 0x1.
- Learned what binary files are.
- Learned how binary files work.
- Learned how to read and write data in binary files.

### Next day
- Implement 0x1 and test it

## 5th October
### Completed 
- Implemented writeResolveRecord & readResolveRecord from 0x1 

### Next day 
- I will be implementing ResolveProgramme

## 7th October
- Completed my resolveProgramme for 0X1 fully and its working and giving the text and offset as checked by main
- In resolve program wherever there was no "call" or "func" or we had something like set or anything else i kept its offset value in resolve.bin as its position in source.bin 
- Moving on to 0X2 completed the Tokenlinesize by 0=keyword, 1=identifier and 2=param
- Tested TokenLinesize with main and it is working correctly and also giving count incremented
- Completed the entire 0x2 with the function buildsnapshot
- Some helper functions to convert string to numbers (is_number, string_to_num, find_variable and get_value)
- Completed execute programme and another function for seeing the timeline snapshots working correctly through doubly linked list named show_timeline

### Next day
- Will be starting with the last stage

## 9th October
- Fixed some errors in my Code and wrote small main to test each function 
- Fixed bugs in executeProgramme as the record was being read twice per loop.
- Started 0x3:completed writeheader and helper function

### Next Day
- Will Finish off 0x3 and run on original main

## 12th October
- Finished 0X3, writeVariable, writeFrame, write_snapshot and writeTdgb
- Tested the .tdgb file 
- Restored the original main so the code full pipeline runs

## 8th November
# Started off with phase 2 on 6th November 
- helper.h: completed my helper.h by writing the common structs for both server and client 
- sendAll and recvAll: loops, to send and receive the complete message
- CRC-32(table method) implemented because to store the values
- Tested in main and ensured that it worked correctly
- Implemented 'TrailerChecker' in 'helper.h' to verify the CRC-32 of data received in chunks. It holds back the latest four bytes while processing the stream. When the stream ends, it reads those four bytes as the sender's checksum and compares it with the CRC calculated from the message.
- Tested it with a message split into two chunks and then one byte at a time. Both tests recovered the original message and passed verification.

- Finished part A of client.cpp. The client can connet to load balancer, upload the source file in chunks and then receive the response.
- Added a status byte before source file and included in CRC. Used SHUT_WR so client can still receive the reply.
- Added requestDebugFile() to call connect, upload and receive function from part A of client.cpp and close the socket after request
- Next i will be starting off with part B.

