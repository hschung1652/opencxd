#include "cxl_ssd.h"
#include "xtime_l.h"
#include <assert.h>
#include "../memory_map.h"

#include "../ftl_config.h"
#include "../request_transform.h"

#include "../writelog/writelog.h"
#include "../writelog/logindex.h"

extern uint32_t write_log_count;
extern WriteLogEntry *write_log_entries;
struct ssd_stat stats;
int AccessCnt = 0;
int NetAggCnt = 0;

static unsigned int ReadNandPageAndLoadToDataCache(const int cmdSlotTag, const unsigned int logicalSliceAddr);

unsigned short MemWriteRequestSingle(uint32_t addressStart, uint32_t addressEnd, NVME_IO_COMMAND *nvmeIOCmd, uint64_t *time)
{
	XTime tstart, tend;
	XTime_GetTime(&tstart);
    // if the Write Log is full, then perform Log Compaction
    if (write_log_count >= MAX_LOG_ENTRIES)
    {
        DEBUG_PRINT("Write log is full. Triggering log compaction...");
        stats.log_compaction_count++;
        logCompaction();

        if (write_log_count >= MAX_LOG_ENTRIES)
        {
            xil_printf("[ERROR] Log Compaction completed, but Write Log is still full.\r\n");
            assert(0);
        }
        XTime_GetTime(&tend);
        *time = tend - tstart;
        xil_printf("Log Compaction complete: %llu\r\n", *time);
        XTime_GetTime(&tstart);
        logWrite(addressStart, addressEnd);
		XTime_GetTime(&tend);
		*time += tend - tstart;
		return 6;
    }
    // append to the Write Log
    logWrite(addressStart, addressEnd);
    XTime_GetTime(&tend);
    *time = tend - tstart;
    return 3;
}

