We use three types of config files: workload, baseline and additional settings.
We list and describe the knobs that can be used in different config files to customize experiments:


### Workload Config Files

1. **trace_locaton**: The location of memory warmup traces (the `pinatrace.out.X` files)
2. **num_files**: Number of memory warmup trace files.
3. **num_initial_threads**: Number of ready threads at the beginning of the programs. (Some program `fork` or `join` threads during execution)
4. **scale_factors**: The ratio of `tracing_time` and `actual_execution time_needed` for the workload. (for the memory warmup trace)
5. **num_mark**: NOT IMPORTANT (deprecated)



### Baseline Config Files

1. **promotion_enable**: Whether enabling the adaptive page migration mechanism or not.
2. **write_log_enable**: Whether enabling the CXL-Aware SSD DRAM management or not.
3. **device_triggered_ctx_swt**: Whether enabling the coordinated context switch mechanism or not.
4. **cs_threshold**: The threshold used for the context switch trigger policy. (Unit: ns)
5. **ssd_cache_size_byte**: The size of the SSD DRAM cache. (Unit: Byte)
6. **ssd_cache_way**: The associativity of the SSD DRAM cache.
7. **host_dram_size_byte**: The size of the host main memory. (Unit: Byte)
8. **t_policy**: The thread scheduling policy. (Choose from "RR", "RANDOM" and "FAIRNESS" (CFS))


### Additional Setting Config Files

Note: the settings speficied in this file will overwrite the settings in the previous two files.

1. **cs_threshold**: The threshold used for the context switch trigger policy. (Unit: ns)
2. **host_dram_size_byte**: The size of the host main memory. (Unit: Byte)
3. **ssd_host_rate**: The value of `SSD DRAM size` (write log + cache) devided by `Host DRAM size`.
4. **write_log_ratio**: The value of `write log size`/`SSD DRAM size`.