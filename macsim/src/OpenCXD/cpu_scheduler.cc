#include <assert.h>

#include "trace_utils.h"
#include "cpu_scheduler.h"
#include "not_ftl.h"
#include "simulator_clock.h"
#include "utils.h"

using std::vector;
using std::queue;
using std::string;
using std::max;
using std::min;
using std::to_string;

extern FILE *output_file;
extern bool device_triggered_ctx_swt;
extern bool use_macsim;
extern bool pinatrace_drive;
extern param param;
extern long skybyte_global_write_count;
extern long skybyte_global_read_count;

sim_clock* the_clock_pt;

bool print_timing_model;

Thread::Thread(int thread_id, string tracefile, uint64_t offset, double scale) :
        thread_id(thread_id),
        started(0), finished(0), finish_time(0),
        time_offset(offset), time_scale(scale),
        last_scheduled_time(0), last_descheduled_time(0), num_instruction(0),
        running_avg_ltc(0),
        last_inst_timestamp(0), last_latency(0), last_issue_time(0),
        real_start(0), real_end(0), trace_start(0), trace_end(0),
        total(0), waited(0), delayed(0) {
    memset(latency_cdf, 0, sizeof(latency_cdf));
    if (pinatrace_drive)
    {
        assert(replay_unit.init(tracefile));
    }

    lsq = std::priority_queue<int64_t, std::vector<int64_t>, std::greater<int64_t>>();
}

uint8_t Thread::getStatus(replay_progress *progress) {
    replay_unit.getStatus(progress);
    return finished;
}

