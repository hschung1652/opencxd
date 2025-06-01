#include "writelog.h"
#include "compactbuffer.h"
#include <assert.h>

#include "../memory_map.h"
#include "../ftl_config.h"
#include "../request_transform.h"
#include "../data_buffer.h"
#include "../nvme/cxl_ssd.h"

uint32_t write_log_count;
WriteLogEntry *write_log_entries = (WriteLogEntry *)WRITE_LOG_START_ADDR;

static void TriggerInternalDataWrite(const unsigned int lsa, const unsigned int bufAddr);
static void TriggerInternalDataRead(const unsigned int lsa, const unsigned int bufAddr);

uint8_t *nand_page_list = (uint8_t *)NAND_PAGE_LIST_START;

void initWriteLog()
{
    write_log_count = 0;

    for (int i = 0; i < MAX_LOG_ENTRIES; i++)
    {
        write_log_entries[i].addressEnd = 0;
        memset(write_log_entries[i].payload, 0, CACHELINE_SIZE);
    }

    DEBUG_PRINT("Write log is fully initialized: %u entries cleared.", MAX_LOG_ENTRIES);
}

void logWrite(uint32_t addressStart, uint32_t addressEnd)
{
	XTime tlog_start, tlog_end;
    if (write_log_count >= MAX_LOG_ENTRIES)
    {
        xil_printf("[ERROR] Write log is full!\r\n");
        assert(0);
    }

    // append to the Write Log (w1)
    WriteLogEntry *entry = &write_log_entries[write_log_count];
    entry->addressEnd = addressEnd;
    memcpy(entry->payload, DUMMY_CACHE_READ, CACHELINE_SIZE);
	stats.byte_wissue_traffic += CACHELINE_SIZE;
	stats.byte_wissue_count++;
	stats.nand_write_user += CACHELINE_SIZE;

    uint32_t log_offset = write_log_count;
    write_log_count++;
    DEBUG_PRINT("[w1] Log entry added: Address = 0x%lx, LogOffset = %lu", addressEnd, log_offset);
    if (log_offset >= MAX_LOG_ENTRIES)
    {
        xil_printf("[ERROR] Something goes wrong related to write log offset\r\n");
        assert(0);
    }

    // update to data cache entry if exists (w2)
    unsigned int logical_page_num = addressEnd / BYTES_PER_DATA_REGION_OF_SLICE;
    unsigned int data_cache_entry = CheckDataBufHitWithLSA(logical_page_num);
    if (data_cache_entry != DATA_BUF_FAIL)
    {
    	stats.byte_wissue_traffic += CACHELINE_SIZE;
    	stats.byte_wissue_count++;
    	stats.nand_write_internal += CACHELINE_SIZE;
        unsigned int data_cache_addr = (DATA_BUFFER_BASE_ADDR + data_cache_entry * BYTES_PER_DATA_REGION_OF_SLICE);
        unsigned int offset_in_page = addressEnd % BYTES_PER_DATA_REGION_OF_SLICE;
        unsigned int cacheline_offset = offset_in_page / CACHELINE_SIZE;

        uint8_t *nand_cache_ptr = (uint8_t *)(data_cache_addr + cacheline_offset * CACHELINE_SIZE);
        memcpy(nand_cache_ptr, DUMMY_CACHE_READ, CACHELINE_SIZE);
        if (RecordDataBufEntryDirty(data_cache_entry) != 0)
        {
            xil_printf("[ERROR] Data buffer entry is not allocated!\r\n");
            assert(0);
        }
        DEBUG_PRINT("[w2] Cache updated: NandPage = %u, DataCacheEntry = %u, CachelineOffset = %u",
                    logical_page_num, data_cache_entry, cacheline_offset);
    }

    // update to the Log Index (w3)
    //XTime_GetTime(&tlog_start);
    insertLogIndex(addressEnd, log_offset);
    //XTime_GetTime(&tlog_end);
    //xil_printf("INSERT WRITE LOG: %llu\r\n", tlog_end - tlog_start);
    DEBUG_PRINT("[w3] Log index updated: Address = 0x%lx, LogOffset = %lu", addressEnd, log_offset);
}

void printWriteLog()
{
    xil_printf("Write Log Entries (%lu entries):\r\n", write_log_count);
    for (uint32_t i = 0; i < write_log_count; i++)
    {
        xil_printf("Entry %lu: Address = 0x%llx, Data[0] = 0x%x\r\n",
                   i, write_log_entries[i].addressEnd, write_log_entries[i].payload[0]);
    }
}

