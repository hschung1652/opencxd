#ifndef __TRACE_UTILS__
#define __TRACE_UTILS__

#include <vector>
#include <string>
#include <fstream>
#include <cstdint>

#include <unistd.h>

using std::string;
using std::vector;
using std::ifstream;
using std::streampos;

class Thread;
class CPULogicalCore;
class ThreadScheduler;

struct trace_sample {
  uint64_t ns;
  uint64_t addr;
  uint8_t  r;
  uint8_t  size;
} __attribute__((packed));

struct replay_progress {
    uint64_t current_inst_idx;
    uint64_t inst_num;
};

struct logical_core_status {
    Thread *current_thread;
    Thread *scheduled_thread;
};

class TraceReplayUnit {
  public:
    TraceReplayUnit();
    TraceReplayUnit(string tracefile);
    int init(string tracefile, uint64_t sample_buf_len = 4096 / sizeof(trace_sample));
    void deinit();
    trace_sample *getCurrentTraceSample();
    trace_sample *getNextTraceSample();
    int resetReadHead();

    void getStatus(replay_progress *progress);

  private:
    inline void __advanceNextTraceSample();
    inline void __getCurrentTraceSample();
    void resetTrackingInfo();

    string tracefile;
    ifstream ifs;

    trace_sample *sample_arr;
    uint64_t sample_idx;
    uint64_t sample_max_idx;
    uint64_t sample_buf_len;

    uint64_t abs_sample_idx;
    uint64_t num_samples;
};

void *parallelLoaderThread(void *thread_args);
class ParallelLoader {
  public:
    struct parallel_loader_args {
      ParallelLoader *loader;
      TraceReplayUnit *tru;
      string tracefile;
      volatile uint8_t *load_autherization_flag;
      volatile uint8_t *worker_report_flag;
      volatile uint8_t *worker_finish_flag;
      int worker_id;
    };

    struct parallel_loader_returns {
      uint64_t trace_starttime;
    };

    ParallelLoader(vector<string> tracefiles);
    void loadIntoSystem();
    uint64_t joinServiceThreads();

    friend void *parallelLoaderThread(void *thread_args);

  private:
    uint64_t nfiles;
    TraceReplayUnit *trus;
    volatile uint8_t *const load_autherization_flags;
    volatile uint8_t *const worker_report_flags;
    volatile uint8_t *const worker_finish_flags;
    parallel_loader_args *args;
    pthread_t *pthread_info;
};

#endif
