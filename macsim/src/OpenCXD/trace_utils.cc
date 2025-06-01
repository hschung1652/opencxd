#include <filesystem>
#include <cstring>
#include <vector>
#include <unordered_set>
#include <iostream>
#include <sys/stat.h>

#include "not_ftl.h"
#include "trace_utils.h"

using std::vector;
using std::unordered_set;
using std::string;
using std::uintmax_t;


off_t getFileSize(const std::string& filename) {
    struct stat stat_buf;
    if (stat(filename.c_str(), &stat_buf) != 0) {
        std::cerr << "Unable to get file size: " << filename << std::endl;
        return -1;
    }
    return stat_buf.st_size;
}


TraceReplayUnit::TraceReplayUnit() :
    sample_arr(nullptr), sample_idx(0), sample_max_idx(0), sample_buf_len(0),
    abs_sample_idx(0), num_samples(0) {
  resetTrackingInfo();
}

TraceReplayUnit::TraceReplayUnit(string filename) : TraceReplayUnit() {
  init(filename);
}

// 0 on fail, success otherwise
int TraceReplayUnit::init(string filename, uint64_t sample_buf_len) {
  if (ifs.is_open())
    return ifs.good() && sample_arr != nullptr;

  ifs.open(filename);
  if (!ifs.good())
    return 0;
  
  uint64_t size = (uint64_t)getFileSize(filename);
  
  assert(size % sizeof(trace_sample) == 0);

  resetTrackingInfo();
  num_samples = size / sizeof(trace_sample);
  this->sample_buf_len = sample_buf_len;
  sample_arr = new trace_sample[this->sample_buf_len];

  sample_idx = 0;
  return 1;
}

void TraceReplayUnit::deinit() {
  if (ifs.is_open()) {
    ifs.close();
    sample_buf_len = 0;
    delete[] sample_arr;
  }
  resetTrackingInfo();
}

trace_sample *TraceReplayUnit::getCurrentTraceSample() {
  if (sample_max_idx == 0 && ifs.eof())
    return nullptr;
  __getCurrentTraceSample();
  return &sample_arr[sample_idx];
}

trace_sample *TraceReplayUnit::getNextTraceSample() {
  if (sample_max_idx == 0 && ifs.eof())
    return nullptr;
  __advanceNextTraceSample();
  return &sample_arr[sample_idx];
}

int TraceReplayUnit::resetReadHead() {
  if (!ifs.is_open())
    return -1;
  ifs.clear();
  ifs.seekg(0);
  resetTrackingInfo();
  return 0;
}

void TraceReplayUnit::getStatus(replay_progress *progress) {
  progress->current_inst_idx = abs_sample_idx;
  progress->inst_num = num_samples;
}

inline void TraceReplayUnit::__advanceNextTraceSample() {
  sample_idx++;
  __getCurrentTraceSample();
  abs_sample_idx += sample_max_idx != 0;
}

inline void TraceReplayUnit::__getCurrentTraceSample() {
  if (sample_idx >= sample_max_idx) {
    sample_idx = 0;
    int nread = ifs.read((char *) sample_arr, sizeof(trace_sample) * sample_buf_len).gcount();
    assert(nread % sizeof(trace_sample) == 0);
    sample_max_idx = nread / sizeof(trace_sample);
  }
}

void TraceReplayUnit::resetTrackingInfo() {
  sample_idx = 0;
  sample_max_idx = 0;
  sample_buf_len = 0;
  abs_sample_idx = 0;
  num_samples = 0;
}

