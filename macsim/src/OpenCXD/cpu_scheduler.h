#ifndef CPU_SCHEDULER_H
#define CPU_SCHEDULER_H
#include <vector>
#include <string>
#include <queue>
#include <cstring>
#include <iostream>
#include <mutex>
#include <atomic>
#include <memory>
#include <filesystem>

#include "trace_utils.h"
#include "dram_ctrl.h"

using std::vector;
using std::queue;
using std::priority_queue;
using std::string;
using std::mutex;
using std::pair;
using std::atomic;

extern uint64_t rr_timeslice;

void *traceReplayThread(void *thread_args);
void *schedulerThread(void *thread_args);

struct not_ready_thread {
    not_ready_thread(uint64_t time_in, Thread* thread_ptr)
    :ready_time(time_in), thread(thread_ptr){}
    uint64_t ready_time;
    Thread* thread;
};
struct GreaterThanByTime
{
  bool operator()(const not_ready_thread& lhs, const not_ready_thread& rhs) const
  {return lhs.ready_time > rhs.ready_time;}
};

class Thread {
    public:
        Thread(int thread_id, string tracefile, uint64_t offset = 0, double scale = 1);
        uint8_t getStatus(replay_progress *progress);

        const int thread_id;

        volatile uint8_t started;
        volatile uint8_t finished;
        volatile uint64_t finish_time;

        const uint64_t time_offset;
        const double time_scale;

        friend class ThreadScheduler;
        friend class dram_ctrl_c;
        friend void *traceReplayThread(void *trace_args);

        std::priority_queue<int64_t, std::vector<int64_t>, std::greater<int64_t>> lsq;

    public:
        uint64_t last_scheduled_time;
        uint64_t last_descheduled_time;

        uint64_t last_inst_timestamp;
        //HAOYANG's implementation
        uint64_t last_issue_time;
        uint64_t last_latency;
        int64_t delayed_ns = 0;
        int64_t last_prea_addr;
        bool last_prea = false;
        bool just_cs = false;

        uint64_t num_instruction = 0;
        uint64_t num_instruction_r = 0;
        uint64_t num_instruction_w = 0;
        double running_avg_ltc = 0;

        TraceReplayUnit replay_unit;

        volatile uint64_t current_time_diff;
        volatile double current_avg;

        //Timing model prints
        uint64_t num_instruction_host_hit = 0;
        double running_avg_ltc_host_hit = 0;
        uint64_t num_instruction_log_w = 0;
        double running_avg_ltc_log_w = 0;
        uint64_t num_log_compactions = 0;
        double avg_ltc_log_compactions = 0;
        uint64_t num_instruction_log_r = 0;
        double running_avg_ltc_log_r = 0;
        uint64_t num_instruction_cache_h = 0;
        double running_avg_ltc_cache_h = 0;
        uint64_t num_instruction_cache_m = 0;
        double running_avg_ltc_cache_m = 0;
        uint64_t num_context_switches = 0;

        double running_avg_ltc_host_hit_cs = 0;
        double running_avg_ltc_log_r_cs = 0;
        double avg_ltc_log_compactions_cs = 0;
        double running_avg_ltc_cache_h_cs = 0;
        double running_avg_ltc_cache_m_cs = 0;
        double running_avg_ltc_log_w_cs = 0;

        uint64_t num_instruction_host_hit_cs = 0;
        uint64_t num_instruction_log_w_cs = 0;
        uint64_t num_log_compactions_cs = 0;
        uint64_t num_instruction_log_r_cs = 0;
        uint64_t num_instruction_cache_h_cs = 0;
        uint64_t num_instruction_cache_m_cs = 0;

        // statistics
        uint64_t real_start;
        uint64_t real_end;
        uint64_t trace_start;
        uint64_t trace_end;
        uint64_t total;
        uint64_t waited;
        int64_t delayed;
        uint64_t latency_cdf[551];
};

struct logical_core_args {
    CPULogicalCore *logical_core;
    ThreadScheduler *scheduler;
};

