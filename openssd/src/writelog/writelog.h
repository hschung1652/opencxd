#ifndef WRITELOG_H_
#define WRITELOG_H_

#include "xil_types.h"
#include "xil_printf.h"
#include "xtime_l.h"

//#define DEBUG_ON
#ifdef DEBUG_ON
#define DEBUG_PRINT(fmt, ...) xil_printf("[DEBUG] " fmt "\r\n", ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...)
#endif

//#define MAX_LOG_ENTRIES 131072   // total size is equal to 4 nand pages
#define MAX_LOG_ENTRIES 932067      // Skybyte writelog size (64MB)
//#define MAX_LOG_ENTRIES 128      // for development-2
//#define MAX_LOG_ENTRIES 64       // for development-3
#define CACHELINE_SIZE 64

// if MAX_LOG_ENTRIES is set to 131072, then total size of write log is 12MB

typedef struct {
    int opcode;  // READ or WRITE
    uint32_t address;
    uint8_t payload[CACHELINE_SIZE];  // payload
} MemoryRequest;

typedef struct {
    uint32_t addressStart;
    uint32_t addressEnd;
    uint8_t payload[CACHELINE_SIZE];  // payload
} WriteLogEntry;

typedef struct {
    WriteLogEntry entries[MAX_LOG_ENTRIES];
    int count;  // # of logged entries
} WriteLog;

void initWriteLog();
void logWrite(uint32_t addressStart, uint32_t addressEnd);
void logCompaction();

#endif // WRITELOG_H_