unsigned short MemReadRequestSingle(uint32_t addressStart, uint32_t addressEnd, uint64_t *time, uint64_t *tmiss)
{
	XTime tstart, tend, td_start, td_end;
	//XTime tcache_start, tcache_end, tlog_start, tlog_end;
	unsigned short result;
	XTime_GetTime(&tstart);
	// if a corresponding nand page is cached (r1)
    unsigned int logical_page_num = addressEnd / BYTES_PER_DATA_REGION_OF_SLICE;

    //XTime_GetTime(&tcache_start);
    unsigned int data_cache_entry = CheckDataBufHitWithLSA(logical_page_num);
    //XTime_GetTime(&tcache_end);
    //xil_printf("CHECK DATA BUFFER: %llu\r\n", tcache_end - tcache_start);

    if (data_cache_entry != DATA_BUF_FAIL)
    {
        unsigned int data_cache_addr = DATA_BUFFER_BASE_ADDR + data_cache_entry * BYTES_PER_DATA_REGION_OF_SLICE;
        unsigned int offset_in_page = addressEnd % BYTES_PER_DATA_REGION_OF_SLICE; // not page offset
        unsigned int cacheline_offset = offset_in_page / CACHELINE_SIZE;           // this is page offset
        uint8_t *nand_cache_ptr = (uint8_t *)(data_cache_addr + cacheline_offset * CACHELINE_SIZE);
        memcpy(DUMMY_CACHE_READ, nand_cache_ptr, CACHELINE_SIZE);
        XTime_GetTime(&tend);
        stats.byte_rissue_count++;
        stats.byte_rissue_traffic += CACHELINE_SIZE;
        stats.nand_read_user += CACHELINE_SIZE;
        DEBUG_PRINT("[r1] Data Cache hit: logical_page_num=%u, data_cache_entry=%u, cacheline_offset=%u, addr=0x%x", logical_page_num, data_cache_entry, cacheline_offset, addressEnd);
        *time = tend - tstart;
		return 4;
    }

    // if a given cacheline is in the Write Log (r2)
    //XTime_GetTime(&tlog_start);
    uint32_t log_offset = lookupLogIndex(addressEnd);
    //XTime_GetTime(&tlog_end);
    //xil_printf("CHECK WRITE LOG: %llu\r\n", tlog_end - tlog_start);

    if (log_offset != MAX_LOG_ENTRIES + 1)
    {
        WriteLogEntry *log_entry = &write_log_entries[log_offset];
        memcpy(DUMMY_CACHE_READ, log_entry->payload, CACHELINE_SIZE);
        XTime_GetTime(&tend);
        stats.byte_rissue_count++;
		stats.byte_rissue_traffic += CACHELINE_SIZE;
		stats.nand_read_user += CACHELINE_SIZE;
        DEBUG_PRINT("[r2] Found in Write Log: address=0x%x, log_offset=%u", addressEnd, log_offset);
        *time = tend - tstart;
		return 2;
    }

    XTime_GetTime(&td_start);
    // else, read the nand page and load it onto the data cache (r3)
    unsigned int data_cache_addr = ReadNandPageAndLoadToDataCache(0, logical_page_num); // cmdSlotTag = 0
    XTime_GetTime(&td_end);
    if (data_cache_addr == 0)
    {
        DEBUG_PRINT("[r3] NAND read failed: logical_page_num=%u (page never written)", logical_page_num);
        return -1; // if the nand page is never written
    }

    unsigned int offset_in_page = addressEnd % BYTES_PER_DATA_REGION_OF_SLICE;
    unsigned int cacheline_offset = offset_in_page / CACHELINE_SIZE;                            // not page offset
    uint8_t *nand_cache_ptr = (uint8_t *)(data_cache_addr + cacheline_offset * CACHELINE_SIZE); // this is page offset

    memcpy(DUMMY_CACHE_READ, nand_cache_ptr, CACHELINE_SIZE);
    XTime_GetTime(&tend);
    stats.block_rissue_traffic += BYTES_PER_DATA_REGION_OF_SLICE;
    stats.block_rissue_count++;
    stats.nand_read_internal += BYTES_PER_DATA_REGION_OF_SLICE;
    stats.nand_read_user += CACHELINE_SIZE;
    stats.byte_rissue_count++;
	stats.byte_rissue_traffic += CACHELINE_SIZE;
    DEBUG_PRINT("[r3] Read from NAND and cached: logical_page_num=%u, cacheline_offset=%u, addr=0x%x", logical_page_num, cacheline_offset, addressEnd);
    *time = tend - tstart - 14 ;
    *tmiss = *time - (td_end - td_start - 14);
	return 5;
}

