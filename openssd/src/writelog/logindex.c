#include "logindex.h"
#include "writelog.h"
#include "xil_printf.h"

#include "../memory_map.h"

void initLogIndex()
{
    uint8_t *nand_page_list = (uint8_t *)NAND_PAGE_LIST_START;
    for (int i = 0; i < LOG_INDEX_BUCKET_COUNT; i++)
    {
        nand_page_list[i] = 0; // mark all nand pages as unused
        for (int j = 0; j < LOG_INDEX_BUCKET_SIZE; j++)
        {
            LOG_INDEX_TABLE[i][j] = MAX_LOG_ENTRIES + 1;
        }
    }
    DEBUG_PRINT("LogIndex Initialized: All entries set to -1.");
}

void insertLogIndex(uint32_t address, uint32_t log_offset)
{
    uint32_t nand_page = address / BYTES_PER_DATA_REGION_OF_SLICE;                      // LPA
    uint32_t page_offset = (address % BYTES_PER_DATA_REGION_OF_SLICE) / CACHELINE_SIZE; // page offset
    uint8_t *nand_page_list = (uint8_t *)NAND_PAGE_LIST_START;

    nand_page_list[nand_page] = 1; // mark the nand page as used
    LOG_INDEX_TABLE[nand_page][page_offset] = log_offset;

    DEBUG_PRINT("Inserted LogIndex: Address = 0x%lx, LogOffset = %lu, nand_page = %u, page_offset = %u", address, log_offset, nand_page, page_offset);
}

uint32_t lookupLogIndex(uint32_t address)
{
    if (address == 0)
    {
        DEBUG_PRINT("Lookup requested for address 0x0. Returning MAX_LOG_ENTRIES + 1.");
        return MAX_LOG_ENTRIES + 1;
    }
    uint32_t nand_page = address / BYTES_PER_DATA_REGION_OF_SLICE;                      // LPA
    uint32_t page_offset = (address % BYTES_PER_DATA_REGION_OF_SLICE) / CACHELINE_SIZE; // page offset
    // uint32_t cacheline_offset = page_offset % CACHELINE_SIZE;                           // cacheline offset

    if (LOG_INDEX_TABLE[nand_page][page_offset] != MAX_LOG_ENTRIES + 1)
    {
    	DEBUG_PRINT("LogIndex Entry: %u", LOG_INDEX_TABLE[nand_page][page_offset]);
        DEBUG_PRINT("LogIndex Lookup Success: Address = 0x%lx, Found at nand_page = %u, page_offset = %u", address, nand_page, page_offset);
        return LOG_INDEX_TABLE[nand_page][page_offset];
    }
    DEBUG_PRINT("LogIndex Lookup Failed: Address = 0x%lx, Not Found", address);
    return MAX_LOG_ENTRIES + 1;
}
