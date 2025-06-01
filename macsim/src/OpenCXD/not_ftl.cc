#include <time.h>
#include <sched.h>
#include <cstring>
#include <mutex>
#include <deque>
#include <set>
#include <chrono>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>

#include "cache_controller.h"
#include "utils.h"
#include "not_ftl.h"
#include "simulator_clock.h"

using std::max;
using std::pair;
using std::mutex;

//Parameters:
bool promotion_enable = true;
bool write_log_enable = true;
bool device_triggered_ctx_swt = true;
long cs_threshold = 43000;
bool is_simulator_not_emulator = true;

long host_dram_size_byte = 1*1024*1024*1024;
long ssd_cache_size_byte = 512*1024*1024;
double write_log_ratio = 0.125;
ssd gdev;

//SSD data cache and controller
cache_controller* dram_subsystem;

//The simulator clock
extern sim_clock* the_clock_pt;
extern param param;
extern std::vector<uint64_t> ordered_vector;

//For TPP implementation:

std::deque<uint64_t> LRU_active_list;
std::set<uint64_t> LRU_inactive_list;
std::mutex tpp_LRU_mutex;
vector<uint64_t> ordered_memory_space;
uint64_t NUMA_scan_pointer = 0;
uint64_t NUMA_scan_threshold_ns = 100000000; //100 ms
long NUMA_scan_count = 0;
bool tpp_enable = false;
std::set<uint64_t> NUMA_scan_set;
std::mutex NUMA_scan_mutex;


//For AstriFlash implementation:
bool astriflash_enable = false;

int init(string dev, int nsid){
    ssd *ssd = &gdev;
    ssd->ssd_fd = open(dev.c_str(), O_RDONLY);
    ssd->NSID = nsid;
    //int error = opencxd_start_threads();
    return 0;
}

/*
void *simulator_timer_thread(void *thread_args){
    sim_clock the_clock(0, param.logical_core_num+1);
    the_clock_pt = &the_clock;

    while (ssd->terminate_flag == 0)
    {
        the_clock.check_pop_and_incre_time();
    }
    return nullptr;
}
*/

int nvme_passthru(int fd, uint8_t opcode,
    uint8_t flags, uint16_t rsvd,
    uint32_t nsid, uint32_t cdw2, uint32_t cdw3, uint32_t cdw10, uint32_t cdw11,
    uint32_t cdw12, uint32_t cdw13, uint32_t cdw14, uint32_t cdw15, uint32_t metadata_len, 
    void *metadata, uint32_t data_len, void *data, uint32_t &result)
{
struct nvme_passthru_cmd cmd = {
    .opcode		= opcode,
    .flags		= flags,
    .rsvd1		= rsvd,
    .nsid		= nsid,
    .cdw2		= cdw2,
    .cdw3		= cdw3,
    .metadata	= (uint64_t)(uintptr_t) NULL,
    .addr		= (uint64_t)(uintptr_t) data,
    .metadata_len	= 0,
    .data_len	= data_len,
    .cdw10		= cdw10,
    .cdw11		= cdw11,
    .cdw12		= cdw12,
    .cdw13		= cdw13,
    .cdw14		= cdw14,
    .cdw15		= cdw15,
    .timeout_ms	= 0,
    .result		= 0,
};
int err;

err = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
result = cmd.result; 

return err;
}

void *promotion_thread(void *thread_args) {
    ssd *ssd = &gdev;
    bool thread_waiting = false;
    while (ssd->terminate_flag == 0) {
        //result holds promoted page index
        uint32_t result = 0;
        uint32_t cdw2, cdw3, cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
        cdw2 = cdw3 = cdw10 = cdw11 = cdw12 = cdw13 = cdw14 = cdw15 = 0;
        //NVME_CMD_CXL_PROMOTION cmd needs to ensure it get a page index that is in data cache
        //and also a candidate for promotion (in the promotion queue)
        nvme_passthru(ssd->ssd_fd, NVME_CMD_CXL_PROMOTION, 0, 0, ssd->NSID, cdw2, cdw3, 
            cdw10, cdw11, cdw12, cdw13, cdw14, cdw15, 
            0, NULL, 0, NULL, result);
        if (result != 0) {
            int tmpResult = (int)result;
            dram_subsystem->host_dram.hold_keep_lock(tmpResult);
            eviction host_demotion = dram_subsystem->host_dram.miss_evict(tmpResult);
            dram_subsystem->host_dram.insert(tmpResult);
            if (host_demotion.condition==2){
                if (tpp_enable)
                {
                    tpp_LRU_mutex.lock();
                    if (LRU_inactive_list.find(host_demotion.index) == LRU_inactive_list.end())
                    {
                        LRU_inactive_list.insert(host_demotion.index);
                    }
                    tpp_LRU_mutex.unlock();
                    NUMA_scan_mutex.lock();
                    NUMA_scan_set.erase(host_demotion.index);
                    NUMA_scan_mutex.unlock();
                }
            }
            dram_subsystem->host_dram.free_keep_lock(tmpResult);
            if (host_demotion.condition == 2){
                cdw3 = 2; //write entire page to CXL-SSD
                uint64_t lpa = host_demotion.index*PG_SIZE;
                nvme_passthru(ssd->ssd_fd, NVME_CMD_CXL_ISSUE, 0, 0, ssd->NSID, cdw2, cdw3, 
                    cdw10, cdw11, cdw12, cdw13, cdw14, cdw15, 
                    0, NULL, 0, NULL, result);
            }
        }
    }
    return nullptr;
}

