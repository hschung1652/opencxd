#ifndef __CACHE_HPP__
#define __CACHE_HPP__

#define CL_SIZE 4096
#define LOW_RATIO 0.25
#define HIGH_RATIO 0.75


#include <stdio.h>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>



struct LRU_node{
    LRU_node* prev = nullptr;
    LRU_node* next = nullptr;
    int64_t cl_index = 0;
};

class LRU_list{
    public:
        LRU_node* head;
        LRU_node* tail;
        LRU_list(){head = nullptr; tail = nullptr;};
};


struct cache_obj
{
    bool valid = false;
    bool is_ready = false;
    bool isdirty = false;
    LRU_node* node = nullptr;
    int64_t PageCnt = 0;
    uint64_t ready_time = 0;
    std::unordered_set<int> accessed_cl_set;
    std::unordered_set<int> dirty_cl_set;
    bool mark = false;
    bool marked_warmup = false;
};


struct eviction
{
    // return 0 for no-eviction, return 1 for eviction-only, return 2 for flush
    int condition = 0;
    int64_t index = 0;
    int64_t PageCnt = 0;
    int accessed_cl_num = 0;
    int dirty_cl_num = 0;
};


class fcache{
    public:
        std::unordered_map<int64_t, cache_obj> cachemem;
        LRU_list LRUlist;
        int64_t cache_size_CL;
        int64_t curr_size;

        int64_t still_marked_dirty_num;
        int64_t still_marked_clean_num;


        std::mutex cache_keep_mutex; //hit-update;
        //std::mutex cache_evict_mutex;
        std::mutex LRU_mutex;

        fcache(int64_t size);

        //Note: Use this with give_access_num();
        void mark_warmup();

        void fill(int64_t index);
        void insert(int64_t index);
        void insert_nb(int64_t index, uint64_t ready_time, bool mark);
        void remove(int64_t index);
        bool is_hit(int64_t index);
        //return 0 if hit, return -1 if totally miss, return rest_waiting_time if miss_on_wait
        int64_t is_hit_nb(int64_t index, uint64_t current_time);
        eviction miss_evict(int64_t index);
        void readhitCL(int64_t index, int cl_offset);
        void writehitCL(int64_t index, int cl_offset);
        int64_t give_dirty_num();
        int64_t give_accessed_num();
        int64_t give_marked_dirty_num();
        int64_t give_marked_accessed_num();
};


class sa_cache{
    public:
        int way;
        int64_t size_byte;
        int64_t num_sets;
        
        /* page promotion algo*/
        int64_t maxThreshold;
        int64_t currThreshold;
        int64_t AggPromotedCnt;
        int64_t NetAggCnt;
        int64_t ResetEpoch;
        int64_t AccessCnt;
        double curr_ratio;
        std::mutex promotion_mutex_net;
        /* page promotion algo*/

        std::vector<fcache*> sets;

        /* Counting the Page locality */
        int r_data[65];
        int w_data[65];

        sa_cache(int64_t size_in_byte, int way, int64_t maxthreshold, int64_t resetepoch);
        ~sa_cache();
        void fill(int64_t index);
        void insert(int64_t index);
        void insert_nb(int64_t index, uint64_t ready_time, bool mark);
        void remove(int64_t index);
        bool is_hit(int64_t index);
        //return 0 if hit, return -1 if totally miss, return rest_waiting_time if miss_on_wait
        int64_t is_hit_nb(int64_t index, uint64_t current_time);
        void hold_keep_lock(int64_t index);
        void free_keep_lock(int64_t index);
        eviction miss_evict(int64_t index);
        void readhitCL(int64_t index, int cl_offset);
        void writehitCL(int64_t index, int cl_offset);

        int64_t give_dirty_num();
        int64_t give_accessed_num();

        int64_t give_marked_dirty_num();
        int64_t give_marked_accessed_num();

        /* page promotion algo*/
        void resetCounters();

        //Note: Use this with give_access_num();
        void mark_warmup();

        /* This function will update the cache and check if a 
        promotion will happen. It returns -1 for no promotion, and returns
        the cacheline index if there's a promotion. */
        int64_t update_and_choose_promotion(int64_t index);
        
        /* page promotion algo*/
        void do_promotion_evict(int64_t index);
        
        /* Counting the Page locality */
        void gen_page_locality_result(std::string filename);

        void snapshot(FILE* output_file);
        void replay_snapshot(FILE* input_file); //type 0 for cache, type 1 for host_dram
};


#endif
