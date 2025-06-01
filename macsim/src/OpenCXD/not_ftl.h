

#include <fstream>
#include <unordered_map>
#include <map>
#include <pthread.h>
#include <time.h>
#include <mutex>

#include "timing_model.h"
#include "utils.h"
#include "thread_utils.h"

#define PG_SIZE             (16 * 1024)

enum issue_status : uint8_t {
    NORMAL                      = 0,
    HOST_DRAM_HIT               = (1 << 0),
    WRITE_LOG_W                 = (1 << 1),
    WRITE_LOG_R                 = (1 << 2),
    SSD_CACHE_HIT               = (1 << 3),
    SSD_CACHE_MISS              = (1 << 4),
    ONGOING_DELAY               = (1 << 5),
    LOG_COMPACTION              = (1 << 6)

};

enum NvmeIoCommands {
    NVME_CMD_FLUSH              = 0x00,
    NVME_CMD_WRITE              = 0x01,
    NVME_CMD_READ               = 0x02,
    NVME_CMD_WRITE_UNCOR        = 0x04,
    NVME_CMD_COMPARE            = 0x05,
    NVME_CMD_WRITE_ZEROES       = 0x08,
    NVME_CMD_CXL_ISSUE          = 0xA0, //normal cxl read/write
    NVME_CMD_CXL_PROMOTION      = 0xA1, //page promotion
    NVME_CMD_CXL_WRITE_PAGE     = 0xA2, //write page directly to NAND
    NVME_CMD_CXL_STATS          = 0xA3, //print daisy stats
};

struct issue_response {
    uint64_t latency;
    uint64_t estimated_latency;
    issue_status flag;
    issue_status flag_origin;
};

struct ssd {
    int ssd_fd;
    // thread
    //const uint64_t n_log_writer_threads = 1;
    const uint64_t n_promotion_threads = 1;
    unsigned int NSID;
    //struct ftl_thread_info *thread_args;
    //pthread_t *ftl_thread_id;
    //pthread_t *polling_thread_id;
    //pthread_t *log_writer_thread_id;
    pthread_t *promotion_thread_id;
    //pthread_t *simulator_timer_id;
    volatile uint8_t terminate_flag = 0;
};

int init(string dev, int nsid);

int64_t get_hostdram_dirty_page_num();
int64_t get_hostdram_accessed_page_num();

int64_t get_hostdram_dirty_marked_page_num();
int64_t get_hostdram_accessed_marked_page_num();
void host_dram_mark_workup();

extern int byte_issue(int is_write, uint64_t lpa, uint64_t size, struct issue_response *resp);
void *promotion_thread(void *thread_args);

void replay_dram_system(FILE* input_file);
void replay_tpp_system(FILE* input_file);

void bytefs_fill_data(uint64_t addr);
void warmup_host_dram(double warmup_dirty_ratio_cache, double warmup_dirty_ratio_dram, uint64_t read_pgnum, uint64_t write_pgnum,
    double cache_overall_cover_rate, double host_overall_cover_rate, double cache_uncovered_dirty_rate, double host_uncovered_dirty_rate);
void print_stats();