int64_t get_hostdram_dirty_page_num(){
    return dram_subsystem->host_dram.give_dirty_num();
}

int64_t get_hostdram_accessed_page_num(){
    return dram_subsystem->host_dram.give_accessed_num();
}

int64_t get_hostdram_dirty_marked_page_num(){
    return dram_subsystem->host_dram.give_marked_dirty_num();
}

int64_t get_hostdram_accessed_marked_page_num(){
    return dram_subsystem->host_dram.give_marked_accessed_num();
}

void replay_dram_system(FILE* input_file){
    dram_subsystem->replay_snapshot(input_file);
}

void replay_tpp_system(FILE* input_file){
    LRU_active_list.clear();
    LRU_inactive_list.clear();
    NUMA_scan_set.clear();
    int64_t inactive_size;
    assert(fscanf(input_file, "%ld\n", &inactive_size));
    for (int64_t i = 0; i < inactive_size; i++)
    {
        int64_t element;
        assert(fscanf(input_file, "%ld\n", &element));
        LRU_inactive_list.insert(element);
    }
    int64_t active_size;
    assert(fscanf(input_file, "%ld\n", &active_size));
    for (int64_t i = 0; i < active_size; i++)
    {
        int64_t element;
        assert(fscanf(input_file, "%ld\n", &element));
        LRU_active_list.push_front(element);
    }
    int64_t numa_size;
    assert(fscanf(input_file, "%ld\n", &numa_size));
    for (int64_t i = 0; i < numa_size; i++)
    {
        int64_t element;
        assert(fscanf(input_file, "%ld\n", &element));
        NUMA_scan_set.insert(element);
    }
    NUMA_scan_threshold_ns = 2000000000;
}

/*
void warmup_write_log(uint64_t read_pgnum, uint64_t write_pgnum){

    std::unordered_set<uint64_t> warmup_pages;
    ssd* ssd = &gdev;
    uint32_t random32bit; //Page index as a 32-bit value

    std::cout<<"Start to Warm up the SSD write log!"<<std::endl;

    while (warmup_pages.size() < (int64_t)((ssd_cache_size_byte / 4096)))
    {
        random32bit = rand() | (rand() << 16);
        warmup_pages.insert(random32bit*PG_SIZE);
    }
    std::vector<uint64_t> warmup_lpns;
    for (auto page : warmup_pages) {
        warmup_lpns.push_back(page/PG_SIZE);
    }

    if (write_log_enable)
    {
        std::cout<<"Start to Warm up the Write Log!"<<std::endl;
        for (size_t i = 0; i < read_pgnum; i++)
        {
            bytefs_fill_data(warmup_lpns[i]*PG_SIZE);
            assert(read_pgnum < ssd_cache_size_byte / 4096);
            uint32_t result = 0;
            uint32_t cdw2, cdw3, cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
            cdw2 = cdw3 = cdw10 = cdw11 = cdw12 = cdw13 = cdw14 = cdw15 = 0;
            nvme_passthru(ssd->ssd_fd, NVME_CMD_CXL_INTERNAL_READ, 0, 0, ssd->NSID, cdw2, cdw3, 
                cdw10, cdw11, cdw12, cdw13, cdw14, cdw15, 
                0, NULL, 0, NULL, result);
        }
        for (size_t i = 0; i < write_pgnum; i++)
        {
            bytefs_fill_data(warmup_lpns[i]*PG_SIZE);
            nand_cmd cmd;
            ppa ppa;
            ssd->maptbl_update_mutex.lock();
            ppa = get_new_page(ssd);
            set_maptbl_ent(ssd, warmup_lpns[i], &ppa);
            set_rmap_ent(ssd, warmup_lpns[i], &ppa);
            mark_page_valid(ssd, &ppa);
            ssd_advance_write_pointer(ssd);

            // perform data write
            cmd.type = INTERNAL_TRANSFER;
            cmd.cmd = NAND_WRITE;
            cmd.stime = 0;
            ssd_advance_status(ssd, &ppa, &cmd);
            ssd->maptbl_update_mutex.unlock();
            SSD_STAT_ADD(log_wr_page, 1);
        }
        
    }
}
*/

void host_dram_mark_workup(){
    dram_subsystem->host_dram.mark_warmup();
}

