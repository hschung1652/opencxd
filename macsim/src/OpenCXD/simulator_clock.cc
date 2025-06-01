#include "simulator_clock.h"
#include "not_ftl.h"
#define SIM_DEBUG true

sim_clock::sim_clock(int64_t init_time, int worker_num){
    time_tick = init_time;
    total_worker_num = worker_num;
    waiting_worker_num = 0;
    ftl_thread_waiting = false;
    promotion_thread_waiting = false;
    terminate_flag = false;
    next_time_door = 0;
    next_time_exist = false;
    event_queue = std::priority_queue<T_event_time, std::vector<T_event_time>, Customcompare>();
}

int64_t sim_clock::get_time_sim(){
    int64_t ans;
    //clock_mutex.lock();
    ans = time_tick;
    //clock_mutex.unlock();
    return ans;
}

void sim_clock::enqueue_future_time(int64_t f_time, ThreadType thre_t, int core_id){
    if (thre_t == Traceworker)
    {
        waiting_worker_m.lock();
        waiting_worker_num++;
        queue_mutex.lock();
        T_event_time next_e = {thre_t, f_time};
#if SIM_DEBUG
        std::cout<<"New event is enqueued: time: "<<f_time<<", threadType=Traceworker, "<<"core_id = "<<core_id<<"."<<std::endl;
        std::cout<<"Current waiting worker number = "<<waiting_worker_num<<std::endl;
#endif
        event_queue.push(next_e);
        queue_mutex.unlock();
        waiting_worker_m.unlock();
    }
    else if (thre_t == Ftl_thread)
    {
        waiting_worker_m.lock();
        assert(ftl_thread_waiting == false);
        ftl_thread_waiting = true;
        queue_mutex.lock();
        T_event_time next_e = {thre_t, f_time};
#if SIM_DEBUG
        std::cout<<"New event is enqueued: time: "<<f_time<<", threadType=Ftl."<<std::endl;
        std::cout<<"Now the ftl_thread is waiting."<<std::endl;
#endif
        event_queue.push(next_e);
        queue_mutex.unlock();
        waiting_worker_m.unlock();
    }
    else
    {
        waiting_worker_m.lock();
        assert(promotion_thread_waiting == false);
        promotion_thread_waiting = true;
        queue_mutex.lock();
        T_event_time next_e = {thre_t, f_time};
#if SIM_DEBUG
        std::cout<<"New event is enqueued: time: "<<f_time<<", threadType=Page_promotion."<<std::endl;
        std::cout<<"Now the promotion_thread is waiting."<<std::endl;
#endif
        event_queue.push(next_e);
        queue_mutex.unlock();
        waiting_worker_m.unlock();
    } 
    workers_cv.notify_one();
}


void sim_clock::check_pop_and_incre_time(){

    //First hold the wait locks
    std::unique_lock lk(waiting_worker_m);
    workers_cv.wait(lk, [this]{return ((waiting_worker_num == total_worker_num && ftl_thread_waiting && promotion_thread_waiting) || terminate_flag);});

    if (!terminate_flag)
    {

        //Then check
        assert (waiting_worker_num == total_worker_num && ftl_thread_waiting && promotion_thread_waiting);
        // {
        int64_t next_time;
        ThreadType th_t;
        queue_mutex.lock();
        next_time = event_queue.top().f_time;
        assert(next_time > time_tick);

        while (!event_queue.empty() && event_queue.top().f_time == next_time)
        {
            th_t = event_queue.top().t_type;
            event_queue.pop();
            if (th_t == Traceworker)
            {
                waiting_worker_num--;
    #if SIM_DEBUG
                std::cout<<"The event from a trace worker is dequeued, now the waiting worker num is "<<waiting_worker_num<<std::endl;
    #endif
            }
            else if (th_t == Ftl_thread)
            {
                ftl_thread_waiting = false;
    #if SIM_DEBUG
                std::cout<<"The event from ftl_thread is dequeued, now the ftl_thread is not waiting."<<std::endl;
    #endif
            }
            else
            {
                promotion_thread_waiting = false;
    #if SIM_DEBUG
                std::cout<<"The event from promotion thread is dequeued, now the promotion thread is not waiting."<<std::endl;
    #endif
            }
        }
        
        clock_mutex.lock();
        assert(next_time > time_tick);
        time_tick = next_time;
    #if SIM_DEBUG
        std::cout<<"The time is changed to "<<next_time<<std::endl;
    #endif
        clock_mutex.unlock();
        queue_mutex.unlock();

        //Release the locks
        lk.unlock();
        clock_cv.notify_all();

    }
}


void sim_clock::wait_for_futuretime(int64_t f_time, int core_id){
    std::unique_lock lk(clock_mutex);
    clock_cv.wait(lk, [this, f_time]{ return this->time_tick >= f_time; });
    lk.unlock();
#if SIM_DEBUG
    print_mutex.lock();
    std::cout<<"Core_id: "<<core_id<<", Now the waiting for time: "<<f_time<<" is released!"<<std::endl;
    print_mutex.unlock();
#endif
    //clock_cv.notify_all();
}



