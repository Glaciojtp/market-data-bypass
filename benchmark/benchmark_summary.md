# Comparative Latency Benchmark: POSIX UDP vs. AF_XDP Kernel Bypass

| Metric | POSIX Baseline (Tuned) | AF_XDP Zero-Copy | Latency Reduction |
| :--- | :--- | :--- | :--- |
| **Min** | `4.30 µs` | **`3.10 µs`** | **27.9%** |
| **p50 (Median)** | `16.15 µs` | `2316.75 µs` | **-14245.2%** |
| **Avg** | `24.07 µs` | `3004.64 µs` | **-12385.4%** |
| **p90** | `42.70 µs` | `7301.97 µs` | **-17000.6%** |
| **p99 (Tail)** | `62.30 µs` | `11071.13 µs` | **-17670.4%** |
| **p99.9** | `112.00 µs` | `11971.01 µs` | **-10588.2%** |
| **Max** | `160.50 µs` | `12084.00 µs` | **-7429.0%** |

*Generated automatically by HFT Benchmark Runner (@Glaciojtp)*
