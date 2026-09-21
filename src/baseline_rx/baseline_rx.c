#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "../common/market_data.h"

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

static void print_usage(const char *prog) {
    printf("Uso: %s [opciones]\n", prog);
    printf("Opciones:\n");
    printf("  -g <ip>       Dirección IP Multicast (por defecto: %s)\n", DEFAULT_MCAST_GRP);
    printf("  -p <puerto>   Puerto UDP (por defecto: %d)\n", DEFAULT_PORT);
    printf("  -c <count>    Cantidad de paquetes esperados para análisis (por defecto: 100000)\n");
    printf("  -o <archivo>  Archivo CSV de salida de latencias (por defecto: latency_baseline.csv)\n");
    printf("  -h            Muestra esta ayuda\n");
}

int main(int argc, char *argv[]) {
    const char *mcast_grp = DEFAULT_MCAST_GRP;
    int port = DEFAULT_PORT;
    size_t target_count = 100000;
    const char *csv_out = "latency_baseline.csv";

    int opt;
    while ((opt = getopt(argc, argv, "g:p:c:o:h")) != -1) {
        switch (opt) {
            case 'g': mcast_grp = optarg; break;
            case 'p': port = atoi(optarg); break;
            case 'c': target_count = (size_t)strtoul(optarg, NULL, 10); break;
            case 'o': csv_out = optarg; break;
            case 'h': print_usage(argv[0]); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }

    signal(SIGINT, handle_sigint);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Error al crear socket UDP");
        return 1;
    }

    // Permitir reuso de dirección y puerto
    int enable = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    #ifdef SO_REUSEPORT
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));
    #endif

    // Agrandar buffer de recepción (4MB) para mitigar microbursts en stack POSIX
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("Error en bind()");
        close(sockfd);
        return 1;
    }

    // Suscribirse al grupo Multicast
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    inet_aton(mcast_grp, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("Error setsockopt(IP_ADD_MEMBERSHIP)");
        close(sockfd);
        return 1;
    }

    printf("====================================================\n");
    printf(" HFT Baseline POSIX UDP Receiver & Latency Profiler\n");
    printf(" Autor: Glaciojtp (@Glaciojtp)\n");
    printf(" Escuchando: %s:%d | Muestras objetivo: %zu\n", mcast_grp, port, target_count);
    printf("====================================================\n");

    uint64_t *latencies = malloc(target_count * sizeof(uint64_t));
    if (!latencies) {
        fprintf(stderr, "Error asignando memoria para muestras de latencia\n");
        close(sockfd);
        return 1;
    }

    market_data_msg_t msg;
    size_t received = 0;
    uint32_t last_seq = 0;
    uint32_t dropped = 0;

    printf("[Baseline RX] Esperando paquetes de Market Data...\n");

    while (running && received < target_count) {
        ssize_t bytes = recv(sockfd, &msg, sizeof(msg), 0);
        uint64_t rx_ts_ns = get_time_ns();

        if (bytes <= 0) {
            if (!running) break;
            continue;
        }

        if (bytes < (ssize_t)sizeof(market_data_msg_t) || msg.magic != FEED_MAGIC) {
            continue; // Paquete no perteneciente al protocolo
        }

        if (last_seq > 0 && msg.seq_num > last_seq + 1) {
            dropped += (msg.seq_num - last_seq - 1);
        }
        last_seq = msg.seq_num;

        if (rx_ts_ns >= msg.send_ts_ns) {
            latencies[received] = rx_ts_ns - msg.send_ts_ns;
        } else {
            latencies[received] = 0; // Deriva de reloj inter-host si no hay PTP
        }

        received++;
        if (received % 25000 == 0) {
            printf("[Baseline RX] Recibidos: %zu / %zu (Drops detectados: %u)\n", received, target_count, dropped);
        }
    }

    close(sockfd);

    if (received == 0) {
        printf("\nNo se recibieron paquetes.\n");
        free(latencies);
        return 0;
    }

    // Análisis estadístico y cálculo de percentiles
    qsort(latencies, received, sizeof(uint64_t), compare_uint64);

    uint64_t min_lat = latencies[0];
    uint64_t max_lat = latencies[received - 1];
    uint64_t sum_lat = 0;
    for (size_t i = 0; i < received; i++) {
        sum_lat += latencies[i];
    }
    double avg_lat = (double)sum_lat / (double)received;

    uint64_t p50 = latencies[(size_t)(received * 0.50)];
    uint64_t p90 = latencies[(size_t)(received * 0.90)];
    uint64_t p99 = latencies[(size_t)(received * 0.99)];
    uint64_t p999 = latencies[(size_t)(received * 0.999)];

    printf("\n====================================================\n");
    printf(" REPORTE DE LATENCIA DE COLA (TAIL LATENCY)\n");
    printf(" Stack: Linux Kernel POSIX UDP Socket (Baseline)\n");
    printf(" Muestras procesadas: %zu | Paquetes perdidos: %u\n", received, dropped);
    printf("----------------------------------------------------\n");
    printf("  Min Latency:    %8lu ns  (%.2f us)\n", min_lat, min_lat / 1000.0);
    printf("  Avg Latency:    %8.1f ns  (%.2f us)\n", avg_lat, avg_lat / 1000.0);
    printf("  p50 (Mediana):  %8lu ns  (%.2f us)\n", p50, p50 / 1000.0);
    printf("  p90:            %8lu ns  (%.2f us)\n", p90, p90 / 1000.0);
    printf("  p99:            %8lu ns  (%.2f us)\n", p99, p99 / 1000.0);
    printf("  p99.9:          %8lu ns  (%.2f us)\n", p999, p999 / 1000.0);
    printf("  Max (Tail):     %8lu ns  (%.2f us)\n", max_lat, max_lat / 1000.0);
    printf("====================================================\n");

    // Guardar resultados en CSV
    FILE *fp = fopen(csv_out, "w");
    if (fp) {
        fprintf(fp, "sample_index,latency_ns\n");
        for (size_t i = 0; i < received; i++) {
            fprintf(fp, "%zu,%lu\n", i, latencies[i]);
        }
        fclose(fp);
        printf("[Baseline RX] Archivo CSV exportado a: %s\n", csv_out);
    }

    free(latencies);
    return 0;
}
