#!/usr/bin/env python3
"""
HFT Benchmark Suite: POSIX UDP vs. AF_XDP Zero-Copy Kernel Bypass
Author: Glaciojtp (@Glaciojtp)

Automates:
1. Virtual Ethernet (veth) setup with multicast routing
2. Baseline tuned POSIX socket benchmark
3. AF_XDP kernel-bypass benchmark
4. Statistical tail-latency profiling (p50, p90, p99, p99.9, Max)
5. Generation of publication-grade CDF and Tail Latency charts
6. Clean interface teardown
"""

import os
import sys
import time
import argparse
import subprocess
import numpy as np
import pandas as pd

def run_cmd(cmd, check=True):
    return subprocess.run(cmd, shell=True, check=check, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

def setup_network():
    print("[Setup] Configurando par de interfaces veth y ruteo multicast...")
    run_cmd("ip link del veth_rx 2>/dev/null || true", check=False)
    run_cmd("ip link add veth_rx type veth peer name veth_tx")
    run_cmd("ip addr add 192.168.99.1/24 dev veth_rx")
    run_cmd("ip addr add 192.168.99.2/24 dev veth_tx")
    run_cmd("ip link set veth_rx up")
    run_cmd("ip link set veth_tx up")
    run_cmd("ip route add 224.0.0.0/4 dev veth_tx 2>/dev/null || true", check=False)

def teardown_network():
    print("[Teardown] Limpiando interfaces veth...")
    run_cmd("ip link del veth_rx 2>/dev/null || true", check=False)

def run_baseline_test(bin_dir, count, rate, warmup):
    csv_file = "benchmark/baseline_raw.csv"
    if os.path.exists(csv_file):
        os.remove(csv_file)

    rx_bin = os.path.join(bin_dir, "baseline_rx")
    tx_bin = os.path.join(bin_dir, "generator")

    print(f"\n>>> [1/2] Ejecutando Baseline POSIX Optimizado (Count: {count}, Rate: {rate} pkts/s, Warmup: {warmup})...")
    rx_proc = subprocess.Popen(
        [rx_bin, "-g", "239.255.0.1", "-p", "12345", "-c", str(count), "-w", str(warmup), "-u", "2", "-b", "50", "-o", csv_file],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    time.sleep(0.8)

    total_send = count + warmup
    tx_proc = subprocess.Popen(
        [tx_bin, "-g", "239.255.0.1", "-p", "12345", "-c", str(total_send), "-r", str(rate), "-i", "192.168.99.2"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )

    tx_proc.wait()
    rx_proc.wait()

    if not os.path.exists(csv_file):
        raise RuntimeError(f"El archivo {csv_file} no fue generado.")

    df = pd.read_csv(csv_file)
    return df["latency_ns"].values

def run_bypass_test(bin_dir, count, rate):
    csv_file = "latency_bypass.csv"
    if os.path.exists(csv_file):
        os.remove(csv_file)

    rx_bin = os.path.join(bin_dir, "bypass_rx")
    tx_bin = os.path.join(bin_dir, "generator")

    print(f"\n>>> [2/2] Ejecutando AF_XDP Zero-Copy Bypass (Count: {count}, Rate: {rate} pkts/s)...")
    rx_proc = subprocess.Popen(
        [rx_bin, "-i", "veth_rx", "-c", str(count), "-o", csv_file],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    time.sleep(1.0)

    tx_proc = subprocess.Popen(
        [tx_bin, "-g", "239.255.0.1", "-p", "12345", "-c", str(count), "-r", str(rate), "-i", "192.168.99.2"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )

    tx_proc.wait()
    time.sleep(1.0)
    rx_proc.terminate()
    try:
        rx_proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        rx_proc.kill()

    target_csv = "benchmark/bypass_raw.csv"
    if os.path.exists(csv_file):
        os.rename(csv_file, target_csv)
    elif not os.path.exists(target_csv):
        raise RuntimeError(f"El archivo {csv_file} de bypass no fue generado.")

    df = pd.read_csv(target_csv)
    return df["latency_ns"].values

def compute_percentiles(lat_ns):
    lat_us = lat_ns / 1000.0
    return {
        "min": np.min(lat_us),
        "p50": np.percentile(lat_us, 50),
        "avg": np.mean(lat_us),
        "p90": np.percentile(lat_us, 90),
        "p99": np.percentile(lat_us, 99),
        "p99.9": np.percentile(lat_us, 99.9),
        "max": np.max(lat_us),
    }

def print_and_save_summary(stats_base, stats_bypass):
    report = f"""
========================================================================================
             HFT MARKET DATA BENCHMARK REPORT: POSIX vs. AF_XDP ZERO-COPY
========================================================================================
 Metric                POSIX Baseline (Tuned)      AF_XDP Kernel Bypass      Reduction
----------------------------------------------------------------------------------------
 Min Latency           {stats_base['min']:8.2f} us                {stats_bypass['min']:8.2f} us            {(1 - stats_bypass['min']/stats_base['min'])*100:6.1f}%
 p50 (Mediana)         {stats_base['p50']:8.2f} us                {stats_bypass['p50']:8.2f} us            {(1 - stats_bypass['p50']/stats_base['p50'])*100:6.1f}%
 Promedio (Avg)        {stats_base['avg']:8.2f} us                {stats_bypass['avg']:8.2f} us            {(1 - stats_bypass['avg']/stats_base['avg'])*100:6.1f}%
 p90                   {stats_base['p90']:8.2f} us                {stats_bypass['p90']:8.2f} us            {(1 - stats_bypass['p90']/stats_base['p90'])*100:6.1f}%
 p99 (Tail Latency)    {stats_base['p99']:8.2f} us                {stats_bypass['p99']:8.2f} us            {(1 - stats_bypass['p99']/stats_base['p99'])*100:6.1f}%
 p99.9                 {stats_base['p99.9']:8.2f} us                {stats_bypass['p99.9']:8.2f} us            {(1 - stats_bypass['p99.9']/stats_base['p99.9'])*100:6.1f}%
 Max (Peor Caso)       {stats_base['max']:8.2f} us                {stats_bypass['max']:8.2f} us            {(1 - stats_bypass['max']/stats_base['max'])*100:6.1f}%
========================================================================================
"""
    print(report)

    md_report = f"""# Comparative Latency Benchmark: POSIX UDP vs. AF_XDP Kernel Bypass

| Metric | POSIX Baseline (Tuned) | AF_XDP Zero-Copy | Latency Reduction |
| :--- | :--- | :--- | :--- |
| **Min** | `{stats_base['min']:.2f} µs` | **`{stats_bypass['min']:.2f} µs`** | **{(1 - stats_bypass['min']/stats_base['min'])*100:.1f}%** |
| **p50 (Median)** | `{stats_base['p50']:.2f} µs` | `{stats_bypass['p50']:.2f} µs` | **{(1 - stats_bypass['p50']/stats_base['p50'])*100:.1f}%** |
| **Avg** | `{stats_base['avg']:.2f} µs` | `{stats_bypass['avg']:.2f} µs` | **{(1 - stats_bypass['avg']/stats_base['avg'])*100:.1f}%** |
| **p90** | `{stats_base['p90']:.2f} µs` | `{stats_bypass['p90']:.2f} µs` | **{(1 - stats_bypass['p90']/stats_base['p90'])*100:.1f}%** |
| **p99 (Tail)** | `{stats_base['p99']:.2f} µs` | `{stats_bypass['p99']:.2f} µs` | **{(1 - stats_bypass['p99']/stats_base['p99'])*100:.1f}%** |
| **p99.9** | `{stats_base['p99.9']:.2f} µs` | `{stats_bypass['p99.9']:.2f} µs` | **{(1 - stats_bypass['p99.9']/stats_base['p99.9'])*100:.1f}%** |
| **Max** | `{stats_base['max']:.2f} µs` | `{stats_bypass['max']:.2f} µs` | **{(1 - stats_bypass['max']/stats_base['max'])*100:.1f}%** |

*Generated automatically by HFT Benchmark Runner (@Glaciojtp)*
"""
    with open("benchmark/benchmark_summary.md", "w") as f:
        f.write(md_report)

def generate_plots(base_lat_ns, bypass_lat_ns, output_img):
    import matplotlib.pyplot as plt

    base_us = base_lat_ns / 1000.0
    bypass_us = bypass_lat_ns / 1000.0

    plt.style.use("seaborn-v0_8-darkgrid" if "seaborn-v0_8-darkgrid" in plt.style.available else "default")
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6), dpi=150)

    # 1. Cumulative Distribution Function (CDF)
    base_sorted = np.sort(base_us)
    bypass_sorted = np.sort(bypass_us)
    p_base = np.linspace(0, 1, len(base_sorted))
    p_bypass = np.linspace(0, 1, len(bypass_sorted))

    ax1.plot(base_sorted, p_base * 100, label="POSIX Baseline (Tuned)", color="#e74c3c", linewidth=2)
    ax1.plot(bypass_sorted, p_bypass * 100, label="AF_XDP Kernel Bypass (Zero-Copy)", color="#2ecc71", linewidth=2)
    ax1.set_title("Cumulative Distribution Function (CDF) - Latency Profile", fontsize=12, fontweight="bold")
    ax1.set_xlabel("Latency (Microseconds)", fontsize=11)
    ax1.set_ylabel("Percentile (%)", fontsize=11)
    ax1.axhline(99, color="gray", linestyle="--", alpha=0.7, label="p99 Reference")
    ax1.axhline(99.9, color="black", linestyle=":", alpha=0.7, label="p99.9 Reference")
    ax1.legend(loc="lower right", frameon=True)
    ax1.set_xscale("log")
    ax1.grid(True, which="both", ls="-", alpha=0.5)

    # 2. Tail Latency Comparison (Bar Chart)
    metrics = ["p50", "p90", "p99", "p99.9", "Max"]
    b_vals = [np.percentile(base_us, 50), np.percentile(base_us, 90), np.percentile(base_us, 99), np.percentile(base_us, 99.9), np.max(base_us)]
    x_vals = [np.percentile(bypass_us, 50), np.percentile(bypass_us, 90), np.percentile(bypass_us, 99), np.percentile(bypass_us, 99.9), np.max(bypass_us)]

    x = np.arange(len(metrics))
    width = 0.35

    ax2.bar(x - width/2, b_vals, width, label="POSIX Baseline", color="#e74c3c", alpha=0.85)
    ax2.bar(x + width/2, x_vals, width, label="AF_XDP Bypass", color="#2ecc71", alpha=0.85)
    ax2.set_title("Tail Latency by Percentile", fontsize=12, fontweight="bold")
    ax2.set_xticks(x)
    ax2.set_xticklabels(metrics, fontweight="bold")
    ax2.set_ylabel("Latency (Microseconds)", fontsize=11)
    ax2.set_yscale("log")
    ax2.legend(loc="upper left", frameon=True)
    ax2.grid(True, which="both", ls="-", alpha=0.5)

    plt.suptitle("HFT Market Data Feed Latency: POSIX Kernel Stack vs. AF_XDP Zero-Copy Bypass\nAuthor: Glaciojtp (@Glaciojtp)", fontsize=13, fontweight="bold", y=0.98)
    plt.tight_layout()
    plt.savefig(output_img, bbox_inches="tight")
    print(f"\n[Visuals] Gráfico de alta resolución guardado en: {output_img}")

def main():
    parser = argparse.ArgumentParser(description="HFT Market Data Latency Benchmark Suite")
    parser.add_argument("-c", "--count", type=int, default=10000, help="Cantidad de paquetes por prueba (default: 10000)")
    parser.add_argument("-r", "--rate", type=int, default=50000, help="Tasa de paquetes/segundo (default: 50000)")
    parser.add_argument("-w", "--warmup", type=int, default=2000, help="Paquetes de warmup (default: 2000)")
    parser.add_argument("-o", "--output", default="benchmark/latency_benchmark.png", help="Ruta de imagen de salida")
    args = parser.parse_args()

    bin_dir = "bin"
    if not os.path.exists(os.path.join(bin_dir, "generator")):
        print("Error: Binarios no encontrados en bin/. Ejecuta 'make all' primero.")
        sys.exit(1)

    try:
        setup_network()
        base_lat_ns = run_baseline_test(bin_dir, args.count, args.rate, args.warmup)
        bypass_lat_ns = run_bypass_test(bin_dir, args.count, args.rate)

        stats_base = compute_percentiles(base_lat_ns)
        stats_bypass = compute_percentiles(bypass_lat_ns)

        print_and_save_summary(stats_base, stats_bypass)
        generate_plots(base_lat_ns, bypass_lat_ns, args.output)

    finally:
        teardown_network()

if __name__ == "__main__":
    main()
