# OpenCXD – OpenSSD Firmware

This repository contains the OpenSSD firmware implementation for OpenCXD, which reproduces the CXL-SSD internal software architecture presented in the state-of-the-art CXL-SSD system, [SkyByte](https://doi.ieeecomputersociety.org/10.1109/HPCA61900.2025.00051).  
The implementation covers **Write Log**, **Log Index**, and **Data Cache** subsystems, integrated into the OpenSSD platform.

---

## Setup Instructions

This repository does not provide basic OpenSSD setup instructions.  
For environment preparation, hardware setup, and OpenSSD build details, please refer to:  
[https://github.com/CRZ-Technology/OpenSSD-OpenChannelSSD/tree/main/DaisyPlus/DOC](https://github.com/CRZ-Technology/OpenSSD-OpenChannelSSD/tree/main/DaisyPlus/DOC)

---

## Key Implementation Details

This firmware reproduces the state-of-the-art CXL-SSD architecture by:
- **Write Log**: Logging the cacheline of each CXL.mem write request
- **Log Index**: Indexing logged writes for efficient lookup and compaction
- **Data Cache**: Retaining OpenSSD’s original NAND Page Cache implementation

**Important files to explore:**
- `memory_map.h`  
  Defines new allocation for Write Log and Log Index (Data Cache remains as in OpenSSD)
- `nvme/cxl_ssd.[ch]`  
  Handlers for CXL.mem memory requests pushdown from MacSim
- `writelog/writelog.[ch]`  
  Implements write logging for CXL.mem write requests
- `writelog/logindex.[ch]`, `writelog/compactbuffer.h`  
  Implements Log Index for Write Log
- `nvme/nvme_io_cmd.c`  
  Entry point for firmware logic execution, triggering relevant handlers

---

## Notes on Implementation

- **Log Index and Log Compaction** follow the SkyByte design principles  
  → Current version uses a naive hash conflict handling mechanism  
  → This will be improved in future releases
- For implementing new **CXL-SSD optimization logic**, start from `nvme/cxl_ssd.[ch]` and extend as needed

