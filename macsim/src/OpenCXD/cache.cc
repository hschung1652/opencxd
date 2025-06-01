#include <stdio.h>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <unordered_map>
#include <assert.h>
#include "cache.h"
#include "not_ftl.h"
#include "simulator_clock.h"

//The simulator clock
extern sim_clock* the_clock_pt;

fcache::fcache(int64_t size){
    cache_size_CL = size/CL_SIZE;
    curr_size = 0;
    still_marked_clean_num = 0;
    still_marked_dirty_num = 0;
    cachemem.reserve(cache_size_CL);
}



void fcache::fill(int64_t index){
    if (cachemem.find(index)==cachemem.end())
    {
        cache_obj new_obj;
        new_obj.valid = false;
        new_obj.isdirty = false;
        new_obj.node = nullptr;
        new_obj.PageCnt = 0;
        new_obj.is_ready = false;
        new_obj.ready_time = 0;
        cachemem[index] = new_obj;
    }
}


void fcache::insert(int64_t index){
    assert(curr_size < cache_size_CL);
    cache_obj new_obj;
    new_obj.valid = true;
    new_obj.isdirty = false;
    new_obj.marked_warmup = false;
    new_obj.node = new LRU_node;
    new_obj.node->prev = nullptr;
    new_obj.node->next = nullptr;
    new_obj.node->cl_index = index;
    new_obj.PageCnt = 0; // wait for update() to increment it

    //LRU_mutex.lock();
    if (LRUlist.tail==nullptr)
    {
        // assert(curr_size==0);
        LRUlist.head = new_obj.node;
        LRUlist.tail = new_obj.node;
    }else
    {
        LRUlist.tail->next = new_obj.node;
        new_obj.node->prev = LRUlist.tail;
        LRUlist.tail = new_obj.node;
    }
    //LRU_mutex.unlock();

    cachemem[index] = new_obj;
    curr_size++;
    /*
    std::cout<<"333 "<<LRUlist.head<<","<<LRUlist.tail<<","<<curr_size<<std::endl;
    for (auto m : this->cachemem)
    {
        LRU_node* node = m.second.node;
        if(node){
        if (node->next!=nullptr)
        {
            assert(node->next!=node);
            assert(node->next->prev==node);
        }
        if (node->prev!=nullptr)
        {
            assert(node->prev!=node);
            assert(node->prev->next==node);
        }
        }
    }
    */
    
}


void fcache::insert_nb(int64_t index, uint64_t ready_time, bool mark){
    assert(curr_size < cache_size_CL);
    cache_obj new_obj;
    new_obj.valid = true;
    new_obj.isdirty = false;
    new_obj.is_ready = false;
    new_obj.ready_time = ready_time;
    new_obj.node = new LRU_node;
    new_obj.node->prev = nullptr;
    new_obj.node->next = nullptr;
    new_obj.node->cl_index = index;
    new_obj.mark = mark;
    new_obj.PageCnt = 0; // wait for update() to increment it

    //LRU_mutex.lock();
    if (LRUlist.tail==nullptr)
    {
        // assert(curr_size==0);
        LRUlist.head = new_obj.node;
        LRUlist.tail = new_obj.node;
    }else
    {
        LRUlist.tail->next = new_obj.node;
        new_obj.node->prev = LRUlist.tail;
        LRUlist.tail = new_obj.node;
    }
    //LRU_mutex.unlock();

    cachemem[index] = new_obj;
    curr_size++;

}



void fcache::remove(int64_t index){
    assert(curr_size>0);
    
    cachemem[index].valid = false;
    cachemem[index].isdirty = false;
    cachemem[index].node = nullptr;
    cachemem[index].PageCnt = 0;
    cachemem[index].accessed_cl_set.clear();
    cachemem[index].dirty_cl_set.clear();
    curr_size--;
}

bool fcache::is_hit(int64_t index){
    // assert((LRUlist.head==LRUlist.tail)&&(curr_size<=1)||(LRUlist.head!=LRUlist.tail));
    return (cachemem[index].valid);
}