void logCompaction()
{
    //DEBUG_PRINT("Starting Log Compaction...\r\n");
	//xil_printf("Starting Log Compaction...\r\n");
    // for dubugging
    int total_cleared_entries = 0;

    // traverse 1st-Level Log Index to find nand pages to flush
    for (int i = 0; i < LOG_INDEX_BUCKET_COUNT; i++)
    {
        if (nand_page_list[i] == 0)
        {
            continue; // page i is not used
        }
        unsigned int nand_page = i;
        DEBUG_PRINT("Processing NAND Page: %u", nand_page);

        // if a given nand page is cached onto the data cache, write immediately
        unsigned int data_cache_entry = CheckDataBufHitWithLSA(nand_page);
        if (data_cache_entry != DATA_BUF_FAIL)
        {
            DEBUG_PRINT("NAND Page %u found in the Data Cache. Flush directly to NAND", nand_page);
            stats.nand_write_internal += BYTES_PER_DATA_REGION_OF_SLICE;
            stats.block_wissue_count++;
            stats.log_compaction_traffic_write += BYTES_PER_DATA_REGION_OF_SLICE;

            // trigger nand flush for the target data cache entry
            // but don't evict (eliminate) it from the data cache
            EvictDataBufEntryForMemoryCopy(data_cache_entry); // this is just flush
            SyncAllLowLevelReqDone();

            // clear the corresponding log entries in the 2nd-Level Log Index
            int cleared_entries = 0;
            for (int page_offset = 0; page_offset < LOG_INDEX_BUCKET_SIZE; page_offset++)
            {
                if (LOG_INDEX_TABLE[nand_page][page_offset] != MAX_LOG_ENTRIES + 1)
                {
                	LOG_INDEX_TABLE[nand_page][page_offset] = MAX_LOG_ENTRIES + 1;
                    cleared_entries++;
                }
            }
            DEBUG_PRINT("Cleared %d Log entries for NAND Page %u", cleared_entries, nand_page);
            total_cleared_entries += cleared_entries;
            continue;
        }

        // if a given nand page is not cached, then load onto memory
        DEBUG_PRINT("NAND Page %u not found in the Data Cache", nand_page);
        uint8_t *compaction_entry = (uint8_t *)(LOG_COMPACT_BUFFER_START);
        if (compaction_entry == NULL)
        {
            xil_printf("[ERROR] Invalid compaction buffer index %d\r\n", i);
            assert(0);
        }
        // corner case: if the nand page has never been programmed -> just write
        unsigned int temp_slice_addr = AddrTransRead(nand_page);
        if (temp_slice_addr != VSA_FAIL)
        {
            DEBUG_PRINT("NAND page %u has been programmed already, loading data...", nand_page);
            TriggerInternalDataRead(nand_page, (unsigned int)compaction_entry);
            SyncAllLowLevelReqDone();
            stats.nand_read_internal += BYTES_PER_DATA_REGION_OF_SLICE;
            stats.block_rissue_count++;
            stats.log_compaction_traffic_read += BYTES_PER_DATA_REGION_OF_SLICE;
        }
        else
        {
            DEBUG_PRINT("NAND page %u has never been programmed, skipping read", nand_page);
            ; // two semicolons are intentional!
        }

        // traverse 2nd-Level Log Index to find cachelines to be merged into the nand page
        int cleared_entries = 0;
        for (int page_offset = 0; page_offset < LOG_INDEX_BUCKET_SIZE; page_offset++)
        {
            if (LOG_INDEX_TABLE[nand_page][page_offset] == MAX_LOG_ENTRIES + 1)
            {
                continue; // skip uninitialized entries
            }

            // merge it into the nand page
            uint8_t *nand_cache_ptr = (uint8_t *)(compaction_entry + page_offset * CACHELINE_SIZE);
            uint32_t log_offset = LOG_INDEX_TABLE[nand_page][page_offset];
            WriteLogEntry *log_entry = &write_log_entries[log_offset];
            memcpy(nand_cache_ptr, log_entry->payload, CACHELINE_SIZE);
            DEBUG_PRINT("Merged log to NAND Page %u: nand_cache_ptr=%p, log_entry_payload=%p, page_offset=%u", nand_page, (void *)nand_cache_ptr, (void *)log_entry->payload, page_offset);
            stats.nand_read_internal += CACHELINE_SIZE;
            stats.byte_rissue_count++;
            stats.log_compaction_traffic_read += CACHELINE_SIZE;
            cleared_entries++;
        }
        DEBUG_PRINT("Cleared %d Log entries for NAND Page %u", cleared_entries, nand_page);
        total_cleared_entries += cleared_entries;
        DEBUG_PRINT("Flushing updated NAND page %u to NAND", nand_page);
        TriggerInternalDataWrite(nand_page, (unsigned int)compaction_entry);
        SyncAllLowLevelReqDone();
        stats.nand_write_internal += BYTES_PER_DATA_REGION_OF_SLICE;
        stats.block_wissue_count++;
		stats.log_compaction_traffic_write += BYTES_PER_DATA_REGION_OF_SLICE;
    }
    DEBUG_PRINT("Total %d Log entries are cleared", total_cleared_entries);

    // after the log compaction
    DEBUG_PRINT("Cleaning up processed logs...");
    if (total_cleared_entries < write_log_count)
    {
        xil_printf("[ERROR] Log Compaction goes wrong: there're still some orphan entries\r\n");
        xil_printf("total cleared entries : %d, writelog count: %d\r\n", total_cleared_entries, write_log_count);
    }
    initWriteLog();
    initLogIndex();

    DEBUG_PRINT("Log Compaction Complete. Remaining log entries: %lu\r\n", write_log_count);
}

static void TriggerInternalDataWrite(const unsigned int lsa, const unsigned int bufAddr)
{
    unsigned int virtualSliceAddr = AddrTransWrite(lsa);
    unsigned int reqSlotTag = GetFromFreeReqQ();

    reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
    reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_WRITE;
    reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = lsa;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
    reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.addr = bufAddr;
    reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = virtualSliceAddr;

    SelectLowLevelReqQ(reqSlotTag);
}

static void TriggerInternalDataRead(const unsigned int lsa, const unsigned int bufAddr)
{
    unsigned int virtualSliceAddr = AddrTransRead(lsa);
    unsigned int reqSlotTag = GetFromFreeReqQ();

    reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
    reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_READ;
    reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = lsa;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR_NO_SPARE;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
    reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
    reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.addr = bufAddr;
    reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = virtualSliceAddr;

    SelectLowLevelReqQ(reqSlotTag);
}