void *traceReplayThread(void *thread_args) {
    // input and return parameters
    logical_core_args *args = static_cast<logical_core_args *>(thread_args);
    logical_core_returns returns;

    ThreadScheduler &scheduler = *(args->scheduler);
    CPULogicalCore &this_core = *(args->logical_core);

    Thread *volatile *const current_thread_ptr = &this_core.current_thread;
    Thread *const volatile *const scheduled_thread_ptr = &this_core.scheduled_thread;

    queue<Thread *> &thread_wait_queue = scheduler.thread_wait_queue;
    mutex &thread_wait_queue_mutex = scheduler.thread_wait_queue_mutex;
    
    const uint64_t ctx_swh_deadtime = args->scheduler->ctx_swh_deadtime;

    volatile trace_sample *sample = nullptr;
    volatile uint64_t *const core_time_diff = &this_core.current_time_diff;
    printf("Logical core (%02d-%16lx): created\n", this_core.core_id, (uint64_t) args->logical_core);

    volatile uint8_t *const start_flag = this_core.start_flag;
    volatile uint8_t *const ready_flag = this_core.ready_flag;
    volatile uint8_t *const terminate_flag = this_core.terminate_flag;
    // signal back for setup completed
    *ready_flag = 1;
    // wait until start is signaled
    while (*start_flag == 0)
        sched_yield();

    printf("Logical core (%02d-%16lx): begin\n", this_core.core_id, (uint64_t) args->logical_core);
    // parameter reinitialize
    the_clock_pt->wait_without_events(ThreadType::Traceworker, this_core.core_id);
    while (!*scheduled_thread_ptr){
        sched_yield();
    }
    the_clock_pt->release_without_events(ThreadType::Traceworker, this_core.core_id);
    printf("Logical core (%02d-%16lx): thread assigned\n", this_core.core_id, (uint64_t) args->logical_core);

    // real time
    // time delta
    int64_t current_evt_dt, ctx_swh_dt;
    // trace related
    uint64_t timestamp = (uint64_t) -1, addr, size;
    uint64_t time_offset = 0, time_scale = 1;
    bool is_fork, is_write;
    // runtime info
    volatile int waited;
    issue_response resp;

    uint64_t logical_cores_started_time = (uint64_t) -1;
    while (logical_cores_started_time == (uint64_t) -1)
        logical_cores_started_time = scheduler.logical_cores_started_time;
    this_core.core_first_assign_thread_time = the_clock_pt->get_time_sim();  //TODO: replace, done

    bool is_not_scheduled = false;

    while (*terminate_flag == 0) {
context_switch:
        // check for context switch
        Thread *current_thread, *scheduled_thread;
        uint64_t thread_exchange_timestamp;
        bool is_first_time_pass = true;
        do {
            /**
             * current_thread and current_thread_ptr is completely controlled by this thread
             * (including during threadYield, as it is exclusively called by this thread), holds
             * the thread that is currently running. scheduled_thread and scheduled_thread_ptr
             * is not controlled by this thread, indicates the thread that should be scheduled.
             */
            // Take a snapshot of the scheduled_thread. The scheduled_thread_ptr is only accessed
            // once during one iteration of scheduling, as the underlying value might change.
            // This depends on loading from the scheduled_thread_ptr. It will be safer to declare it
            // atomic.
            if (!is_not_scheduled && !is_first_time_pass)
            {
                is_not_scheduled = true;
                the_clock_pt->wait_without_events(ThreadType::Traceworker, this_core.core_id);  //one-time
            }

            current_thread = *current_thread_ptr;
            scheduled_thread = *scheduled_thread_ptr;
            // if the scheduled_thread is different from current_thread, it means that we need to
            // schedule the scheduled_thread now
            if (scheduled_thread != current_thread) {
                // context switch timestamp
                thread_exchange_timestamp = the_clock_pt->get_time_sim() - logical_cores_started_time;  //TODO: replace, done
                // if scheduled_thread exists, record and get necessary stat/execution info from it
                if (scheduled_thread && !scheduled_thread->finished) {
                    scheduled_thread->last_scheduled_time = thread_exchange_timestamp;
                    time_offset = scheduled_thread->time_offset;
                    time_scale = scheduled_thread->time_scale;
                }
                // if current_thread exists, record necessary stat from it
                if (current_thread && !current_thread->finished) {
                    current_thread->last_descheduled_time = thread_exchange_timestamp;
                    // if current_thread is not finished, put it back to wait queue
                    thread_wait_queue_mutex.lock();
                    thread_wait_queue.push(current_thread);
                    thread_wait_queue_mutex.unlock();
                }
                // switching current_thread and current_thread_ptr
                if (!scheduled_thread || (scheduled_thread && !scheduled_thread->finished)) {
                    current_thread = scheduled_thread;
                    *current_thread_ptr = current_thread;
                }
            }
            if (*terminate_flag != 0) {
                current_thread = nullptr;
                goto core_terminate;
            }
            is_first_time_pass = false;
        } while (current_thread == nullptr);

        if (is_not_scheduled)
        {
            is_not_scheduled = false;
            the_clock_pt->release_without_events(ThreadType::Traceworker, this_core.core_id);
        }
        

        // context switch overhead
        uint64_t current_ctx_swh_overhead;

        int64_t currr_time = the_clock_pt->get_time_sim();

        // the_clock_pt->print_mutex.lock();
        // std::cout<<"Core_id "<<this_core.core_id<<", PART 1"<<std::endl;
        // the_clock_pt->print_mutex.unlock();
        the_clock_pt->enqueue_future_time(currr_time + ctx_swh_deadtime, ThreadType::Traceworker, this_core.core_id);
        the_clock_pt->wait_for_futuretime(currr_time + ctx_swh_deadtime, this_core.core_id);

        //while ((current_ctx_swh_overhead = get_time_ns() - logical_cores_started_time - thread_exchange_timestamp) < ctx_swh_deadtime) {};    //TODO: replace, done
        this_core.total_ctx_swh_time += the_clock_pt->get_time_sim() - currr_time;
        current_thread->just_cs = true;

        // check for issuing
        uint64_t current_busy_start = the_clock_pt->get_time_sim();    //TODO: replace, done
        while ((sample = current_thread->replay_unit.getCurrentTraceSample()) != nullptr) {
            timestamp = (uint64_t)((double)(sample->ns - time_offset) * current_thread->time_scale);
            addr = sample->addr;
            is_fork = sample->r == 'F';
            is_write = sample->r == 'W';
            size = sample->size;

            // if this is a fork, fork the thread and move on to the next instruction immediately
            if (is_fork) {
                scheduler.threadFork(addr);
                current_thread->replay_unit.getNextTraceSample();
                continue;
            }

            uint64_t last_inst_timestamp = current_thread->last_inst_timestamp;
            uint64_t schedule_inst_delta = last_inst_timestamp == 0 ? 0 : timestamp - last_inst_timestamp;
            
            waited = 0;

            // // calculate the instruction finish timing and wait until that time
            // uint64_t min_issue_interval = max(current_thread->last_latency, schedule_inst_delta);

            // Now change to non-blocking interface
            uint64_t min_issue_interval = schedule_inst_delta;


            uint64_t last_issue_time = current_thread->last_issue_time;
            if (last_issue_time==0) {
                current_evt_dt = 0;
                last_issue_time = the_clock_pt->get_time_sim();    //TODO: replace, done
                current_thread->delayed_ns = 0;
            }

            // do I need to do a context switch now?
            if (*scheduled_thread_ptr != current_thread) {
                uint64_t ctx_swh_start_time = the_clock_pt->get_time_sim();    //TODO: replace
                this_core.current_inst_finish_time = ctx_swh_start_time;
                this_core.total_busy_time += ctx_swh_start_time - current_busy_start;
                goto finish_checking;
            }

            int64_t min_next_issue_time;
            int64_t fastest_lsq_available_time = 0;
            
            //First dequeue all past waiting events
            while (!current_thread->lsq.empty() && current_thread->lsq.top() <= the_clock_pt->get_time_sim())
            {
                current_thread->lsq.pop();
            }
            //Then check
            while (current_thread->lsq.size()>= 64)  //TODO: we can change size
            {
                fastest_lsq_available_time = current_thread->lsq.top();
                current_thread->lsq.pop();
            }
            
            min_next_issue_time = max((int64_t)(last_issue_time + min_issue_interval), fastest_lsq_available_time);

            if (the_clock_pt->get_time_sim() < min_next_issue_time)
            {
                assert(min_next_issue_time > the_clock_pt->get_time_sim());
                // the_clock_pt->print_mutex.lock();
                // std::cout<<"Core_id "<<this_core.core_id<<", PART 2"<<std::endl;
                // the_clock_pt->print_mutex.unlock();
                the_clock_pt->enqueue_future_time(min_next_issue_time, ThreadType::Traceworker, this_core.core_id);
                the_clock_pt->wait_for_futuretime(min_next_issue_time, this_core.core_id);
            }
            
            current_evt_dt = the_clock_pt->get_time_sim() - last_issue_time - min_issue_interval;

            // while ((current_evt_dt = ((int64_t) get_time_ns() - last_issue_time) - min_issue_interval) < 0) {   //TODO: replace, done
            //     waited = 1;
            //     // // do I need to do a context switch now?
            //     // if (*scheduled_thread_ptr != current_thread) {
            //     //     uint64_t ctx_swh_start_time = get_time_ns();    //TODO: replace, done
            //     //     this_core.current_inst_finish_time = ctx_swh_start_time;
            //     //     this_core.total_busy_time += ctx_swh_start_time - current_busy_start;
            //     //     goto finish_checking;
            //     // }
            // }


            bool last_cs = false;
            if (current_thread->last_prea)
            {
                current_thread->last_prea = false;
                last_cs = true;
            }
            

            byte_issue(is_write, addr, size, &resp);
            

            // if a preemption is signaled, yield this thread immediately
            if (resp.flag & issue_status::ONGOING_DELAY) {
                // If the number of current running threads is less than number of cores, do not trigger context switch
                // if (scheduler.active_thread_num.load() < scheduler.total_core_num)
                // {
                //     resp.flag = issue_status::SSD_CACHE_MISS;
                //     resp.latency = resp.latency + resp.estimated_latency;
                // }
                // else
                // {
                    uint64_t preemption_start_time = the_clock_pt->get_time_sim();     //TODO: replace, done
                    current_thread->last_prea = true;
                    this_core.current_inst_finish_time = preemption_start_time;
                    this_core.total_busy_time += preemption_start_time - current_busy_start;
                    scheduler.threadYield(&this_core, resp.estimated_latency + preemption_start_time);
                    this_core.total_ctx_swh_time += the_clock_pt->get_time_sim() - preemption_start_time;     //TODO: replace, done
                    goto context_switch;
                // }
            }


            // update the thread last issue time if preemption does not occur 
            current_thread->last_issue_time = min_issue_interval + last_issue_time + current_evt_dt;


            //Enqueue the LSQ
            current_thread->lsq.push(current_thread->last_issue_time + resp.latency);
            
            // current instruction succefully commited
            current_thread->last_inst_timestamp = timestamp;
            current_thread->last_latency = resp.latency;

            current_thread->running_avg_ltc = (current_thread->running_avg_ltc*current_thread->num_instruction + resp.latency) / (current_thread->num_instruction+1);

            current_thread->num_instruction++;
            if (resp.latency < 1000)            current_thread->latency_cdf[resp.latency / 10]++;
            else if (resp.latency < 10000)      current_thread->latency_cdf[resp.latency / 100 + 90]++;
            else if (resp.latency < 100000)     current_thread->latency_cdf[resp.latency / 1000 + 180]++;
            else if (resp.latency < 1000000)    current_thread->latency_cdf[resp.latency / 10000 + 270]++;
            else if (resp.latency < 10000000)   current_thread->latency_cdf[resp.latency / 100000 + 360]++;
            else if (resp.latency < 100000000)  current_thread->latency_cdf[resp.latency / 1000000 + 450]++;
            else                                current_thread->latency_cdf[550]++;

            if (print_timing_model) {
                if (resp.flag & issue_status::HOST_DRAM_HIT) {
                    current_thread->running_avg_ltc_host_hit = (current_thread->running_avg_ltc_host_hit*current_thread->num_instruction_host_hit + resp.latency) / (current_thread->num_instruction_host_hit+1);
                    current_thread->num_instruction_host_hit++;
                } else if (resp.flag & issue_status::WRITE_LOG_W) {
                    current_thread->running_avg_ltc_log_w = (current_thread->running_avg_ltc_log_w*current_thread->num_instruction_log_w + resp.latency) / (current_thread->num_instruction_log_w+1);
                    current_thread->num_instruction_log_w++;
                } else if (resp.flag & issue_status::SSD_CACHE_HIT) {
                    current_thread->running_avg_ltc_cache_h = (current_thread->running_avg_ltc_cache_h*current_thread->num_instruction_cache_h + resp.latency) / (current_thread->num_instruction_cache_h+1);
                    current_thread->num_instruction_cache_h++;
                } else if (resp.flag & issue_status::WRITE_LOG_R) {
                    current_thread->running_avg_ltc_log_r = (current_thread->running_avg_ltc_log_r*current_thread->num_instruction_log_r + resp.latency) / (current_thread->num_instruction_log_r+1);
                    current_thread->num_instruction_log_r++;
                } else if (resp.flag & issue_status::SSD_CACHE_MISS) {
                    current_thread->running_avg_ltc_cache_m = (current_thread->running_avg_ltc_cache_m*current_thread->num_instruction_cache_m + resp.latency) / (current_thread->num_instruction_cache_m+1);
                    current_thread->num_instruction_cache_m++;
                }    
            }
            

            // statistics for return values of this thread
            returns.total_inst += 1;
            returns.waited_inst += waited;
            if (!last_cs && !current_thread->just_cs)
            {
                current_thread->delayed_ns += current_evt_dt;
            }
            if (current_thread->just_cs)
            {
                current_thread->just_cs = false;
            }
            

            // statistics for this core and the thread that this core is currently running
            // *core_time_diff = current_evt_dt;
            current_thread->current_avg = current_thread->running_avg_ltc;
            current_thread->replay_unit.getNextTraceSample();

            // advance busy time timer
            uint64_t busy_timer_end_time = the_clock_pt->get_time_sim();    //TODO: replace, done
            this_core.current_inst_finish_time = busy_timer_end_time;
            this_core.total_busy_time += busy_timer_end_time - current_busy_start;
            current_busy_start = busy_timer_end_time;
        }

finish_checking:
        // check for thread finishing
        if (sample == nullptr && current_thread->finished == 0) {
            // thread finish here
            uint64_t thread_finish_time = the_clock_pt->get_time_sim();    //TODO: replace, done
            current_thread->finish_time = thread_finish_time;
            current_thread->finished = 1;
            scheduler.active_thread_num--;
            // terminate this thread
            *current_thread_ptr = nullptr;
            // record thread exit time
            this_core.last_finished_thread_exit_time = thread_finish_time;
        }
    }
core_terminate:
    uint64_t finish_time = the_clock_pt->get_time_sim();     //TODO: replace, done

    printf("Logical core (%02d-%16lx) finished @ %ld\n", this_core.core_id, (uint64_t) args->logical_core, finish_time);

    logical_core_returns *to_ret = new logical_core_returns();
    memcpy(to_ret, &returns, sizeof(logical_core_returns));
    return (void *) to_ret;
}