//return 0 if hit, return -1 if totally miss, return rest_waiting_time if miss_on_wait
int64_t fcache::is_hit_nb(int64_t index, uint64_t current_time){
    // assert((LRUlist.head==LRUlist.tail)&&(curr_size<=1)||(LRUlist.head!=LRUlist.tail));
    if (!cachemem[index].valid)
    {
        return -1;
    }
    else if (!cachemem[index].is_ready)
    {
        if (current_time >= cachemem[index].ready_time)
        {
            cachemem[index].is_ready = true;
            return 0;
        } else
        {
            return (cachemem[index].ready_time - current_time);
        }
    }
    else
    {
        return 0;
    }

}

eviction fcache::miss_evict(int64_t index){
    if (curr_size < cache_size_CL)
    {

        eviction ev;
        ev.condition = 0;
        ev.index = 0;
        ev.accessed_cl_num = 0;
        ev.dirty_cl_num = 0;
        ev.PageCnt = 0;
        return ev;
    }else
    {
        // assert(LRUlist.head!=nullptr);
        //LRU_mutex.lock();
        LRU_node* evi = LRUlist.head;
        LRUlist.head->next->prev = nullptr;
        // std::cout<<LRUlist.head<<","<<LRUlist.head->next<<std::endl;
        LRUlist.head = evi->next;
        //LRU_mutex.unlock();

        bool is_dirty = cachemem[evi->cl_index].isdirty;
        eviction eev;
        eev.PageCnt = cachemem[evi->cl_index].PageCnt;
        eev.accessed_cl_num = cachemem[evi->cl_index].accessed_cl_set.size();
        eev.dirty_cl_num = cachemem[evi->cl_index].dirty_cl_set.size();
        
        remove(evi->cl_index);
        if (is_dirty)
        {
            eev.condition = 2;
        }
        else
        {
            eev.condition = 1;
        }
        eev.index = evi->cl_index;
        delete evi;

        /*
        std::cout<<"222 "<<LRUlist.head<<","<<LRUlist.tail<<","<<curr_size<<std::endl;
        for (auto m : this->cachemem)
        {
            LRU_node* node = m.second.node;
            if(node){
            if (node->next!=nullptr)
            {
                assert(node->next!=node);
                assert(node->next->prev==node);
            }
            if (node->prev!=nullptr)
            {
                assert(node->prev!=node);
                assert(node->prev->next==node);
            }
            }
        }
        */
        return eev;
    }
}


void fcache::readhitCL(int64_t index, int cl_offset){
    //LRU_mutex.lock();
    if(cachemem[index].marked_warmup){
        cachemem[index].marked_warmup = false;
        if(cachemem[index].isdirty){
            this->still_marked_dirty_num--;
        }
        else
        {
            this->still_marked_clean_num--;
        }
    }

    LRU_node* node = cachemem[index].node;
    if (node!=LRUlist.tail)
    {
        node->next->prev = node->prev;
        if (node->prev!=nullptr)
        {
            node->prev->next = node->next;
        } else
        {
            // assert(node==LRUlist.head);
            LRUlist.head = node->next;
        }
        
        LRUlist.tail->next = node;
        node->prev = LRUlist.tail;
        node->next = nullptr;
        LRUlist.tail = node;
    }
    //LRU_mutex.unlock();

    cachemem[index].accessed_cl_set.insert(cl_offset);

/*
    std::cout<<"111 "<<LRUlist.head<<std::endl;
    for (auto m : this->cachemem)
    {
        LRU_node* node = m.second.node;
        if(node){
        if (node->next!=nullptr)
        {
            assert(node->next!=node);
            assert(node->next->prev==node);
        }
        if (node->prev!=nullptr)
        {
            assert(node->prev!=node);
            assert(node->prev->next==node);
        }
        }
    }
    */
    // cachemem[index].PageCnt++;   //wait for update() to increment it
}


