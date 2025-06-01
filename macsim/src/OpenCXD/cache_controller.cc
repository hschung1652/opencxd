#include "cache_controller.h"

extern bool promotion_enable;
extern bool tpp_enable;

cache_controller::cache_controller(int64_t cache_size_in_byte, int way, int64_t maxthreshold, 
    int64_t resetepoch, int64_t host_dram_size_in_byte, int64_t host_way)
    : 
        the_cache(cache_size_in_byte, way, maxthreshold, resetepoch), 
        host_dram(host_dram_size_in_byte, host_way, maxthreshold, resetepoch),
        WritelogQueue(40960), PromotionQueue(409600){
    host_dram_size_pagenum = host_dram_size_in_byte / 4096;

    total_access_num = 0;
    host_hit = 0;
    anywaydram_hit = 0;
}


void cache_controller::snapshot(FILE* output_file){
    if (promotion_enable || tpp_enable)
    {
        host_dram.snapshot(output_file);
    }
}


void cache_controller::replay_snapshot(FILE* input_file){
    if (promotion_enable || tpp_enable)
    {
        host_dram.replay_snapshot(input_file);
    }
}


                                                                                                                                                                                                                            
/*
void cache_controller::process_a_memrequest(char type, int64_t addr){
    int64_t page_index = addr / 4096;
    total_access_num++;
    if (host_dram.is_hit(page_index))
    {
        // In Host DRAM
        printf("Addr: %x hit in host DRAM!\n", addr);
        host_hit++;
        anywaydram_hit++;
    }
    else
    {
        //Access cache
        if (the_cache.is_hit(page_index))
        { 
            anywaydram_hit++;
            printf("Addr: %x hit in SSD DRAM cache!\n", addr);
            if (type=='R')
            {
                the_cache.readhitCL(page_index);
            }
            else
            {
                the_cache.writehitCL(page_index);
            }
            
            //Promotion
            int64_t pm_index = the_cache.update_and_choose_promotion(page_index);
            if (pm_index!=-1)
            {
                printf("@@@Promote page addr: %x to host DRAM!\n", pm_index * 4096);
                host_dram.miss_evict(pm_index);
                host_dram.insert(pm_index);
                //promoted_set.insert(pm_index);
                the_cache.do_promotion_evict(pm_index);
            }
            
        }
        else
        {
            printf("Addr: %x missed in SSD DRAM cache!\n", addr);
            eviction evi = the_cache.miss_evict(page_index);
            
            the_cache.insert(page_index);
            if (type == 'R')
            {
                // 
            }
            else if (type == 'W')
            {
                the_cache.writehitCL(page_index);
            }

            //Promotion
            int64_t pm_index = the_cache.update_and_choose_promotion(page_index);
            if (pm_index!=-1)
            {
                printf("@@@Promote page addr: %x to host DRAM!\n", pm_index * 4096);
                host_dram.miss_evict(pm_index);
                host_dram.insert(pm_index);
                // promoted_set.insert(pm_index);
                the_cache.do_promotion_evict(pm_index);
            }
        }
    }
}
*/

void cache_controller::report_statistics(){
    std::cout<<"-----------------------------------------------------"<<std::endl;
    std::cout<<"Host DRAM hit rate: "<<(double)host_hit/total_access_num<<std::endl;
    std::cout<<"Anyway DRAM hit rate: "<<(double)anywaydram_hit/total_access_num<<std::endl;
}