CPULogicalCore::CPULogicalCore(int core_id, ThreadScheduler *scheduler,
                               volatile uint8_t *start_flag,
                               volatile uint8_t *ready_flag,
                               volatile uint8_t *terminate_flag) :
        core_id(core_id), scheduler(scheduler), should_schedule(1),
        current_thread(nullptr), scheduled_thread(nullptr),
        start_flag(start_flag), ready_flag(ready_flag), terminate_flag(terminate_flag),
        current_time_diff(0), last_finished_thread_exit_time(0),
        total_ctx_swh_time(0), total_busy_time(0), scheduled_thread_mutex(std::make_unique<std::mutex>()),
        core_first_assign_thread_time((uint64_t) -1), current_inst_finish_time(0) {
}

inline bool CPULogicalCore::scheduleThread(Thread *thread) {
    if (current_thread != scheduled_thread)
        return false;
    scheduled_thread = thread;
    return true;
}

inline Thread *CPULogicalCore::forceScheduleThread(Thread *thread) {
    Thread *descheduled_thread = nullptr;
    if (current_thread != scheduled_thread) {
        descheduled_thread = scheduled_thread;
    }
    scheduled_thread_mutex->lock();
    scheduled_thread = thread;
    scheduled_thread_mutex->unlock();
    return descheduled_thread;
}

inline void CPULogicalCore::descheduleIfFinish() {
    scheduled_thread_mutex->lock();
    if (scheduled_thread && scheduled_thread->finished)
        scheduled_thread = nullptr;
    scheduled_thread_mutex->unlock();
}

void CPULogicalCore::spawnServiceThread() {
    args.logical_core = this;
    args.scheduler = scheduler;
    pthread_create(&pthread_info, nullptr, traceReplayThread, (void *) &args);
    pthread_setname(pthread_info, string("logicore #" + to_string(core_id)).c_str());
    int cpu_idx = pthread_bind(pthread_info);
    printf("Logical core (%02d) service thread (%16lx) CPU: %d, initializing\n", core_id, (uint64_t) this, cpu_idx);
    while (ready_flag == 0) {
        sched_yield();
    }
    printf("Logical core (%02d) service thread (%16lx) initialized\n", core_id, (uint64_t) this);
}

void CPULogicalCore::joinServiceThread(logical_core_returns **returns) {
    pthread_join(pthread_info, (void **) returns);
}