void fcache::writehitCL(int64_t index, int cl_offset){
    //LRU_mutex.lock();
    if(cachemem[index].marked_warmup){
        cachemem[index].marked_warmup = false;
        if(cachemem[index].isdirty){
            this->still_marked_dirty_num--;
        }
        else
        {
            this->still_marked_clean_num--;
        }
    }

    LRU_node* node = cachemem[index].node;
    cachemem[index].isdirty = true;
    if (node!=LRUlist.tail)
    {
        node->next->prev = node->prev;
        if (node->prev!=nullptr)
        {
            node->prev->next = node->next;
        } else
        {
            // assert(node==LRUlist.head);
            LRUlist.head = node->next;
        }
        
        LRUlist.tail->next = node;
        node->prev = LRUlist.tail;
        node->next = nullptr;
        LRUlist.tail = node;
    }
    //LRU_mutex.unlock();

    cachemem[index].dirty_cl_set.insert(cl_offset);
    cachemem[index].accessed_cl_set.insert(cl_offset);
    // cachemem[index].PageCnt++;   //wait for update() to increment it
}


int64_t fcache::give_dirty_num(){
    int64_t dirty_num = 0;
    for (auto obj : cachemem)
    {
        if (obj.second.valid && obj.second.isdirty)
        {
            dirty_num++;
        }
    }
    return dirty_num;
}


int64_t fcache::give_accessed_num(){
    int64_t accessed_num = 0;
    for (auto obj : cachemem)
    {
        if (obj.second.valid)
        {
            accessed_num++;
        }
    }
    return accessed_num;
}


int64_t fcache::give_marked_accessed_num(){
    return (still_marked_clean_num + still_marked_dirty_num);
}

int64_t fcache::give_marked_dirty_num(){
    return still_marked_dirty_num;
}


void fcache::mark_warmup(){
    for (auto obj : cachemem)
    {
        if (obj.second.valid)
        {
            obj.second.marked_warmup = true;
            if(obj.second.isdirty){
                this->still_marked_dirty_num++;
            }
            else
            {
                this->still_marked_clean_num++;
            }
        }
    }
}



sa_cache::sa_cache(int64_t size_in_byte, int way, int64_t maxthreshold, int64_t resetepoch){
    this->way = way;
    this->size_byte = size_in_byte;
    int64_t bank_size = size_byte / way;
    num_sets = bank_size / CL_SIZE;
    for (int i = 0; i < num_sets; i++)
    {
        fcache* sett = new fcache((int64_t)way*CL_SIZE);
        sets.push_back(sett);
    }
    maxThreshold = maxthreshold;
    currThreshold = maxthreshold;
    ResetEpoch = resetepoch;
    AggPromotedCnt = 0;
    NetAggCnt = 0;
    AccessCnt = 0;
    curr_ratio = 0.5;

    for (int i = 0; i < 65; i++)
    {
        r_data[i] = 0;
        w_data[i] = 0;
    }
}

sa_cache::~sa_cache(){
    for (auto i : sets)
    {
        delete i;
    }
}


void sa_cache::fill(int64_t index){
    int set_index = index % num_sets;
    sets[set_index]->fill(index / num_sets);
}

void sa_cache::insert(int64_t index){
    int set_index = index % num_sets;
    sets[set_index]->insert(index / num_sets);
}

void sa_cache::insert_nb(int64_t index, uint64_t ready_time, bool mark){
    int set_index = index % num_sets;
    sets[set_index]->insert_nb(index / num_sets, ready_time, mark);
}

void sa_cache::remove(int64_t index){
    int set_index = index % num_sets;
    sets[set_index]->remove(index / num_sets);
}

bool sa_cache::is_hit(int64_t index){
    int set_index = index % num_sets;
    return (sets[set_index]->is_hit(index / num_sets));
}

int64_t sa_cache::is_hit_nb(int64_t index, uint64_t current_time){
    int set_index = index % num_sets;
    return (sets[set_index]->is_hit_nb(index / num_sets, current_time));
}

