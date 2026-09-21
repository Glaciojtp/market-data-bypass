# Comparative Latency Benchmark: POSIX UDP vs. AF_XDP Kernel Bypass

| Metric | POSIX Baseline (Tuned) | AF_XDP Zero-Copy | Latency Reduction |
| :--- | :--- | :--- | :--- |
| **Min** | `4.40 µs` | **`3.10 µs`** | **29.5%** |
| **p50 (Median)** | `16.50 µs` | `2498.10 µs` | **-15040.0%** |
| **Avg** | `28.74 µs` | `3169.04 µs` | **-10926.1%** |
| **p90** | `42.50 µs` | `7548.37 µs` | **-17660.9%** |
| **p99 (Tail)** | `119.64 µs` | `11375.78 µs` | **-9408.2%** |
| **p99.9** | `924.31 µs` | `11975.71 µs` | **-1195.6%** |
| **Max** | `1070.60 µs` | `12036.10 µs` | **-1024.2%** |

*Generated automatically by HFT Benchmark Runner (@Glaciojtp)*