uint8_t CPULogicalCore::getStatus(logical_core_status *progress) {
    progress->current_thread = current_thread;
    progress->scheduled_thread = scheduled_thread;
    return 0;
}

void *schedulerThread(void *thread_args) {
    // input and return parameters
    scheduler_args *args = static_cast<scheduler_args *>(thread_args);
    volatile uint8_t *const start_flag = &args->scheduler->start_flag;
    volatile uint8_t *const ready_flag = &args->scheduler->ready_flag;
    volatile uint8_t *const terminate_flag = &args->scheduler->terminate_flag;

    uint64_t total_core_num = args->scheduler->total_core_num;
    const uint64_t time_quanta = args->scheduler->time_quanta;

    vector<CPULogicalCore> &logical_cores = args->scheduler->logical_cores;
    vector<Thread> &threads = args->scheduler->threads;
    auto &thread_wait_queue = args->scheduler->thread_wait_queue;
    auto &thread_not_ready_queue = args->scheduler->thread_not_ready_queue;
    mutex &thread_wait_queue_mutex = args->scheduler->thread_wait_queue_mutex;
    mutex &thread_not_ready_queue_mutex = args->scheduler->thread_not_ready_queue_mutex;
    printf("Scheduler thread ready!\n");

    // scheduler thread initialization
    volatile uint64_t iter_start_time, iter_end_time;
    int64_t iter_sleep_time;
    timespec wait_time;
    ns_to_timespec(args->scheduler->getTimeQuanta(), wait_time);
    // signal back for setup completed
    *ready_flag = 1;
    // wait until start is signaled
    while (*start_flag == 0)
        sched_yield();


    while (*terminate_flag == 0) {
        if (use_macsim && !pinatrace_drive)
        {
            the_clock_pt->enqueue_future_time(the_clock_pt->get_time_sim() + 10000000, ThreadType::Traceworker, 0);
            the_clock_pt->wait_for_futuretime(the_clock_pt->get_time_sim() + 10000000, 0);
            continue;
        }
        //TODO: adjust for macsim  
        
        iter_start_time = the_clock_pt->get_time_sim();  
        // Do all scheduing tasks in between BEGIN
        thread_not_ready_queue_mutex.lock();

        while (!thread_not_ready_queue.empty() && thread_not_ready_queue.top().ready_time < the_clock_pt->get_time_sim()) { 
            thread_wait_queue_mutex.lock();
            thread_wait_queue.push(thread_not_ready_queue.top().thread);
            thread_wait_queue_mutex.unlock();
            thread_not_ready_queue.pop();
        }
        thread_not_ready_queue_mutex.unlock();
        
        // get random starting point for the scheduler so that the work done by each of the core is more even
        uint64_t start_core_idx = get_time_ns() % total_core_num; // a bad rng, but hey, it works  
        for (int idx = 0; idx < total_core_num; idx++) {
            CPULogicalCore &core = logical_cores[(start_core_idx + idx) % total_core_num];
            core.descheduleIfFinish();
            thread_wait_queue_mutex.lock();
            // if the core does not allow scheduling, skip it (this condition resides in lock region as this should be serialized)
            // if there is nothing in the thread queue, skip it
            // if the core already have a scheduled thread, skip it
            if (core.should_schedule && thread_wait_queue.size() > 0 &&
                core.scheduleThread(thread_wait_queue.front())) {
                thread_wait_queue.pop();
            }
            thread_wait_queue_mutex.unlock();
        }
        // Do all scheduing tasks in between END
        iter_end_time = the_clock_pt->get_time_sim();  //TODO: replace, done
        iter_sleep_time = (int64_t) iter_start_time + time_quanta - iter_end_time;
        // sleep until next scheduling time
        if (iter_sleep_time > 0){
            the_clock_pt->enqueue_future_time(iter_end_time + iter_sleep_time, ThreadType::Traceworker, total_core_num);
            the_clock_pt->wait_for_futuretime(iter_end_time + iter_sleep_time, total_core_num);
            // sleepns(iter_sleep_time);
        }
    }
    printf("Scheduler thread done!\n");
    return nullptr;
}


ThreadScheduler::ThreadScheduler(vector<string> &tracefiles, int core_num, uint64_t start_timestamp,
                                 uint64_t time_quanta, uint64_t ctx_swh_deadtime,
                                 double replay_speed_scale, int spawn_first) : 
        total_thread_num(tracefiles.size()), total_core_num(core_num),
        time_quanta(time_quanta), ctx_swh_deadtime(ctx_swh_deadtime),
        start_flag(0), ready_flag(0), terminate_flag(0),
        logical_cores_started_time((uint64_t) -1),
        logical_core_start_flag(0),
        logical_core_ready_flags(new uint8_t[total_core_num]),
        logical_core_terminate_flags(new uint8_t[total_core_num]) {
    memset((void *) logical_core_ready_flags, 0, sizeof(uint8_t) * total_core_num);
    memset((void *) logical_core_terminate_flags, 0, sizeof(uint8_t) * total_core_num);
    // initialize all logical cores
    for (int core_idx = 0; core_idx < total_core_num; core_idx++) {
        logical_cores.emplace_back(core_idx, this,
                &logical_core_start_flag,
                &logical_core_ready_flags[core_idx],
                &logical_core_terminate_flags[core_idx]);
    }
    // initialize active_thread_num
    active_thread_num = spawn_first;

    if (!pinatrace_drive){
        total_thread_num = param.sim_thread_num;
        std::cout<<"Total thread num: "<<total_thread_num<<std::endl;
    }

    // initialize all threads
    for (int thread_idx = 0; thread_idx < total_thread_num; thread_idx++) {
        string filename = tracefiles[thread_idx%tracefiles.size()];
        threads.emplace_back(thread_idx, filename, start_timestamp, replay_speed_scale);
    }

    if (pinatrace_drive)
    {
        for (int core_idx = 0; core_idx < total_core_num; core_idx++) {
            logical_cores[core_idx].spawnServiceThread();
        }
    }
    
    // only the first spawn_first threads is considered started
    int spawn_idx = 0;
    for (auto thread_iter = threads.begin();
         thread_iter != threads.end() && spawn_idx < spawn_first;
         ++thread_iter, ++spawn_idx) {
        thread_iter->started = 1;
        thread_wait_queue.push(&*thread_iter);
    }
    // schedule initial threads to cores
    for (int core_idx = 0; core_idx < total_core_num; core_idx++) {
        if (thread_wait_queue.size() > 0) {
            logical_cores[core_idx].scheduleThread(thread_wait_queue.front());
            thread_wait_queue.pop();
        }
    }
    // start all the logical core threads here
    logical_core_start_flag = 1;
}