void sim_clock::check_pop_and_incre_time_macsim(int64_t cur_time){
    //First hold the wait locks
    if (next_time_exist)
    {
        if (cur_time < next_time_door)
        {
            return;
        }
        else
        {
            next_time_exist = false;
        }
    }
    
    std::unique_lock lk(waiting_worker_m);
    //printf("workers_cv.wait: %d\n", waiting_worker_num);
    //workers_cv.wait(lk, [this]{return ((waiting_worker_num == total_worker_num && ftl_thread_waiting && promotion_thread_waiting) || terminate_flag);});
    //printf("done\n");

    if (!terminate_flag)
    {

        //Then check
        //assert (waiting_worker_num == total_worker_num && ftl_thread_waiting && promotion_thread_waiting);
        // {
        int64_t next_time;
        ThreadType th_t;
        queue_mutex.lock();
        next_time = event_queue.top().f_time;
        std::cout << next_time << std::endl;
        if (time_tick < next_time)
        {
            queue_mutex.unlock();
            lk.unlock();
            next_time_door = next_time;
            next_time_exist = true;
            return;
        }
        else
        {
             while (!event_queue.empty() && event_queue.top().f_time == next_time)
            {
                th_t = event_queue.top().t_type;
                event_queue.pop();
                if (th_t == Traceworker)
                {
                    waiting_worker_num--;
        #if SIM_DEBUG
                    std::cout<<"The event from a trace worker is dequeued, now the waiting worker num is "<<waiting_worker_num<<std::endl;
        #endif
                }
                else if (th_t == Ftl_thread)
                {
                    ftl_thread_waiting = false;
        #if SIM_DEBUG
                    std::cout<<"The event from ftl_thread is dequeued, now the ftl_thread is not waiting."<<std::endl;
        #endif
                }
                else
                {
                    promotion_thread_waiting = false;
        #if SIM_DEBUG
                    std::cout<<"The event from promotion thread is dequeued, now the promotion thread is not waiting."<<std::endl;
        #endif
                }
            }
        //     clock_mutex.lock();
        //     assert(next_time > time_tick);
        //     time_tick = next_time;
        // #if SIM_DEBUG
        //     std::cout<<"The time is changed to "<<next_time<<std::endl;
        // #endif
        //     clock_mutex.unlock();
            queue_mutex.unlock();
            next_time_exist = false;

            //Release the locks
            lk.unlock();
            clock_cv.notify_all();
        }
    }
}




int sim_clock::give_queue_size(){
    int size;
    queue_mutex.lock();
    size = (int)(event_queue.size());
    queue_mutex.unlock();
    return size;
}


void sim_clock::wait_without_events(ThreadType thre_t, int core_id){
    if (thre_t == Traceworker)
    {
        waiting_worker_m.lock();
        waiting_worker_num++;
#if SIM_DEBUG
        print_mutex.lock();
        std::cout<<"Core_id: "<<core_id<<", A trace worker is waiting without events now! Now the waiting worker number is "<<waiting_worker_num<<std::endl;
        print_mutex.unlock();
#endif
        waiting_worker_m.unlock();
    }
    else if (thre_t == Ftl_thread)
    {
        waiting_worker_m.lock();
        ftl_thread_waiting = true;
#if SIM_DEBUG
        print_mutex.lock();
        std::cout<<"The ftl_thread is waiting without events now!"<<std::endl;
        print_mutex.unlock();
#endif
        waiting_worker_m.unlock();
    }
    else
    {
        waiting_worker_m.lock();
        promotion_thread_waiting = true;
#if SIM_DEBUG
        print_mutex.lock();
        std::cout<<"The page promotion thread is waiting without events now!"<<std::endl;
        print_mutex.unlock();
#endif
        waiting_worker_m.unlock();
    }
    workers_cv.notify_one();
}


void sim_clock::release_without_events(ThreadType thre_t, int core_id){
    if (thre_t == Traceworker)
    {
        waiting_worker_m.lock();
        waiting_worker_num--;
#if SIM_DEBUG
        print_mutex.lock();
        std::cout<<"Core_id: "<<core_id<<", A trace worker is released without events now! Now the waiting worker number is "<<waiting_worker_num<<std::endl;
        print_mutex.unlock();
#endif
        waiting_worker_m.unlock();
    }
    else if (thre_t == Ftl_thread)
    {
        waiting_worker_m.lock();
        ftl_thread_waiting = false;
#if SIM_DEBUG
        print_mutex.lock();
        std::cout<<"The ftl_thread is released without events now!"<<std::endl;
        print_mutex.unlock();
#endif
        waiting_worker_m.unlock();
    }
    else
    {
        waiting_worker_m.lock();
        promotion_thread_waiting = false;
#if SIM_DEBUG
        print_mutex.lock();
        std::cout<<"The promotion thread is released without events now!"<<std::endl;
        print_mutex.unlock();
#endif
        waiting_worker_m.unlock();
    }
    workers_cv.notify_one();
}

void sim_clock::force_finish(){
    int64_t next_time = time_tick;
     while (!event_queue.empty())
            {
                ThreadType th_t = event_queue.top().t_type;
                next_time = event_queue.top().f_time;
                event_queue.pop();
                if (th_t == Traceworker)
                {
                    waiting_worker_num--;
        #if SIM_DEBUG
                    std::cout<<"The event from a trace worker is dequeued, now the waiting worker num is "<<waiting_worker_num<<std::endl;
        #endif
                }
                else if (th_t == Ftl_thread)
                {
                    ftl_thread_waiting = false;
        #if SIM_DEBUG
                    std::cout<<"The event from ftl_thread is dequeued, now the ftl_thread is not waiting."<<std::endl;
        #endif
                }
                else
                {
                    promotion_thread_waiting = false;
        #if SIM_DEBUG
                    std::cout<<"The event from promotion thread is dequeued, now the promotion thread is not waiting."<<std::endl;
        #endif
                }
            }

    clock_mutex.lock();
    time_tick = next_time;
    clock_mutex.unlock();
    clock_cv.notify_all();
}