eviction sa_cache::miss_evict(int64_t index){
    int set_index = index % num_sets;
    eviction evi = sets[set_index]->miss_evict(index / num_sets);
    evi.index = evi.index * num_sets + set_index;

    r_data[evi.accessed_cl_num]++;
    w_data[evi.dirty_cl_num]++;

    promotion_mutex_net.lock();
    if (evi.condition!=0)
    {
        NetAggCnt = NetAggCnt - evi.PageCnt;
        assert(NetAggCnt>=0);
    }
    promotion_mutex_net.unlock();
    
    return evi;
}

void sa_cache::readhitCL(int64_t index, int cl_offset){
    int set_index = index % num_sets;
    sets[set_index]->readhitCL(index / num_sets, cl_offset);
}

void sa_cache::writehitCL(int64_t index, int cl_offset){
    int set_index = index % num_sets;
    sets[set_index]->writehitCL(index / num_sets, cl_offset);
}

int64_t sa_cache::give_dirty_num(){
    int64_t dirty_num = 0;
    for (auto i : sets)
    {
        dirty_num += i->give_dirty_num();
    }
    return dirty_num;
}

int64_t sa_cache::give_accessed_num(){
    int64_t accessed_num = 0;
    for (auto i : sets)
    {
        accessed_num += i->give_accessed_num();
    }
    return accessed_num;
}

int64_t sa_cache::give_marked_dirty_num(){
    int64_t dirty_num = 0;
    for (auto i : sets)
    {
        dirty_num += i->give_marked_dirty_num();
    }
    return dirty_num;
}

int64_t sa_cache::give_marked_accessed_num(){
    int64_t accessed_num = 0;
    for (auto i : sets)
    {
        accessed_num += i->give_marked_accessed_num();
    }
    return accessed_num;
}


void sa_cache::mark_warmup(){
    for(auto i :sets)
    {
        i->mark_warmup();
    }
}


void sa_cache::resetCounters(){
    //promotion_mutex_net.lock();
    AccessCnt = NetAggCnt;
    AggPromotedCnt = 0;
    currThreshold = maxThreshold;
    //promotion_mutex_net.unlock();
}

int64_t sa_cache::update_and_choose_promotion(int64_t index){
    promotion_mutex_net.lock();
    NetAggCnt++;
    AccessCnt++;
    int set_index = index % num_sets;
    sets[set_index]->cachemem[index / num_sets].PageCnt++;
    bool promotion_flag = false;
    if (sets[set_index]->cachemem[index / num_sets].PageCnt >= currThreshold)
    {
        promotion_flag = true;
        AggPromotedCnt = AggPromotedCnt + sets[set_index]->cachemem[index / num_sets].PageCnt;
    }
    curr_ratio = AggPromotedCnt / AccessCnt;
    if (curr_ratio <= LOW_RATIO)
    {
        if (currThreshold < maxThreshold)
        {
            currThreshold++;
        }
    }
    else if (curr_ratio >= HIGH_RATIO)
    {
        if (currThreshold > 1 && promotion_flag)
        {
            currThreshold--;
        }
    }
    
    if (AccessCnt >= ResetEpoch)
    {
        resetCounters();
    }

    promotion_mutex_net.unlock();

    if (promotion_flag)
    {
        return index;
    }
    else
    {
        return -1;
    }
}