void ThreadScheduler::spawnServiceThread() {
    args.scheduler = this;
    pthread_create(&pthread_info, nullptr, schedulerThread, (void *) &args);
    pthread_setname(pthread_info, "scheduler");
    int cpu_idx = pthread_bind(pthread_info);
    printf("Scheduler service thread CPU: %d, time quanta: %ld ns (%.3f us), initializing\n",
            cpu_idx, time_quanta, time_quanta / 1e3);
    while (ready_flag == 0) {
        sched_yield();
    }
    printf("Scheduler service thread initialized\n");
}

void ThreadScheduler::startExecution() {
    logical_cores_started_time = 0; 
    start_flag = 1;
}

void ThreadScheduler::joinAll(vector<logical_core_returns> &logical_core_returns_list) {
    logical_core_returns *logical_core_returns;
    memset((void *) logical_core_terminate_flags, 1, sizeof(uint8_t) * total_core_num);
    printStaticCDF();

    if (pinatrace_drive)
    {
        for (auto logical_core_iter = logical_cores.begin(); logical_core_iter != logical_cores.end(); ++logical_core_iter) {
            logical_core_iter->joinServiceThread(&logical_core_returns);
            logical_core_returns_list.push_back(*logical_core_returns);
        }
    }

    terminate_flag = 1;
}

void ThreadScheduler::printStaticCDF() {
    uint64_t total_cdf[551];
    double avg_latency = 0;
    int64_t num_inst = 0;
    int64_t num_inst_host_hit = 0;
    int64_t num_inst_log_w = 0;
    int64_t num_inst_cache_h = 0;
    int64_t num_inst_log_r = 0;
    int64_t num_inst_cache_m = 0;
    int64_t num_inst_host_hit_cs = 0;
    int64_t num_inst_log_w_cs = 0;
    int64_t num_inst_cache_h_cs = 0;
    int64_t num_inst_log_r_cs = 0;
    int64_t num_inst_cache_m_cs = 0;
    memset(total_cdf ,0, sizeof(total_cdf));
    for(vector<Thread>::iterator it = threads.begin(); it != threads.end(); it++) {
        if (it->started && it->num_instruction > 0)
        {
            for (uint32_t i = 0; i < sizeof(it->latency_cdf) / sizeof(uint64_t); i++) {
                total_cdf[i] += it->latency_cdf[i];
            }
            avg_latency = ((it->current_avg) * (it->num_instruction) + avg_latency * num_inst) / (num_inst + it->num_instruction);
            num_inst += it->num_instruction;
            num_inst_host_hit += it->num_instruction_host_hit;
            num_inst_log_w += it->num_instruction_log_w;
            num_inst_cache_h += it->num_instruction_cache_h;
            num_inst_log_r += it->num_instruction_log_r;
            num_inst_cache_m += it->num_instruction_cache_m;
            num_inst_host_hit_cs += it->num_instruction_host_hit_cs;
            num_inst_log_w_cs += it->num_instruction_log_w_cs;
            num_inst_cache_h_cs += it->num_instruction_cache_h_cs;
            num_inst_log_r_cs += it->num_instruction_log_r_cs;
            num_inst_cache_m_cs += it->num_instruction_cache_m_cs;
        }
    }

    fprintf(output_file, "Number of Load Instructions: %ld\n", skybyte_global_read_count);
    fprintf(output_file, "Number of Store Instructions: %ld\n", skybyte_global_write_count);
    fprintf(output_file, "Number of Memory Accesses: %ld\n", num_inst);
    fprintf(output_file, "Number of Memory Accesses (Host DRAM Hit): %ld\n", num_inst_host_hit);
    fprintf(output_file, "Number of Memory Accesses (Log Write): %ld\n", num_inst_log_w);
    fprintf(output_file, "Number of Memory Accesses (SSD Cache Hit): %ld\n", num_inst_cache_h);
    fprintf(output_file, "Number of Memory Accesses (Log Read): %ld\n", num_inst_log_r);
    fprintf(output_file, "Number of Memory Accesses (SSD Cache Miss): %ld\n", num_inst_cache_m);

    fprintf(output_file, "Number of Memory Accesses (Host DRAM Hit, Context Switch): %ld\n", num_inst_host_hit_cs);
    fprintf(output_file, "Number of Memory Accesses (Log Write, Context Switch): %ld\n", num_inst_log_w_cs);
    fprintf(output_file, "Number of Memory Accesses (SSD Cache Hit, Context Switch): %ld\n", num_inst_cache_h_cs);
    fprintf(output_file, "Number of Memory Accesses (Log Read, Context Switch): %ld\n", num_inst_log_r_cs);
    fprintf(output_file, "Number of Memory Accesses (SSD Cache Miss, Context Switch): %ld\n", num_inst_cache_m_cs);


    fprintf(output_file, "Latency_CDF_Timestamp: ");
    for (int i = 0; i < 100; i++){
        fprintf(output_file, "%d ",i*10);
    }
    for (int i = 100; i < 190; i++){
        fprintf(output_file, "%d ",1000 + (i - 100) * 100);
    }
    for (int i = 190; i < 280; i++){
        fprintf(output_file, "%d ",10000 + (i - 190) * 1000);
    }
    for (int i = 280; i < 370; i++){
        fprintf(output_file, "%d ",100000 + (i - 280) * 10000);
    }
    for (int i = 370; i < 460; i++){
        fprintf(output_file, "%d ",1000000 + (i - 370) * 100000);
    }
    for (int i = 460; i < 550; i++){
        fprintf(output_file, "%d ",10000000 + (i - 460) * 1000000);
    }
    fprintf(output_file, "100000000\n");
    
    fprintf(output_file, "Latency_CDF_Data: ");
    for (int i = 0; i < 100; i++){
        fprintf(output_file, "%ld ", total_cdf[i]);
    }
    for (int i = 100; i < 190; i++){
        fprintf(output_file, "%ld ", total_cdf[i]);
    }
    for (int i = 190; i < 280; i++){
        fprintf(output_file, "%ld ", total_cdf[i]);
    }
    for (int i = 280; i < 370; i++){
        fprintf(output_file, "%ld ", total_cdf[i]);
    }
    for (int i = 370; i < 460; i++){
        fprintf(output_file, "%ld ", total_cdf[i]);
    }
    for (int i = 460; i < 550; i++){
        fprintf(output_file, "%ld ", total_cdf[i]);
    }
    fprintf(output_file, "%ld\n", total_cdf[550]);

    //Average_latency
    fprintf(output_file, "Overall_Average_Latency: %f\n", avg_latency);
        
}

