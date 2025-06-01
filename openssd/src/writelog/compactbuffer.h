#ifndef COMPACTBUFFER_H
#define COMPACTBUFFER_H

#include "../memory_map.h"

static inline uint8_t *getCompactionEntry(int index) {
    if (index < 0 || index >= LOG_COMPACT_BUFFER_COUNT) {
        return NULL; 
    }
    return (uint8_t *)(LOG_COMPACT_BUFFER_START + (index * BYTES_PER_DATA_REGION_OF_SLICE));
}

#endif // COMPACTBUFFER_H
