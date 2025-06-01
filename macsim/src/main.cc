/*
Copyright (c) <2012>, <Georgia Institute of Technology> All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted
provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list of conditions
and the following disclaimer.

Redistributions in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or other materials provided
with the distribution.

Neither the name of the <Georgia Institue of Technology> nor the names of its contributors
may be used to endorse or promote products derived from this software without specific prior
written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

/**********************************************************************************************
 * File         : main.cc
 * Author       : HPArch
 * Date         : 3/25/2011
 * SVN          : $Id: main.cc 911 2009-11-20 19:08:10Z kacear $:
 * Description  : main file
 *********************************************************************************************/

#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <set>
#include <algorithm>

#include <assert.h>
#include <sched.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "OpenCXD/trace_utils.h"
#include "OpenCXD/utils.h"
#include "OpenCXD/thread_utils.h"
#include "OpenCXD/not_ftl.h"
#include "OpenCXD/simulator_clock.h"
#include "OpenCXD/cpu_scheduler.h"

#include "macsim.h"
#include "assert_macros.h"
#include "knob.h"
#include "core.h"
#include "utils.h"
#include "statistics.h"
#include "frontend.h"
#include "process_manager.h"
#include "pref_common.h"
#include "trace_read.h"

#include "debug_macros.h"

#include "all_knobs.h"

using std::string;
using std::ifstream;
using std::ofstream;
using std::stringstream;
using std::getline;
using std::abs;
using std::min;
using std::max;
using std::stoi;
using std::stod;
using std::to_string;
using std::unordered_set;

//Parameters:
extern bool promotion_enable;
extern bool write_log_enable;
extern bool device_triggered_ctx_swt;
extern long cs_threshold;

extern long ssd_cache_size_byte;
extern int ssd_cache_way;
extern long host_dram_size_byte;

string baseline_config_filename;
string workload_config_filename;
string setting_config_filename;
string workload_name;

double ssd_host_rate = 0;
extern double write_log_ratio;

string btype;
string bench;

FILE *output_file;
std::string main_filename;

extern bool print_timing_model;

extern const uint64_t rr_timeslice_nano;
extern const uint64_t ctx_swh_deadtime_nano;

extern Thread_Policy_enum t_policy;

bool use_macsim = true;
bool warmed_up_mode = false;
bool pinatrace_drive = false;
extern sim_clock* the_clock_pt;
unordered_set<uint64_t> prefill_pages;
bool prefill_pass = false;
ThreadScheduler* skybyte_scheduler_pt;
bool dram_only = false;
bool dram_baseline = false;
std::vector<uint64_t> ordered_vector;
uint64_t mark_inst_num = 0;

int64_t cache_marked_num = 0;
int64_t host_marked_num = 0; 
bool need_mark = false;

//TPP implementation only
extern vector<uint64_t> ordered_memory_space;
extern bool tpp_enable;
extern std::deque<uint64_t> LRU_active_list;
extern std::set<uint64_t> LRU_inactive_list;
extern std::set<uint64_t> NUMA_scan_set;

std::string extractFilename(const std::string& path) {
  // Find the position of the last directory separator
  size_t lastSlash = path.find_last_of('/');
  
  // Extract the substring starting from the position after the last slash
  if (lastSlash != std::string::npos && lastSlash < path.length() - 1) {
      return path.substr(lastSlash + 1);
  }
  
  // Return the original path if no directory separator is found
  return path;
}

int parameter_validation(param *param) {
  int ret = 0;
  bytefs_assert(param->filename_prefix.size() > 0);
  bytefs_assert(param->ntraces > 0);
  bytefs_assert(param->spawn_first > 0 && param->spawn_first <= param->ntraces);
  for (int file_idx = 0; file_idx < param->ntraces; file_idx++) {
    string cur_filename = param->filename_prefix + to_string(file_idx);
    param->trace_filenames.push_back(cur_filename);
    printf("Filename: %s\n", cur_filename.c_str());
  }
  bytefs_assert(param->trace_filenames.size() > 0);
  bytefs_assert(param->logical_core_num > 0);
  bytefs_assert(param->time_scale > 0);

  param->status_tty = stdout;
  return ret;
}

param param;