void ThreadScheduler::threadFork(uint64_t thread_id) {
    // fork thread indicated by the index thread_id provided
    if (thread_id >= total_thread_num)
        return;
    // mark the thread as started and append into wait queue
    threads[thread_id].started = 1;
    thread_wait_queue_mutex.lock();
    thread_wait_queue.push(&threads[thread_id]);
    thread_wait_queue_mutex.unlock();
    active_thread_num++;
}

void ThreadScheduler::threadYield(CPULogicalCore *core, uint64_t ready_time) {
    Thread *to_schedule_thread = nullptr;
    Thread *descheduled_thread = nullptr;
    assert(core->current_thread);

    // exclude this core from scheduler
    core->should_schedule = 0;
    
    // schedule the next thread if there is one
    thread_wait_queue_mutex.lock();
    if (thread_wait_queue.size() > 0) {
        to_schedule_thread = thread_wait_queue.front();
        thread_wait_queue.pop();
    }
    thread_wait_queue_mutex.unlock();
    // swap with the thread that is already assigned to the core
    descheduled_thread = core->forceScheduleThread(to_schedule_thread);

    // reenable scheduling
    core->should_schedule = 1;
    // scheduling will not be enalbed again until core performs context switch, as
    // scheduled_thread is now different from current_thread
    
    // core is already scheduled with a thread, return it back to wait queue
    if(descheduled_thread) {
        thread_not_ready_queue_mutex.lock();
        thread_not_ready_queue.emplace(ready_time, descheduled_thread);
        thread_not_ready_queue_mutex.unlock();
        //printf("enqueue time : %ld, thread: %d \n", get_time_ns(), descheduled_thread->thread_id);
    }
}

uint64_t ThreadScheduler::getTimeSinceStart() {
    return the_clock_pt->get_time_sim() - logical_cores_started_time;  //TODO: replace, done
}

void ThreadScheduler::setOutputFile(FILE *output_tty, string output_tty_name) {
    this->output_tty = output_tty;
    this->output_tty_name = output_tty_name;
}

