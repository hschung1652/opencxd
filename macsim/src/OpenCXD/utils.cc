#include "utils.h"

#define HZ 1000

const int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
const uint64_t tsc_frequency_khz = get_tsc_frequency();
/**
 * default timeslice is 100 msecs (used only for SCHED_RR tasks).
 * Timeslices get refilled after they expire.
 * Unit: microseconds
 */
const uint64_t rr_timeslice = 100 * HZ / 1000;
const uint64_t rr_timeslice_nano = rr_timeslice * 1000;

const uint64_t ctx_swh_deadtime_nano = 1000;

const string ansi_clearline_str = "\033[2K\r";
const char *ansi_clearline = ansi_clearline_str.c_str();

const string ansi_clearscreen_str = "\033[2J\033[H";
const char *ansi_clearscreen = ansi_clearscreen_str.c_str();

const string ansi_reset_cursor_str = "\033[H";
const char *ansi_reset_cursor = ansi_reset_cursor_str.c_str();

int cur_cpu_idx = 0;