int main(int argc, char** argv) {

  const_init();
  std::string t_policy_name;
  t_policy = Thread_Policy_enum::RR;

  int opt;
  while ((opt = getopt(argc, argv, "rhpdw:c:o:b:f:s:t:")) != -1) {
    switch (opt) {
      case 'w': {
        workload_config_filename = optarg;
        break;
      }
      case 'c': {
        param.logical_core_num = stoi(optarg);
        break;
      }
      case 'b': {
        baseline_config_filename = optarg;
        break;
      }
      case 't': {
        setting_config_filename = optarg;
      }
      case 'p': {
        print_timing_model = true;
        break;
      }
      case 'f': {
        bench = optarg;
        break;
      }

      case '?':
      case 'h':
      default: {
        printf("Help\n");
        printf("  -h  print this, the help message\n");
        printf("  -w  workload config file\n");
        printf("  -b  baseline config file\n");
        printf("  -c  number of cores\n");
        printf("  -o  output status redirect tty\n");
        printf("  -f  output file name\n");
        printf("  -p  print timing model\n");
        return -1;
      }
    }
  }

  std::ifstream bconfig_file(baseline_config_filename);
    if (!bconfig_file.good()) {
        printf("Config file <%s> does not exist\n", baseline_config_filename.c_str());
        assert(false);
    }
    // parse config file
    std::string line; 
    std::string command;
    std::string value;
    printf("\nConfigs:\n");
    while (std::getline(bconfig_file, line)) {
        std::stringstream ss(line);
        command.clear();
        value.clear();
        ss >> command >> value;
        if (command != "#" && command != "")
            printf("  %25s: <%s>\n", command.c_str(), value.c_str());

        // baseline settings
        if (command == "promotion_enable")              { promotion_enable = std::stoi(value) != 0; }
        else if (command == "write_log_enable")         { write_log_enable = std::stoi(value) != 0; }
        else if (command == "tpp_enable")               { tpp_enable = std::stoi(value) != 0; }
        else if (command == "device_triggered_ctx_swt") { device_triggered_ctx_swt = std::stoi(value) != 0; }
        else if (command == "cs_threshold")             { cs_threshold = std::stoul(value); }
        else if (command == "t_policy")                 { t_policy_name = value; }
        // size settings
        else if (command == "host_dram_size_byte")      { host_dram_size_byte = std::stoul(value); }
        // comments or empty line
        else if (command == "#" || command == "")       {}
        else {
          printf("Error: Invalid config entry <%s>, aborting...\n", command.c_str());
          assert(false);
        }
    }
  
  if (t_policy_name == "RR") {
    t_policy = Thread_Policy_enum::RR;
  } else if (t_policy_name == "RANDOM") {
    t_policy = Thread_Policy_enum::RANDOM;
  } else if (t_policy_name == "LOCALITY"){
    t_policy = Thread_Policy_enum::LOCALITY;
  } else if (t_policy_name == "FAIRNESS"){
    t_policy = Thread_Policy_enum::FAIRNESS;
  }

  std::ifstream wconfig_file(workload_config_filename);
    if (!wconfig_file.good()) {
        printf("Config file <%s> does not exist\n", workload_config_filename.c_str());
        assert(false);
    }
    // parse config file
    printf("\nConfigs:\n");
    while (std::getline(wconfig_file, line)) {
        std::stringstream ss(line);
        command.clear();
        value.clear();
        ss >> command >> value;
        if (command != "#" && command != "")
            printf("  %25s: <%s>\n", command.c_str(), value.c_str());

        // workload settings
        if (command == "trace_location")              { param.filename_prefix = value; }
        else if (command == "num_files")         { param.ntraces = std::stoi(value); }
        else if (command == "num_initial_threads") { param.spawn_first = std::stoi(value); }
        else if (command == "num_mark")             { mark_inst_num = std::stoul(value); }
        else if (command == "num_sim_threads")       { param.sim_thread_num = std::stoi(value); }
        else if (command == "scale_factor")             { param.time_scale = std::stod(value); }
        // comments or empty line
        else if (command == "#" || command == "")       {}
        else {
          printf("Error: Invalid config entry <%s>, aborting...\n", command.c_str());
          assert(false);
        }
    }

  parameter_validation(&param);

  ordered_memory_space.clear();
  NUMA_scan_set.clear();
  LRU_active_list.clear();
  LRU_inactive_list.clear();
  
  init("/dev/nvme0n1", 0x3991c00);
  
  std::string bench_wmp = warmed_up_mode ? ("../output/warmup_traces/" + bench) : ("../output/" + bench);
  bench = "../output/" + bench;
  main_filename = bench;

  output_file = fopen(main_filename.c_str(), "w");
  

  FILE* prefill_and_warmup_data_file;
  FILE* prefill_data_file_read;

  if (!write_log_enable)
  {
    write_log_ratio = 0;
  }

  need_mark = false;


  uint64_t start_timestamp = (uint64_t) -1;

  workload_name = extractFilename(workload_config_filename);
  size_t last_dot = workload_name.find_last_of('.');
    
  // Extract the substring starting from the position after the last slash
  if (last_dot != std::string::npos && last_dot < workload_name.length() - 1) {
      workload_name = workload_name.substr(0, last_dot);
  }

  std::string warmup_trace_workload_name = "../output/warmup_traces/" + workload_name;
  workload_name = "../output/" + workload_name;

  string workload_dram_prefill = warmup_trace_workload_name + "_prefill_data.txt";
  std::cout << "warmup_trace_dram_prefill: " << workload_dram_prefill << std::endl; 

  //SSD Data prefill - trace read
  prefill_data_file_read = fopen((workload_dram_prefill).c_str(), "r");
  if (prefill_data_file_read != NULL)
  {
    if (dram_only)
    {
      return 0;
    }
    
    uint64_t page;
    uint64_t prefill_size = 0;
    assert(fscanf(prefill_data_file_read, "%ld", &prefill_size));
    for (size_t i = 0; i < prefill_size; i++)
    {
      assert(fscanf(prefill_data_file_read, "%ld", &page));
      prefill_pages.insert(page);
    }
    fclose(prefill_data_file_read);
  }

  // Define a vector to store elements in order
  ordered_vector.clear();

  // Reserve space in the vector to avoid reallocations
  ordered_vector.reserve(prefill_pages.size());

  // Traverse the unordered set and insert elements into the ordered vector
  for (const auto& element : prefill_pages) {
      ordered_vector.push_back(element);
  }

  // Sort the vector to maintain the order
  std::sort(ordered_vector.begin(), ordered_vector.end());

  std::cout << ordered_vector.front() << std::endl;
  std::cout << ordered_vector.back() << std::endl;

  size_t total_footprint_kb = ordered_vector.size() * 4096 / 1024;
  fprintf(output_file, "Total Data Footprint: %ld kB (%.3f GB)\n",
      total_footprint_kb, total_footprint_kb / 1024.0 / 1024.0);
  
  std::cout << "Start SSD Prefill" << std::endl;
  //prefill SSD, warmup write log
  for (auto page : ordered_vector) {
    bytefs_fill_data(page);
  }
  
  std::cout << "Start Host DRAM warmump" << std::endl;
  //warmup data cache
  FILE* warmup_hint_data_file = fopen((bench_wmp + "_warmup_hint_dram_system.txt").c_str(),"r");
  std::cout << "bench_wmp: " << (bench_wmp + "_warmup_hint_dram_system.txt").c_str() << std::endl; 
  replay_dram_system(warmup_hint_data_file);
  if (tpp_enable)
  {
    FILE* warmup_hint_data_file_tpp = fopen((bench_wmp + "_warmup_hint_tpp_system.txt").c_str(),"r");
    replay_tpp_system(warmup_hint_data_file_tpp);
    fclose(warmup_hint_data_file_tpp);
    std::sort(ordered_memory_space.begin(), ordered_memory_space.end());
  }

  /*warmup write log - useless (?)
  FILE* warmup_hint_data_file_2 = fopen((bench_wmp + "_warmup_hint_data_wlog.txt").c_str(),"r");
  uint64_t read_pgnum = 0;
  uint64_t write_pgnum = 0;

  assert(fscanf(warmup_hint_data_file_2, "%ld", &read_pgnum));
  assert(fscanf(warmup_hint_data_file_2, "%ld", &write_pgnum));
  warmup_write_log(read_pgnum, write_pgnum);
  */


  prefill_pages.clear();
  ordered_vector.clear();

  sim_clock the_clock(0, 1);
  the_clock_pt = &the_clock;
  
  start_timestamp = (uint64_t) -1;

  std::cout << "Start Actual Simulation!" << std::endl;
  vector<logical_core_returns> thread_returns_list_1;
  ThreadScheduler scheduler(param.trace_filenames, param.logical_core_num, start_timestamp,
                            rr_timeslice_nano, ctx_swh_deadtime_nano,
                            1.0 / param.time_scale, param.spawn_first);
  scheduler.spawnServiceThread();
  skybyte_scheduler_pt = &scheduler;
  sleep(1);

  scheduler.startExecution();

  macsim_c* sim;

  // Instantiate
  sim = new macsim_c();

  // Initialize Simulation State
  sim->initialize(argc, argv);

  // Run simulation
  // report("run core (single threads)");
  while (sim->run_a_cycle())
    ;
  
  sim->stat_stalls(output_file);
  // Finialize Simulation State
  sim->finalize();

  scheduler.getThreadsProgresses(1);
  fprintf(param.status_tty, "%sDone\n", ansi_clearline);

  scheduler.joinAll(thread_returns_list_1);
  the_clock_pt->force_finish();
  opencxd_stop_threads_gracefully();
  print_stats();

  return 0;
}
