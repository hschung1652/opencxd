#ifndef LOGINDEX_H_
#define LOGINDEX_H_

#include "xil_types.h"

#define LOG_INDEX_BUCKET_COUNT 262144 // 1st-Level table size
#define LOG_INDEX_BUCKET_SIZE 256     // # of 2nd-level entries per bucket

// if LOG_INDEX_BUCKET_SIZE is set to 256, then no hash conflict occurs for 2nd-level
// in this case, the total size of log index would be about 256MB

#define LOG_INDEX_TABLE ((uint32_t (*)[LOG_INDEX_BUCKET_SIZE])LOG_INDEX_START_ADDR)

void initLogIndex();
void insertLogIndex(uint32_t address, uint32_t log_offset);
uint32_t lookupLogIndex(uint32_t address);

#endif // LOGINDEX_H_