int byte_issue(int is_write, uint64_t lpa, uint64_t size, issue_response *resp) {
    //std::cout << "byte issue " << std::endl;
    ssd *ssd = &gdev;
    volatile uint64_t stime, endtime;
    long latency = 0;
    
    bool print_flag = false;


    if (size == 0)
        return -1;

    
    stime = the_clock_pt->get_time_sim(); 

    int64_t page_index = lpa / PG_SIZE;
    int cl_offs = (lpa % PG_SIZE) / 64;
    bool host_dram_hit = false;

    /*
    if (promotion_enable || tpp_enable)
    {
        dram_subsystem->host_dram.hold_keep_lock(page_index);
    }
    
    if ((promotion_enable || tpp_enable) && dram_subsystem->host_dram.is_hit(page_index))
    {
        host_dram_hit = true;
        
        if (is_write)
        {
            dram_subsystem->host_dram.writehitCL(page_index, cl_offs);
        } else
        {
            dram_subsystem->host_dram.readhitCL(page_index, cl_offs);
        }
        
        dram_subsystem->host_dram.free_keep_lock(page_index);

        //endtime = get_time_ns();  
        
        if (is_simulator_not_emulator)
        {
            latency = 46;
        }
        
        resp->flag = HOST_DRAM_HIT;
        // print_flag = true;
    }
    else*/
    {
        if (promotion_enable || tpp_enable)
        {
            dram_subsystem->host_dram.free_keep_lock(page_index);
        }
        uint32_t result = 0;
        void *data = NULL, *metadata = NULL;
        uint32_t cdw2, cdw3, cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
        cdw2 = cdw3 = cdw10 = cdw11 = cdw12 = cdw13 = cdw14 = cdw15 = 0;
        cdw12 = is_write; //0 == read, 1 == write
        cdw11 = static_cast<uint32_t>(lpa);
        //std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
        int returned = nvme_passthru(ssd->ssd_fd, NVME_CMD_CXL_ISSUE, 0, 0, ssd->NSID, cdw2, cdw3, 
            cdw10, cdw11, cdw12, cdw13, cdw14, cdw15, 
            0, &data, 0, &metadata, result);
        //std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now();
        //std::chrono::steady_clock::duration time_span = (t2 - t1);
        //std::cout << result << " " << returned << std::endl;
        bool context_siwtch = false;
        latency = (long)(((double)result / 33333000) * 1000000000) - 420 + 40; // counts / clk * nanosecond - xtime_l latency + CXL latency
        if (returned == 3){
            resp->flag_origin = WRITE_LOG_W;
        } else if (returned == 2){
            resp->flag_origin = WRITE_LOG_R;
        } else if (returned == 4){
            resp->flag_origin = SSD_CACHE_HIT;
        } else if (returned == 5){
            resp->flag_origin = SSD_CACHE_MISS;
        } else if (returned == 6) {
            latency -= 420;
            resp->flag_origin = LOG_COMPACTION;
        }
        //std::cout << "cxl latency: "<< latency << std::endl;
        //std::cout << "nvme_passthru nanoseconds: " << time_span.count() << std::endl;
        if (device_triggered_ctx_swt && latency >= cs_threshold){
            resp->flag = ONGOING_DELAY;
            if (returned <= 6){
                //std::cout << "!!!!!!!!!!!!!!!!!!!" << latency << std::endl;
                resp->latency = latency;
                resp->estimated_latency = latency;
            }
            else{
                //std::cout << returned << std::endl;
                resp->flag_origin = SSD_CACHE_MISS;
                resp->latency = (long)((((double)returned) / 33333000) * 1000000000) - 420;
                resp->estimated_latency = (long)((((double)result - (double)returned) / 33333000) * 1000000000) - 420;
                //std::cout << resp->latency + resp->estimated_latency << std::endl;
            }
            context_siwtch = true;
            return 0;
        } else {
            resp->flag = resp->flag_origin;
        }
    }
    resp->latency = latency;
    return 0;
}

void bytefs_fill_data(uint64_t addr) {
    ssd *ssd = &gdev;
    uint64_t current_lpn = addr / PG_SIZE;

    if (tpp_enable)
    {
        ordered_memory_space.push_back(current_lpn);
        LRU_inactive_list.insert(current_lpn);
    }

    if (promotion_enable || tpp_enable)
    {
        dram_subsystem->host_dram.fill(current_lpn);
    }
    uint32_t result = 0;
    uint32_t cdw2, cdw3, cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
    cdw2 = cdw3 = cdw10 = cdw11 = cdw12 = cdw13 = cdw14 = cdw15 = 0;
    cdw11 = static_cast<uint32_t>(addr);
    nvme_passthru(ssd->ssd_fd, NVME_CMD_CXL_WRITE_PAGE, 0, 0, ssd->NSID, cdw2, cdw3, 
        cdw10, cdw11, cdw12, cdw13, cdw14, cdw15, 
        0, NULL, 0, NULL, result);
}

void print_stats(){
  ssd *ssd = &gdev;
  uint32_t result = 0;
  void *data = NULL, *metadata = NULL;
  uint32_t cdw2, cdw3, cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
  cdw2 = cdw3 = cdw10 = cdw11 = cdw12 = cdw13 = cdw14 = cdw15 = 0;
  int returned = nvme_passthru(ssd->ssd_fd, NVME_CMD_CXL_STATS, 0, 0, ssd->NSID, cdw2, cdw3, 
            cdw10, cdw11, cdw12, cdw13, cdw14, cdw15, 
            0, &data, 0, &metadata, result);
}