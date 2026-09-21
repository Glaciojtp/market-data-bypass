#ifndef MARKET_DATA_H
#define MARKET_DATA_H

#include <stdint.h>
#include <time.h>

#define DEFAULT_MCAST_GRP "239.255.0.1"
#define DEFAULT_PORT 12345
#define FEED_MAGIC 0x48465431  // "HFT1"

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;           // Magic protocol identifier
    uint32_t seq_num;         // Monotonic sequence counter
    uint64_t send_ts_ns;      // Nanosecond timestamp at transmission (CLOCK_MONOTONIC_RAW)
    char     symbol[8];       // Ticker symbol (e.g. "NVDA    ")
    uint32_t price;           // Fixed-point price in cents (e.g. 12550 = $125.50)
    uint32_t qty;             // Order/execution quantity
    char     side;            // 'B' (Bid/Buy) or 'A' (Ask/Sell)
} market_data_msg_t;
#pragma pack(pop)

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#endif // MARKET_DATA_H