struct logical_core_returns {
    uint64_t total_inst = 0;
    uint64_t waited_inst = 0;
    uint64_t delayed_ns = 0;
};

class CPULogicalCore {
    public:
        CPULogicalCore(int core_id, ThreadScheduler *scheduler,
                       volatile uint8_t *start_flag,
                       volatile uint8_t *ready_flag,
                       volatile uint8_t *terminate_flag);
        inline bool scheduleThread(Thread *thread);
        inline Thread *forceScheduleThread(Thread *thread);
        inline void descheduleIfFinish();

        void spawnServiceThread();
        void joinServiceThread(logical_core_returns **returns);

        uint8_t getStatus(logical_core_status *progress);

        const int core_id;
        ThreadScheduler *const scheduler;
        volatile uint8_t should_schedule;
        
        friend void *traceReplayThread(void *thread_args);
        friend class Thread;
        friend class ThreadScheduler;
        std::unique_ptr<std::mutex> scheduled_thread_mutex;

    private:
        Thread *volatile current_thread;
        Thread *volatile scheduled_thread;

        volatile uint8_t *const start_flag;
        volatile uint8_t *const ready_flag;
        volatile uint8_t *const terminate_flag;

        volatile uint64_t current_time_diff;
        volatile uint64_t last_finished_thread_exit_time;

        volatile uint64_t total_ctx_swh_time;
        volatile uint64_t total_busy_time;

        volatile uint64_t core_first_assign_thread_time;
        volatile uint64_t current_inst_finish_time;

        logical_core_args args;
        pthread_t pthread_info;
        
};

struct scheduler_args {
    ThreadScheduler *scheduler;
};

struct scheduler_returns {
    uint64_t total = 0;
    uint64_t waited = 0;
    int64_t delayed = 0;
};

// auto thread_io_wait_cmp = [](const pair<uint64_t, Thread *> &left, const pair<uint64_t, Thread *> &right) {
//     return left.first < right.first;
// };

class ThreadScheduler {
    public:
        ThreadScheduler(vector<string> &tracefiles, int core_num, uint64_t start_timestamp,
                        uint64_t time_quanta, uint64_t ctx_swh_deadtime,
                        double replay_speed_scale = 1, int spawn_first = 1);
        void spawnServiceThread();
        void startExecution();
        void joinAll(vector<logical_core_returns> &logical_core_returns_list);
        
        void threadFork(uint64_t thread_id);
        void threadYield(CPULogicalCore *core, uint64_t ready_time);

        uint64_t getTimeSinceStart();
        void setOutputFile(FILE *output_tty, string output_tty_name);
        double getThreadsProgresses(int status);
        Thread* get_thread(int thread_id){ return &threads[thread_id];};
        
        uint64_t getTimeQuanta() { return time_quanta; }
        void printStaticCDF();

        int total_thread_num;
        const int total_core_num;
        const uint64_t time_quanta;
        const uint64_t ctx_swh_deadtime;

        atomic_int active_thread_num{0};

        // scheduler thread flag
        volatile uint8_t start_flag;
        volatile uint8_t ready_flag;
        volatile uint8_t terminate_flag;

        friend void *traceReplayThread(void *thread_args);
        friend void *schedulerThread(void *thread_args);
        friend class Thread;
    private:
        volatile uint64_t logical_cores_started_time;

        // managed logical core flags
        volatile uint8_t logical_core_start_flag;
        volatile uint8_t *const logical_core_ready_flags;
        volatile uint8_t *const logical_core_terminate_flags;

        vector<CPULogicalCore> logical_cores;
        vector<Thread> threads;
        
        mutex thread_wait_queue_mutex;
        mutex thread_not_ready_queue_mutex;
        queue<Thread *> thread_wait_queue;
        priority_queue<not_ready_thread, vector<not_ready_thread>, GreaterThanByTime> thread_not_ready_queue;

        scheduler_args args;
        pthread_t pthread_info;

        FILE *output_tty;
        string output_tty_name;
};

#endif
