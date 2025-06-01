#ifndef CXL_SSD_H_
#define CXL_SSD_H_

#include "xil_types.h"
#include "nvme.h"

unsigned short MemWriteRequestSingle(uint32_t addressStart, uint32_t addressEnd, NVME_IO_COMMAND *nvmeIOCmd, uint64_t *time);
unsigned short MemReadRequestSingle(uint32_t addressStart, uint32_t addressEnd, uint64_t *time, uint64_t *tmiss);
//int MemWriteRequest(uint32_t address, const uint8_t *payload);
//int MemReadRequest(uint32_t address, uint8_t *output);

//copied over from skybyte stats
struct ssd_stat {
    // total issue counter by request count
    uint64_t block_rissue_count;
    uint64_t block_wissue_count;
    uint64_t byte_rissue_count;
    uint64_t byte_wissue_count;
    // total traffic
    uint64_t block_rissue_traffic;
    uint64_t block_wissue_traffic;
    uint64_t byte_rissue_traffic;
    uint64_t byte_wissue_traffic;

    // log compaction stats
    uint64_t log_compaction_count;
    uint64_t log_compaction_traffic_read;
    uint64_t log_compaction_traffic_write;

    // internal traffic
    uint64_t nand_read_user;
    uint64_t nand_read_internal;
    uint64_t nand_write_user;
    uint64_t nand_write_internal;

};

extern struct ssd_stat stats;


#endif // CXL_SSD_H_
