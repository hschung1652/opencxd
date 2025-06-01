#ifndef __SIMULATOR_CLOCK_H__
#define __SIMULATOR_CLOCK_H__

#include <fstream>
#include <unordered_map>
#include <map>
#include <pthread.h>
#include <time.h>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <assert.h>

#include "timing_model.h"
#include "utils.h"


enum ThreadType{
    Traceworker, Ftl_thread, Page_promotion_thread
};

struct T_event_time{
    ThreadType t_type;
    int64_t f_time;
};

struct Customcompare
{
    bool operator()(const T_event_time l, const T_event_time r) const { return l.f_time > r.f_time; }
};


class sim_clock{

    public:
    int64_t time_tick;

    int64_t next_time_door;
    bool next_time_exist;

    int total_worker_num;
    int waiting_worker_num;
    std::mutex waiting_worker_m;

    bool ftl_thread_waiting;
    std::mutex ftl_thread_m;
    bool promotion_thread_waiting;
    std::mutex promotion_thrad_m;
    bool terminate_flag;

    sim_clock(int64_t init_time, int worker_num);
    std::mutex clock_mutex;
    std::condition_variable clock_cv;
    std::mutex workers_mutex;
    std::condition_variable workers_cv;
    std::priority_queue<T_event_time, std::vector<T_event_time>, Customcompare> event_queue; 
    std::mutex queue_mutex;
    std::mutex print_mutex;
    int64_t get_time_sim();
    void enqueue_future_time(int64_t f_time, ThreadType thre_t, int core_id);
    void check_pop_and_incre_time();
    void check_pop_and_incre_time_macsim(int64_t cur_time);
    void wait_for_futuretime(int64_t f_time, int core_id);
    int give_queue_size();
    void wait_without_events(ThreadType thre_t, int core_id);
    void release_without_events(ThreadType thre_t, int core_id);
    void force_finish();
};



















#endif