void sa_cache::do_promotion_evict(int64_t index){
    int set_index = index % num_sets;
    int64_t page_cnt = sets[set_index]->cachemem[index / num_sets].PageCnt;

    //LRU update:
    //sets[set_index]->LRU_mutex.lock();
    LRU_node* evi = sets[set_index]->cachemem[index / num_sets].node;
    if (evi->next!=nullptr && evi->prev!=nullptr)
    {
        evi->next->prev = evi->prev;
        evi->prev->next = evi->next;
    }
    else if (evi->prev==nullptr && evi->next!=nullptr)
    {
        assert(sets[set_index]->LRUlist.head==evi);
        sets[set_index]->LRUlist.head = evi->next;
        evi->next->prev = nullptr;
    }
    else if (evi->prev!=nullptr && evi->next==nullptr)
    {
        assert(sets[set_index]->LRUlist.tail==evi);
        sets[set_index]->LRUlist.tail = evi->prev;
        evi->prev->next = nullptr;
    }
    else
    {
        assert(sets[set_index]->LRUlist.head==evi);
        assert(sets[set_index]->LRUlist.tail==evi);
        sets[set_index]->LRUlist.head = nullptr;
        sets[set_index]->LRUlist.tail = nullptr;
    }
    //sets[set_index]->LRU_mutex.unlock();

    promotion_mutex_net.lock();
    NetAggCnt = NetAggCnt - page_cnt;
    assert(NetAggCnt>=0);
    promotion_mutex_net.unlock();
    
    remove(index);

    /*
    std::cout<<"444 "<<sets[set_index]->LRUlist.head<<std::endl;
    for (auto m : sets[set_index]->cachemem)
    {
        LRU_node* node = m.second.node;
        if(node){
        if (node->next!=nullptr)
        {
            assert(node->next!=node);
            assert(node->next->prev==node);
        }
        if (node->prev!=nullptr)
        {
            assert(node->prev!=node);
            assert(node->prev->next==node);
        }
        }
    }*/
}


void sa_cache::hold_keep_lock(int64_t index){
    int set_index = index % num_sets;
    sets[set_index]->cache_keep_mutex.lock();
}

void sa_cache::free_keep_lock(int64_t index){
    int set_index = index % num_sets;
    sets[set_index]->cache_keep_mutex.unlock();
}


void sa_cache::gen_page_locality_result(std::string filename){
    std::ofstream fout1(filename+"_parsed_R_amp.txt");
    std::ofstream fout2(filename+"_parsed_W_amp.txt");
    for (int i = 1; i < 65; i++)
    {
        fout1 << ((double)i/64) <<": "<< r_data[i]<<std::endl;
        fout2 << ((double)i/64) <<": "<< w_data[i]<<std::endl;
    }
    fout1.close();
    fout2.close();
}



void sa_cache::snapshot(FILE* output_file){
    fprintf(output_file, "%ld\n", num_sets);

    for (int i = 0; i < num_sets; i++)
    {
        // std::stringstream ss;
        long count = 0;
        fcache* current_set = sets[i];
        fprintf(output_file, "%ld\n", current_set->curr_size);
        count = current_set->curr_size;
        LRU_node* evi = current_set->LRUlist.head;
        while (count > 0)
        {
            assert(evi!=nullptr);
            bool is_dirty = current_set->cachemem[evi->cl_index].isdirty;
            if (is_dirty)
            {
                fprintf(output_file, "%ld 1\n", evi->cl_index);
            }else
            {
                fprintf(output_file, "%ld 0\n", evi->cl_index);
            }
            assert(current_set->cachemem[evi->cl_index].valid);
            evi = evi->next;
            count--;
        }
    }
    fprintf(output_file, "-----------------------------------------------------\n");
}

void sa_cache::replay_snapshot(FILE* input_file){
    int64_t num_sets;
    assert(fscanf(input_file, "%ld\n", &num_sets));
    for (int i = 0; i < num_sets; i++)
    {
        long size;
        assert(fscanf(input_file, "%ld\n", &size));
        int64_t index;
        int is_dirty;
        for (long j = 0; j < size; j++)
        {
            assert(fscanf(input_file, "%ld %d\n", &index, &is_dirty));
            sets[i]->insert(index);
            if (is_dirty)
            {
                sets[i]->cachemem[index].isdirty = true;
            }
            int64_t sa_index = index * num_sets + i;
            bytefs_fill_data(sa_index*PG_SIZE);
        }
    }
    char line[100];
    char* del = fgets(line, 100, input_file);
}