double ThreadScheduler::getThreadsProgresses(int status) {

    int num_finished = 0;
    replay_progress progress_report, total_progress_report;
    memset(&total_progress_report, 0, sizeof(replay_progress));

    if (status==1)
    {
        double program_finish_time = 0;
        double program_finish_time_real = 0;
        fprintf(output_file, "### Thread Info =====================\n");
        if (!print_timing_model) {
            fprintf(output_file, "| %44s | %14s | %18s | %18s |\n", 
                     "Generic Info", "Latency", "Miscellaneous", "Finish Time");
            fprintf(output_file, "| %8s %11s / %11s = %7s | %14s | %18s | %18s |\n",
                     "ThreadID", "CurInst", "TotalInst", "Percent",
                    "Avg(Total)", "Acc(TimeDiff)", "FinishTime");
        } else {
            fprintf(output_file, "| %44s | %89s | %18s | %18s |\n", 
                     "Generic Info", "Latency", "Miscellaneous", "Finish Time");
            fprintf(output_file, "| %8s %11s / %11s = %7s | %14s %14s %14s %14s %14s %14s | %18s | %18s |\n",
                     "ThreadID", "CurInst", "TotalInst", "Percent",
                    "Avg(HostHit)", "Avg(LogRead)", "Avg(LogWrite)", "Avg(CacheHit)", "Avg(CacheMiss)", "Avg(Total)",
                    "Acc(TimeDiff)",
                    "FinishTime");
        }
        for (auto thread_iter = threads.begin(); thread_iter != threads.end(); ++thread_iter) {
            thread_iter->getStatus(&progress_report);
            if (!thread_iter->started) {
                if (pinatrace_drive)
                {
                    fprintf(output_file, "%s| %8d %35s | %14s | %18s | %18s |\n",
                            ansi_clearline, thread_iter->thread_id, "Not spawned yet", "", "", "");
                    assert(0);
                }
            
            } else if (thread_iter->finished) {
                program_finish_time = max(program_finish_time, (thread_iter->finish_time - logical_cores_started_time) / 1e9);
                // if (!device_triggered_ctx_swt)
                // {
                program_finish_time_real = max(program_finish_time_real, ((thread_iter->finish_time - logical_cores_started_time) / 1e9) - ((thread_iter->delayed_ns) / 1e9));
                // }
                if (!print_timing_model) {
                fprintf(output_file, "| %8d %11ld / %11ld = %6.2f%% | %11.2f ns | %16.9f s | %16.9f s |\n",
                         thread_iter->thread_id,
                        progress_report.inst_num, progress_report.inst_num, 100.0,
                        thread_iter->current_avg, thread_iter->delayed_ns / 1e9,
                        (thread_iter->finish_time - logical_cores_started_time) / 1e9);
                } else {
                    fprintf(output_file, "| %8d %11ld / %11ld = %6.2f%% | %11.2f ns %11.2f ns %11.2f ns %11.2f ns %11.2f ns %11.2f ns | %16.9f s | %16.9f s |\n",
                            thread_iter->thread_id,
                            progress_report.current_inst_idx, progress_report.inst_num,
                            100.0 * progress_report.current_inst_idx / progress_report.inst_num,
                            thread_iter->running_avg_ltc_host_hit,
                            thread_iter->running_avg_ltc_log_r, thread_iter->running_avg_ltc_log_w,
                            thread_iter->running_avg_ltc_cache_h, thread_iter->running_avg_ltc_cache_m,
                            thread_iter->current_avg, thread_iter->delayed_ns / 1e9,
                            (thread_iter->finish_time - logical_cores_started_time) / 1e9);
                }
            } else {
                if (pinatrace_drive)
                {
                    assert(0);
                }
                fprintf(output_file, "| %8d %11ld / %11ld = %6.2f%% | %11.2f ns %11.2f ns %11.2f ns %11.2f ns %11.2f ns %11.2f ns | %16.9f s | %16.9f s|\n",
                            thread_iter->thread_id,
                            progress_report.current_inst_idx, progress_report.inst_num,
                            100.0 * progress_report.current_inst_idx / progress_report.inst_num,
                            thread_iter->running_avg_ltc_host_hit,
                            thread_iter->running_avg_ltc_log_r, thread_iter->running_avg_ltc_log_w,
                            thread_iter->running_avg_ltc_cache_h, thread_iter->running_avg_ltc_cache_m,
                            thread_iter->current_avg, thread_iter->delayed_ns / 1e9,
                            (thread_iter->finish_time - logical_cores_started_time) / 1e9);
            }
            total_progress_report.current_inst_idx += progress_report.current_inst_idx;
            total_progress_report.inst_num += progress_report.inst_num;
        }
        // if (device_triggered_ctx_swt)
        // {
        //     program_finish_time_real = -1; //N/A
        // }
        fprintf(output_file, "\n| %8s |  %8s  |  %14s %14s %14s %14s %14s %14s |\n",
                "ThreadID", "Context Switch #", "Avg(HostHit_CS)", "Avg(LogRead_CS)", "Avg(LogWrite_CS)", "Avg(CacheHit_CS)", "Avg(CacheMiss_CS)", "Avg(LogComp_CS)");
        for (auto thread_iter = threads.begin(); thread_iter != threads.end(); ++thread_iter) {
                fprintf(output_file, "| %8d |  %8ld  |  %11.2f ns %11.2f ns %11.2f ns %11.2f ns %11.2f ns %11.2f ns |\n",
                            thread_iter->thread_id, thread_iter->num_context_switches,
                            thread_iter->running_avg_ltc_host_hit_cs,
                            thread_iter->running_avg_ltc_log_r_cs, thread_iter->running_avg_ltc_log_w_cs,
                            thread_iter->running_avg_ltc_cache_h_cs, thread_iter->running_avg_ltc_cache_m_cs,
                            thread_iter->avg_ltc_log_compactions_cs);
                }
        

        fprintf(output_file, "Program_Finish_Time(All): %10.6f\n", program_finish_time);
        fprintf(output_file, "Program_Finish_Time(Real): %10.6f\n", program_finish_time_real);

        logical_core_status core_status;
        bool all_thread_finished = num_finished == total_thread_num;
        double alltotal_idle_time = 0;
        double alltotal_cs_time = 0;
        double alltotal_busy_time = 0;
        memset(&core_status, 0, sizeof(logical_core_status));
        fprintf(output_file, "### Logical Core Info =====================\n");
        for (auto core_iter = logical_cores.begin(); core_iter != logical_cores.end(); ++core_iter) {
            core_iter->getStatus(&core_status);
            Thread *current_thread = core_status.current_thread;
            Thread *scheduled_thread = core_status.scheduled_thread;
            uint64_t elapsed_time;
            if (!all_thread_finished) {
                if (core_iter->core_first_assign_thread_time == (uint64_t) -1) {
                    elapsed_time = 0;
                } else {
                    elapsed_time = core_iter->current_inst_finish_time - core_iter->core_first_assign_thread_time;
                }
            } else {
                elapsed_time = core_iter->last_finished_thread_exit_time - core_iter->core_first_assign_thread_time;
            }
            
            uint64_t total_ctx_swh_time = core_iter->total_ctx_swh_time;
            uint64_t total_busy_time = core_iter->total_busy_time;
            fprintf(output_file, "Core #%-2d: Status: %10s [Current: %3s, Scheduled: %3s] Idle/Busy/ContextSwitch: (%10.6f%% / %10.6f%% / %10.6f%%) [%15.9f ns / %15.9f ns / %15.9f ns]\n",
                    core_iter->core_id,
                    current_thread == nullptr ? "Idle" : "Busy",
                    current_thread == nullptr ? "N/A" : to_string(current_thread->thread_id).c_str(),
                    scheduled_thread == nullptr ? "N/A" : to_string(scheduled_thread->thread_id).c_str(),
                    100.0 * (elapsed_time - total_ctx_swh_time - total_busy_time) / elapsed_time,
                    100.0 * total_busy_time / elapsed_time,
                    100.0 * total_ctx_swh_time / elapsed_time,
                    (elapsed_time - total_ctx_swh_time - total_busy_time) / 1e9,
                    total_busy_time / 1e9,
                    total_ctx_swh_time / 1e9);
            alltotal_idle_time += (elapsed_time - total_ctx_swh_time - total_busy_time) / 1e9;
            alltotal_cs_time += total_ctx_swh_time / 1e9;
            alltotal_busy_time += total_busy_time / 1e9;
        }

        fprintf(output_file, "Total_Cores_Idle_Time: %10.6f\n", alltotal_idle_time);
        fprintf(output_file, "Total_Cores_Context_Switch_Time: %10.6f\n", alltotal_cs_time);
        fprintf(output_file, "Total_Cores_Busy_Time: %10.6f\n", alltotal_busy_time);

        return 1;
    }

    // time diff explode
    uint64_t cur_time = getTimeSinceStart();
    if (cur_time > 1000000e9){
        std::cout<<"Timestamp exploded!"<<std::endl;
        //assert(0);
    }

    // fprintf(output_tty, "%s", ansi_clearscreen);
    fprintf(output_tty, "%s", ansi_reset_cursor);
    fprintf(output_tty, "%s### [%s] Progress Report @%10.3f sec \n", ansi_clearline,
            output_tty_name.c_str(), getTimeSinceStart() / 1e9);

    fprintf(output_tty, "%s### Thread Info =====================\n", ansi_clearline);
    if (!print_timing_model) {
        fprintf(output_tty, "%s| %44s | %14s | %18s | %18s |\n", 
                ansi_clearline, "Generic Info", "Latency", "Miscellaneous", "Finish Time");
        fprintf(output_tty, "%s| %8s %11s / %11s = %7s | %14s | %18s | %18s |\n",
                ansi_clearline, "ThreadID", "CurInst", "TotalInst", "Percent",
                "Avg(Total)", "Acc(TimeDiff)", "FinishTime");
    } else {
        fprintf(output_tty, "%s| %44s | %89s | %18s | %18s |\n", 
                ansi_clearline, "Generic Info", "Latency", "Miscellaneous", "Finish Time");
        fprintf(output_tty, "%s| %8s %11s / %11s = %7s | %14s %14s %14s %14s %14s %14s | %18s | %18s |\n",
                ansi_clearline, "ThreadID", "CurInst", "TotalInst", "Percent",
                "Avg(HostHit)", "Avg(LogRead)", "Avg(LogWrite)", "Avg(CacheHit)", "Avg(CacheMiss)", "Avg(Total)",
                "Acc(TimeDiff)",
                "FinishTime");
    }
    for (auto thread_iter = threads.begin(); thread_iter != threads.end(); ++thread_iter) {
        thread_iter->getStatus(&progress_report);
        if (!thread_iter->started) {
            if (!print_timing_model) {
                fprintf(output_tty, "%s| %8d %35s | %14s | %18s | %18s |\n",
                        ansi_clearline, thread_iter->thread_id, "Not spawned yet", "", "", "");
            } else {
                fprintf(output_tty, "%s| %8d %35s | %89s | %18s | %18s |\n",
                        ansi_clearline, thread_iter->thread_id, "Not spawned yet", "", "", "");
            }
        } else if (thread_iter->finished) {
            num_finished++;
            if (!print_timing_model) {
                fprintf(output_tty, "%s| %8d %11ld / %11ld = %6.2f%% | %11.2f ns | %16.9f s | %16.9f s |\n",
                        ansi_clearline, thread_iter->thread_id,
                        progress_report.inst_num, progress_report.inst_num, 100.0,
                        thread_iter->current_avg, thread_iter->delayed_ns / 1e9,
                        (thread_iter->finish_time - logical_cores_started_time) / 1e9);
            } else {
                fprintf(output_tty, "%s| %8d %11ld / %11ld = %6.2f%% | %11.2f ns %11.2f ns %11.2f ns %11.2f ns %11.2f ns %11.2f ns | %16.9f s | %16.9f s |\n",
                        ansi_clearline, thread_iter->thread_id,
                        progress_report.current_inst_idx, progress_report.inst_num,
                        100.0 * progress_report.current_inst_idx / progress_report.inst_num,
                        thread_iter->running_avg_ltc_host_hit,
                        thread_iter->running_avg_ltc_log_r, thread_iter->running_avg_ltc_log_w,
                        thread_iter->running_avg_ltc_cache_h, thread_iter->running_avg_ltc_cache_m,
                        thread_iter->current_avg, thread_iter->delayed_ns / 1e9,
                        (thread_iter->finish_time - logical_cores_started_time) / 1e9);
            }
        } else {
            if (!print_timing_model) {
                fprintf(output_tty, "%s| %8d %11ld / %11ld = %6.2f%% | %11.2f ns | %16.9f s | %18s |\n",
                        ansi_clearline, thread_iter->thread_id,
                        progress_report.current_inst_idx, progress_report.inst_num,
                        100.0 * progress_report.current_inst_idx / progress_report.inst_num,
                        thread_iter->current_avg, thread_iter->delayed_ns / 1e9,
                        "---");
            } else {
                fprintf(output_tty, "%s| %8d %11ld / %11ld = %6.2f%% | %11.2f ns %11.2f ns %11.2f ns %11.2f ns %11.2f ns %11.2f ns | %16.9f s | %18s |\n",
                        ansi_clearline, thread_iter->thread_id,
                        progress_report.current_inst_idx, progress_report.inst_num,
                        100.0 * progress_report.current_inst_idx / progress_report.inst_num,
                        thread_iter->running_avg_ltc_host_hit,
                        thread_iter->running_avg_ltc_log_r, thread_iter->running_avg_ltc_log_w,
                        thread_iter->running_avg_ltc_cache_h, thread_iter->running_avg_ltc_cache_m,
                        thread_iter->current_avg, thread_iter->delayed_ns / 1e9,
                        "---");
            }
        }
        total_progress_report.current_inst_idx += progress_report.current_inst_idx;
        total_progress_report.inst_num += progress_report.inst_num;
    }
    bool all_thread_finished = num_finished == total_thread_num;

    fprintf(output_tty, "%s### Logical Core Info ===============\n", ansi_clearline);
    logical_core_status core_status;
    memset(&core_status, 0, sizeof(logical_core_status));
    for (auto core_iter = logical_cores.begin(); core_iter != logical_cores.end(); ++core_iter) {
        core_iter->getStatus(&core_status);
        Thread *current_thread = core_status.current_thread;
        Thread *scheduled_thread = core_status.scheduled_thread;
        uint64_t elapsed_time;
        if (!all_thread_finished) {
            if (core_iter->core_first_assign_thread_time == (uint64_t) -1) {
                elapsed_time = 0;
            } else {
                elapsed_time = core_iter->current_inst_finish_time - core_iter->core_first_assign_thread_time;
            }
        } else {
            elapsed_time = core_iter->last_finished_thread_exit_time - logical_cores_started_time;
        }

        uint64_t total_ctx_swh_time = core_iter->total_ctx_swh_time;
        uint64_t total_busy_time = core_iter->total_busy_time;
        fprintf(output_tty, "%sCore #%-2d: Status: %10s [Current: %3s, Scheduled: %3s] Idle/Busy/ContextSwitch: (%10.6f%% / %10.6f%% / %10.6f%%) [%15.9f s / %15.9f s / %15.9f s]\n",
                ansi_clearline, core_iter->core_id,
                current_thread == nullptr ? "Idle" : "Busy",
                current_thread == nullptr ? "N/A" : to_string(current_thread->thread_id).c_str(),
                scheduled_thread == nullptr ? "N/A" : to_string(scheduled_thread->thread_id).c_str(),
                100.0 * (elapsed_time - total_ctx_swh_time - total_busy_time) / elapsed_time,
                100.0 * total_busy_time / elapsed_time,
                100.0 * total_ctx_swh_time / elapsed_time,
                (elapsed_time - total_ctx_swh_time - total_busy_time) / 1e9,
                total_busy_time / 1e9,
                total_ctx_swh_time / 1e9);
    }

    // nand_lun *lun;
    // for (uint64_t ch_idx = 0; ch_idx < CH_COUNT; ch_idx++) {
    //     for (uint64_t lun_idx = 0; lun_idx < WAY_COUNT; lun_idx++) {
    //         lun = ssd_get_lun(&gdev, ch_idx, lun_idx);
    //         bytefs_assert(lun);
    //         fprintf(output_tty, "%sChannel %3ld Lun %3ld R: %15ld W: %15ld Lat: %.9f s\n",
    //                 ansi_clearline, ch_idx, lun_idx, lun->nrd, lun->nwr, 
    //                 (lun->next_lun_avail_time - logical_cores_started_time) / 1e9);
    //     }
    // }

    double progress = (double) total_progress_report.current_inst_idx / total_progress_report.inst_num;
    if (all_thread_finished) {
        fprintf(output_tty, "%sAll threads finished execution.\n", ansi_clearline);
        progress = (double) 1.0;
    } else {
        uint64_t elapsed_time = getTimeSinceStart();
        uint64_t estimated_completion_time = elapsed_time / progress;
        fprintf(output_tty, "%sEstimated completion in %17.9f sec.\n",
            ansi_clearline,
            (estimated_completion_time - elapsed_time) / 1e9
        );
    }
    return progress;
}
