#ifndef __UTILS_H__
#define __UTILS_H__

#include <sys/prctl.h>

#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <cerrno>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>

using std::ifstream;
using std::string;
using std::stoull;
using std::vector;

// current machine configuration
extern const int num_cores;
extern const uint64_t tsc_frequency_khz;
// default parameter
extern const uint64_t rr_timeslice_nano;
extern const uint64_t ctx_swh_deadtime_nano;
// print facility
extern const char *ansi_clearline;
extern const char *ansi_clearscreen;
extern const char *ansi_reset_cursor;
// pthread CPU affinity facility
extern int cur_cpu_idx;


struct param {
  string filename_prefix;
  vector<string> trace_filenames;
  vector<string> macsim_trace_filenames;
  int ntraces = 1;
  int spawn_first = 1;
  int logical_core_num = 1;
  int sim_thread_num = 1;
  double time_scale = 1;

  uint64_t rr_timeslice = rr_timeslice_nano;
  uint64_t ctx_swh_deadtime = ctx_swh_deadtime_nano;

  string status_tty_name;
  FILE *status_tty = stdout;
};


static int pthread_bind(pthread_t tid) {
  int cpu_idx = cur_cpu_idx++;

  if (cpu_idx < 0 || cpu_idx >= num_cores)
    return -EINVAL;

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_idx, &cpuset);

  if (pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cpuset) == 0)
    return cpu_idx;
  return -1;
}

static int pthread_setname(pthread_t tid, const char *name) {
  return pthread_setname_np(tid, name);
}

static uint64_t get_tsc_frequency() {
  string tsc_freq_khz_file = "/sys/devices/system/cpu/cpu0/tsc_freq_khz";
  // ifstream tsc_freq_file(tsc_freq_khz_file);
  // // if (!tsc_freq_file.good()) {
  // //   printf("Could not determine TSC frequency, make sure kernel module <tsc_freq_khz> is installed\n");
  // //   assert(false);
  // //   return -1; // Couldn't determine the CPU base frequency.
  // // }
  // string tsc_freq_khz_str;
  // tsc_freq_file >> tsc_freq_khz_str;
  // uint64_t tsc_frequency_khz = stoull(tsc_freq_khz_str);
  uint64_t tsc_frequency_khz = 3000000;
  printf("TSC frequency %ld kHz (%f GHz) detected\n", tsc_frequency_khz, tsc_frequency_khz / 1e6);
  return tsc_frequency_khz;
}

static inline uint64_t get_time_ns(void) {
  // this method will be valid until Jan 19, 586524
  return __builtin_ia32_rdtsc() * 1000000 / tsc_frequency_khz;
}

static void const_init() {
  cur_cpu_idx = 0;
}

static inline void ns_to_timespec(uint64_t ns, timespec &req) {
  req.tv_sec = (long) (ns / 1000000000UL);
  req.tv_nsec = (long) (ns % 1000000000UL);
}

static inline void sleepns(uint64_t ns) {
  timespec rem;
  timespec req = {
      (long) (ns / 1000000000UL), /* secs (Must be Non-Negative)                */ 
      (long) (ns % 1000000000UL)  /* nano (Must be in range of 0 to 999999999)  */ 
  };
  nanosleep(&req , &rem);
  return;
}

static inline void sleepns(timespec &req) {
  timespec rem;
  nanosleep(&req , &rem);
  return;
}

#endif
