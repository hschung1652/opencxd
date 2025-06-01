#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <random>

#include "thread_utils.h"
#include "utils.h"
#include "not_ftl.h"
#include "simulator_clock.h"

using std::to_string;

/* a series of macro for debug : reduce printk number in pre-compilation level */
#define TEST_SSD_NODEBUG        0
#define TEST_SSD_RANDRW_VERIFY  1
#define TEST_SSD_DEBUG          TEST_SSD_RANDRW_VERIFY

#define TEST_SSD_RANDW_LOC_NUM  1280
#define TEST_SSD_RAND_ROUNDS    50

extern ssd gdev;
extern sim_clock* the_clock_pt;

extern bool promotion_enable;
extern bool tpp_enable;
extern bool write_log_enable;
extern bool use_macsim;
extern bool pinatrace_drive;

static int start_thread(pthread_t *thread_id, void *thread_args, void *(func)(void *), string thread_name) {
    int retval;
    retval = pthread_create(thread_id, nullptr, func, thread_args);
    if (retval != 0) {
        bytefs_err("Failed to create %s thread", thread_name.c_str());
        retval = -1;
    } else {
        pthread_setname(*thread_id, thread_name.c_str());
        //int cpu_idx = pthread_bind(*thread_id);
        bytefs_log("Creating %s thread", thread_name.c_str());
    }
    return retval;
}

static int cancel_thread(pthread_t *thread_id, string thread_name) {
    int retval = pthread_cancel(*thread_id);
    if (retval == 0) {
        bytefs_log("Successfully terminated %s thread", thread_name.c_str());
        return 0;
    }
    bytefs_log("Termination of %s thread failed with retcode %d", thread_name.c_str(), retval);
    return retval;
}

int opencxd_start_threads(void) {
    ssd *ssd = &gdev;
    int retval;

    if (promotion_enable || tpp_enable) {
        bytefs_log("Start promotion thread");
        bytefs_log("Initizing %ld promotion threads", ssd->n_promotion_threads);
        ssd->promotion_thread_id = new pthread_t[ssd->n_promotion_threads];
        for (uint64_t promotion_thread_idx = 0; promotion_thread_idx < ssd->n_promotion_threads; promotion_thread_idx++) {
            start_thread(&ssd->promotion_thread_id[promotion_thread_idx], nullptr, 
                                promotion_thread, ("promotion #" + to_string(promotion_thread_idx)).c_str());
        }
    }
    else
    {
        the_clock_pt->wait_without_events(ThreadType::Page_promotion_thread, 0);
    }
    
    return 0;
}

int opencxd_stop_threads(void) {
    ssd *ssd = &gdev;

    // kill the thread first
    bytefs_log("Stop promotion thread");

    if (promotion_enable || tpp_enable) {
        for (uint64_t promotion_thread_idx = 0; promotion_thread_idx < ssd->n_promotion_threads; promotion_thread_idx++)
            //bytefs_cancel_thread(&ssd->promotion_thread_id[promotion_thread_idx],
            //                    ("promotion #" + to_string(promotion_thread_idx)).c_str());
        delete[] ssd->promotion_thread_id;
    }

    return 0;
}

void opencxd_stop_threads_gracefully(void) {
    ssd *ssd = &gdev;
    ssd->terminate_flag = 1;
    
    if (promotion_enable || tpp_enable) {
        for (uint64_t i = 0; i < ssd->n_promotion_threads; i++)
            //pthread_join(ssd->promotion_thread_id[i], nullptr);
        bytefs_log("Promotion threads terminated");
    }
}
