#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <sys/mman.h>
#include <poll.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <xdp/xsk.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "../common/market_data.h"

#define NUM_FRAMES         4096
#define FRAME_SIZE         XSK_UMEM__DEFAULT_FRAME_SIZE // 4096 bytes
#define BATCH_SIZE         64

static volatile int running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

static int compare_uint64(const void *a, const void *b) {
    uint64_t arg1 = *(const uint64_t *)a;
    uint64_t arg2 = *(const uint64_t *)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

static int pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) < 0) {
        perror("sched_setaffinity");
        return -1;
    }
    printf("[HFT Optimization] Hilo AF_XDP fijado al CPU Core %d\n", core_id);
    return 0;
}

struct xsk_umem_info {
    struct xsk_ring_prod fq;
    struct xsk_ring_cons cq;
    struct xsk_umem *umem;
    void *buffer;
};

struct xsk_socket_info {
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;
    struct xsk_umem_info *umem;
    struct xsk_socket *xsk;
    uint64_t umem_frame_addr[NUM_FRAMES];
    uint32_t outstanding_tx;
};

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *ifname = "lo";
    int queue_id = 0;
    size_t target_count = 10000;
    int core_id = 3; // Core dedicado para bypass
    const char *csv_out = "latency_bypass.csv";

    int opt;
    while ((opt = getopt(argc, argv, "i:c:o:u:q:h")) != -1) {
        switch (opt) {
            case 'i': ifname = optarg; break;
            case 'c': target_count = (size_t)strtoul(optarg, NULL, 10); break;
            case 'o': csv_out = optarg; break;
            case 'u': core_id = atoi(optarg); break;
            case 'q': queue_id = atoi(optarg); break;
            case 'h':
                printf("Uso: %s [-i iface] [-c count] [-o csv_out] [-u core_id]\n", argv[0]);
                return 0;
            default: break;
        }
    }
    if (optind < argc) {
        ifname = argv[optind];
    }

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    if (core_id >= 0) {
        pin_to_core(core_id);
    }
    mlockall(MCL_CURRENT | MCL_FUTURE);

    printf("====================================================\n");
    printf(" HFT Kernel Bypass AF_XDP Zero-Copy Receiver\n");
    printf(" Autor: Glaciojtp (@Glaciojtp)\n");
    printf(" Interfaz: %s | Cola: %d | Muestras: %zu\n", ifname, queue_id, target_count);
    printf("====================================================\n");

    // 1. Reservar memoria contigua alineada para la UMEM
    void *bufs = NULL;
    size_t total_size = NUM_FRAMES * FRAME_SIZE;
    if (posix_memalign(&bufs, getpagesize(), total_size)) {
        perror("Error en posix_memalign para UMEM");
        return 1;
    }

    struct xsk_umem_info *umem_info = calloc(1, sizeof(*umem_info));
    umem_info->buffer = bufs;

    // 2. Crear UMEM (Fill Ring + Completion Ring)
    struct xsk_umem_config ucfg = {
        .fill_size = NUM_FRAMES,
        .comp_size = NUM_FRAMES,
        .frame_size = FRAME_SIZE,
        .frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM,
        .flags = 0
    };
    if (xsk_umem__create(&umem_info->umem, bufs, total_size,
                         &umem_info->fq, &umem_info->cq, &ucfg)) {
        perror("xsk_umem__create fallo (requiere privilegios root / CAP_NET_ADMIN)");
        return 1;
    }

    // Poblar el Fill Ring inicial con direcciones de buffers vacios
    uint32_t idx_fq = 0;
    xsk_ring_prod__reserve(&umem_info->fq, NUM_FRAMES, &idx_fq);
    for (int i = 0; i < NUM_FRAMES; i++) {
        *xsk_ring_prod__fill_addr(&umem_info->fq, idx_fq++) = i * FRAME_SIZE;
    }
    xsk_ring_prod__submit(&umem_info->fq, NUM_FRAMES);

    // 3. Crear el socket AF_XDP
    struct xsk_socket_info *xsk_info = calloc(1, sizeof(*xsk_info));
    xsk_info->umem = umem_info;

    struct xsk_socket_config xcfg = {
        .rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS,
        .tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS,
        .libbpf_flags = 0,
        .xdp_flags = XDP_FLAGS_SKB_MODE, // SKB mode para emulacion/veth/lo; DRV mode para NICs fisicas
        .bind_flags = 0
    };

    int ifindex = if_nametoindex(ifname);
    if (!ifindex) {
        perror("if_nametoindex");
        return 1;
    }

    if (xsk_socket__create(&xsk_info->xsk, ifname, queue_id,
                           umem_info->umem, &xsk_info->rx, &xsk_info->tx, &xcfg)) {
        perror("xsk_socket__create fallo");
        return 1;
    }

    printf("[AF_XDP] Socket vinculado a %s (ifindex %d). Esperando paquetes...\n", ifname, ifindex);

    uint64_t *latencies = malloc(target_count * sizeof(uint64_t));
    size_t received = 0;

    // 4. Bucle de Polling Zero-Copy (sin syscalls)
    while (running && received < target_count) {
        uint32_t idx_rx;
        uint32_t rcvd = xsk_ring_cons__peek(&xsk_info->rx, BATCH_SIZE, &idx_rx);
        if (!rcvd) {
            #if defined(__x86_64__)
            __builtin_ia32_pause();
            #endif
            continue;
        }

        uint64_t rx_ts_ns = get_time_ns();

        uint32_t idx_fq = 0;
        xsk_ring_prod__reserve(&umem_info->fq, rcvd, &idx_fq);

        for (uint32_t i = 0; i < rcvd; i++) {
            uint64_t addr = xsk_ring_cons__rx_desc(&xsk_info->rx, idx_rx)->addr;
            uint32_t len = xsk_ring_cons__rx_desc(&xsk_info->rx, idx_rx)->len;
            idx_rx++;

            char *pkt = xsk_umem__get_data(umem_info->buffer, addr);
            // Payload de Market Data despues de Ethernet(14) + IP(20) + UDP(8) = 42 bytes
            if (len >= 42 + sizeof(market_data_msg_t)) {
                market_data_msg_t *msg = (market_data_msg_t *)(pkt + 42);
                if (msg->magic == FEED_MAGIC && rx_ts_ns >= msg->send_ts_ns) {
                    latencies[received++] = rx_ts_ns - msg->send_ts_ns;
                }
            }

            // Devolver buffer al Fill Ring
            *xsk_ring_prod__fill_addr(&umem_info->fq, idx_fq++) = addr;
        }

        xsk_ring_cons__release(&xsk_info->rx, rcvd);
        xsk_ring_prod__submit(&umem_info->fq, rcvd);
    }

    printf("\n[AF_XDP] Medicion finalizada. Procesando percentiles...\n");

    if (received > 0) {
        qsort(latencies, received, sizeof(uint64_t), compare_uint64);
        uint64_t min_lat = latencies[0];
        uint64_t max_lat = latencies[received - 1];
        uint64_t p50  = latencies[(size_t)(received * 0.50)];
        uint64_t p90  = latencies[(size_t)(received * 0.90)];
        uint64_t p99  = latencies[(size_t)(received * 0.99)];
        uint64_t p999 = latencies[(size_t)(received * 0.999)];

        printf("\n====================================================\n");
        printf(" REPORTE DE LATENCIA DE COLA - AF_XDP ZERO-COPY\n");
        printf(" Muestras analizadas: %zu\n", received);
        printf("----------------------------------------------------\n");
        printf("  Min Latency:    %8lu ns  (%.2f us)\n", min_lat, min_lat / 1000.0);
        printf("  p50 (Mediana):  %8lu ns  (%.2f us)\n", p50, p50 / 1000.0);
        printf("  p90:            %8lu ns  (%.2f us)\n", p90, p90 / 1000.0);
        printf("  p99:            %8lu ns  (%.2f us)\n", p99, p99 / 1000.0);
        printf("  p99.9:          %8lu ns  (%.2f us)\n", p999, p999 / 1000.0);
        printf("  Max (Tail):     %8lu ns  (%.2f us)\n", max_lat, max_lat / 1000.0);
        printf("====================================================\n");

        FILE *fp = fopen(csv_out, "w");
        if (fp) {
            fprintf(fp, "sample_index,latency_ns\n");
            for (size_t i = 0; i < received; i++) {
                fprintf(fp, "%zu,%lu\n", i, latencies[i]);
            }
            fclose(fp);
            printf("[AF_XDP] CSV exportado a: %s\n", csv_out);
        }
    }

    free(latencies);
    return 0;
}