void *parallelLoaderThread(void *thread_args) {
  ParallelLoader::parallel_loader_args *args = static_cast<ParallelLoader::parallel_loader_args *>(thread_args);
  ParallelLoader::parallel_loader_returns *returns = new ParallelLoader::parallel_loader_returns;
  TraceReplayUnit *tru = args->tru;
  trace_sample *sample;

  volatile uint8_t *const load_autherization_flag = args->load_autherization_flag;
  volatile uint8_t *const worker_report_flag = args->worker_report_flag;
  volatile uint8_t *const worker_finish_flag = args->worker_finish_flag;

  unordered_set<uint64_t> pages;
  unordered_set<uint64_t>::iterator pages_iter;
  assert(tru->init(args->tracefile));
  sample = tru->getCurrentTraceSample();
  uint64_t trace_starttime = sample->ns;
  while ((sample = tru->getCurrentTraceSample()) != nullptr) {
    if (sample->r != 'R' && sample->r != 'W') {
      tru->getNextTraceSample();
      continue;
    }
    uint64_t addr = sample->addr;
    pages.insert(addr / PG_SIZE * PG_SIZE);
    if (*load_autherization_flag != 0) {
      pages_iter = pages.begin();
      while (pages_iter != pages.end() && *load_autherization_flag != 0) {
        bytefs_fill_data(*pages_iter);
        pages.erase(pages_iter);
        pages_iter = pages.begin();
      }
      *load_autherization_flag = 0;
      *worker_report_flag = 1;
    }
    tru->getNextTraceSample();
  }
  printf("Worker %d read file finish ==============\n", args->worker_id);
  *worker_finish_flag = 1;
  while (*load_autherization_flag != 0) {}
  *worker_report_flag = 2;
  printf("Worker %d exclusive load begin ==========\n", args->worker_id);
  pages_iter = pages.begin();
  while (pages_iter != pages.end() && *load_autherization_flag != 0) {
      bytefs_fill_data(*pages_iter);
      pages.erase(pages_iter);
      pages_iter = pages.begin();
  }
  assert(pages_iter == pages.end());
  *worker_report_flag = 1;
  while (*load_autherization_flag != 0);
  
  *worker_finish_flag = 2;
  printf("Worker %d terminate =====================\n", args->worker_id);
  tru->deinit();
  return (void *) returns;
}

ParallelLoader::ParallelLoader(vector<string> tracefiles) :
    nfiles(tracefiles.size()),
    trus(new TraceReplayUnit[nfiles]),
    load_autherization_flags(new uint8_t[nfiles]),
    worker_report_flags(new uint8_t[nfiles]),
    worker_finish_flags(new uint8_t[nfiles]),
    args(new parallel_loader_args[nfiles]),
    pthread_info(new pthread_t[nfiles]) {
  memset((void *) load_autherization_flags, 0, sizeof(load_autherization_flags));
  memset((void *) worker_report_flags, 0, sizeof(worker_report_flags));
  memset((void *) worker_finish_flags, 0, sizeof(worker_finish_flags));
  for (int fileidx = 0; fileidx < nfiles; fileidx++) {
    args[fileidx].loader = this;
    args[fileidx].tru = &trus[fileidx];
    args[fileidx].tracefile = tracefiles[fileidx];
    args[fileidx].load_autherization_flag = &load_autherization_flags[fileidx];
    args[fileidx].worker_report_flag = &worker_report_flags[fileidx];
    args[fileidx].worker_finish_flag = &worker_finish_flags[fileidx];
    args[fileidx].worker_id = fileidx;
    pthread_create(&pthread_info[fileidx], nullptr, parallelLoaderThread, (void *) &args[fileidx]);
  }
}

void ParallelLoader::loadIntoSystem() {
  uint64_t timeslice_end;
  uint64_t loader_interval = (uint64_t) 5e9;
  bool all_completed;
  replay_progress progress;
  do {
    all_completed = true;
    for (int file_idx = 0; file_idx < nfiles; file_idx++) {
      if (worker_finish_flags[file_idx] == 2)
        continue;
      all_completed = false;
      trus[file_idx].getStatus(&progress);
      printf("Parallel loader %d run\n", file_idx);
      printf("Parallel loader %d progress [%20ld/%20ld](%6.2f%%)\n",
          file_idx, progress.current_inst_idx, progress.inst_num,
          100.0 * progress.current_inst_idx / progress.inst_num);
      timeslice_end = get_time_ns() + loader_interval;  //TODO: replace
      worker_report_flags[file_idx] = 0;
      load_autherization_flags[file_idx] = 1;
      while (worker_report_flags[file_idx] == 2 ||
             (get_time_ns() < timeslice_end && worker_finish_flags[file_idx] == 0)) {  //TODO: replace
        sched_yield();
      }
      load_autherization_flags[file_idx] = 0;
      worker_report_flags[file_idx] = 0;
      printf("Parallel loader %d timeslice expire\n", file_idx);
    }
  } while (!all_completed);
}

uint64_t ParallelLoader::joinServiceThreads() {
  uint64_t start_timestamp = (uint64_t) -1;
  parallel_loader_returns *returns;
  for (int fileidx = 0; fileidx < nfiles; fileidx++) {
    pthread_join(pthread_info[fileidx], (void **) &returns);
    start_timestamp = std::min(start_timestamp, returns->trace_starttime);
  }
  return start_timestamp;
}