/*
int MemWriteRequest(uint32_t address, const uint8_t *payload) {
    // if the Write Log is full, then perform Log Compaction
    if (write_log_count >= MAX_LOG_ENTRIES) {
        DEBUG_PRINT("Write log is full. Triggering log compaction...");
        logCompaction();

        if (write_log_count >= MAX_LOG_ENTRIES) {
            xil_printf("[ERROR] Log Compaction completed, but Write Log is still full.\r\n");
            assert(0);
        }
    }

    // append to the Write Log
    logWrite(address, payload);
    return 0;
}

int MemReadRequest(uint32_t address, uint8_t *output) {
    // if a corresponding nand page is cached (r1)
    unsigned int logical_page_num = address / BYTES_PER_DATA_REGION_OF_SLICE;
    unsigned int data_cache_entry = CheckDataBufHitWithLSA(logical_page_num);
    if (data_cache_entry != DATA_BUF_FAIL) {
        unsigned int data_cache_addr = DATA_BUFFER_BASE_ADDR + data_cache_entry * BYTES_PER_DATA_REGION_OF_SLICE;
        unsigned int offset_in_page = address % BYTES_PER_DATA_REGION_OF_SLICE;
        unsigned int cacheline_offset = offset_in_page / CACHELINE_SIZE;

        uint8_t *nand_cache_ptr = (uint8_t *)(data_cache_addr + cacheline_offset * CACHELINE_SIZE);

        memcpy(output, nand_cache_ptr, CACHELINE_SIZE);
        DEBUG_PRINT("[r1] Data Cache hit: logical_page_num=%u, data_cache_entry=%u, cacheline_offset=%u, addr=0x%x", logical_page_num, data_cache_entry, cacheline_offset, address);
        return 0;
    }

    // if a given cacheline is in the Write Log (r2)
    LogIndexEntry *log_index_entry = lookupLogIndex(address);
    if (log_index_entry != NULL) {
        uint32_t log_offset = log_index_entry->log_offset;
        WriteLogEntry *log_entry = &write_log_entries[log_offset];

        memcpy(output, log_entry->payload, CACHELINE_SIZE);
        DEBUG_PRINT("[r2] Found in Write Log: address=0x%x, log_offset=%u", address, log_offset);
        return 0;
    }

    // else, read the nand page and load it onto the data cache (r3)
    unsigned int data_cache_addr = ReadNandPageAndLoadToDataCache(0, logical_page_num);  // cmdSlotTag = 0
    if (data_cache_addr == 0) {
        DEBUG_PRINT("[r3] NAND read failed: logical_page_num=%u (page never written)", logical_page_num);
         return -1;  // if the nand page is never written
    }

    unsigned int offset_in_page = address % BYTES_PER_DATA_REGION_OF_SLICE;
    unsigned int cacheline_offset = offset_in_page / CACHELINE_SIZE;
    uint8_t *nand_cache_ptr = (uint8_t *)(data_cache_addr + cacheline_offset * CACHELINE_SIZE);

    memcpy(output, nand_cache_ptr, CACHELINE_SIZE);
    DEBUG_PRINT("[r3] Read from NAND and cached: logical_page_num=%u, cacheline_offset=%u, addr=0x%x", logical_page_num, cacheline_offset, address);
    return 0;
}
*/

static unsigned int ReadNandPageAndLoadToDataCache(const int cmdSlotTag, const unsigned int logicalSliceAddr)
{
    unsigned int reqSlotTag, dataBufEntry, virtualSliceAddr, DataBufAddress;
    XTime tcache_start, tcache_end;

    // dataBufEntry = CheckDataBufHitWithLSA(logicalSliceAddr, addressEnd);
    // if (dataBufEntry == DATA_BUF_FAIL) {
    virtualSliceAddr = AddrTransRead(logicalSliceAddr);
    if (virtualSliceAddr == VSA_FAIL)
    {
        DEBUG_PRINT("NAND page %u has never been programmed, thus this read req is wrong");
        return 0;
    }
    // data buffer miss, allocate a new buffer entry
    dataBufEntry = AllocateDataBuf();

    // clear the allocated data buffer entry being used by previous requests
    EvictDataBufEntryForMemoryCopy(dataBufEntry);

    // update meta-data of the allocated data buffer entry
    dataBufMapPtr->dataBuf[dataBufEntry].logicalSliceAddr = logicalSliceAddr;
    //XTime_GetTime(&tcache_start);
    PutToDataBufHashList(dataBufEntry);
    //XTime_GetTime(&tcache_end);
    //xil_printf("INSERT DATA BUFFER: %llu\r\n", tcache_end - tcache_start);

    // Wait for Eviction to be completed
    reqSlotTag = GetFromFreeReqQ();

    reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
    reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_READ;
    reqPoolPtr->reqPool[reqSlotTag].nvmeCmdSlotTag = cmdSlotTag;
    reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = logicalSliceAddr;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_ON;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
    reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = dataBufEntry;
    UpdateDataBufEntryInfoBlockingReq(reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry, reqSlotTag);
    reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = virtualSliceAddr;

    SelectLowLevelReqQ(reqSlotTag);
    SyncAllLowLevelReqDone();
    //}
    DataBufAddress = (DATA_BUFFER_BASE_ADDR + dataBufEntry * BYTES_PER_DATA_REGION_OF_SLICE);
    return DataBufAddress;
}
