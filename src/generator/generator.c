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

static void print_usage(const char *prog) {
    printf("Uso: %s [opciones]\n", prog);
    printf("Opciones:\n");
    printf("  -g <ip>       Direccion IP Multicast (por defecto: %s)\n", DEFAULT_MCAST_GRP);
    printf("  -p <puerto>   Puerto UDP (por defecto: %d)\n", DEFAULT_PORT);
    printf("  -c <count>    Cantidad de paquetes a enviar (0 = infinito, por defecto: 100000)\n");
    printf("  -r <rate>     Tasa de envio en paquetes/seg (0 = wire-speed máxima velocidad, por defecto: 50000)\n");
    printf("  -i <iface_ip> IP local de la interfaz de salida (por defecto: INADDR_ANY)\n");
    printf("  -h            Muestra esta ayuda\n");
}

int main(int argc, char *argv[]) {
    const char *mcast_grp = DEFAULT_MCAST_GRP;
    int port = DEFAULT_PORT;
    uint32_t count = 100000;
    uint32_t rate = 50000;
    const char *iface_ip = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "g:p:c:r:i:h")) != -1) {
        switch (opt) {
            case 'g': mcast_grp = optarg; break;
            case 'p': port = atoi(optarg); break;
            case 'c': count = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 'r': rate = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 'i': iface_ip = optarg; break;
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

    // Configurar TTL de Multicast
    unsigned char ttl = 1; // Enlace local por defecto
    if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        perror("Error setsockopt(IP_MULTICAST_TTL)");
        close(sockfd);
        return 1;
    }

    // Permitir loopback local de multicast
    unsigned char loop = 1;
    if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        perror("Error setsockopt(IP_MULTICAST_LOOP)");
    }

    // Configurar interfaz de salida si se especifica
    if (iface_ip) {
        struct in_addr local_iface;
        if (inet_aton(iface_ip, &local_iface) == 0) {
            fprintf(stderr, "IP de interfaz invalida: %s\n", iface_ip);
            close(sockfd);
            return 1;
        }
        if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_IF, &local_iface, sizeof(local_iface)) < 0) {
            perror("Error setsockopt(IP_MULTICAST_IF)");
            close(sockfd);
            return 1;
        }
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    if (inet_aton(mcast_grp, &dest_addr.sin_addr) == 0) {
        fprintf(stderr, "Direccion multicast invalida: %s\n", mcast_grp);
        close(sockfd);
        return 1;
    }

    printf("====================================================\n");
    printf(" HFT Synthetic Market Data Feed Generator\n");
    printf(" Autor: Glaciojtp (@Glaciojtp)\n");
    printf(" Destino: %s:%d | Conteo: %u | Tasa: %u pkts/s\n", mcast_grp, port, count, rate);
    printf("====================================================\n");

    market_data_msg_t msg;
    msg.magic = FEED_MAGIC;
    memcpy(msg.symbol, "NVDA    ", 8);
    msg.price = 12550; // $125.50
    msg.qty = 100;
    msg.side = 'B';

    uint32_t sent = 0;
    uint64_t interval_ns = (rate > 0) ? (1000000000ULL / rate) : 0;
    uint64_t next_send_time = get_time_ns();

    while (running && (count == 0 || sent < count)) {
        if (rate > 0) {
            uint64_t now = get_time_ns();
            if (now < next_send_time) {
                // Busy wait de alta precisión para evitar desplanificación del OS (sleep jitter)
                while (get_time_ns() < next_send_time) {
                    #if defined(__x86_64__) || defined(_M_X64)
                    __builtin_ia32_pause();
                    #endif
                }
            }
            next_send_time += interval_ns;
        }

        msg.seq_num = ++sent;
        msg.send_ts_ns = get_time_ns();

        ssize_t bytes = sendto(sockfd, &msg, sizeof(msg), 0,
                               (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (bytes < 0) {
            perror("Error en sendto");
            break;
        }

        if (sent % 25000 == 0) {
            printf("[Generator] Paquetes enviados: %u...\n", sent);
        }
    }

    printf("\n[Generator] Finalizado. Total transmitidos: %u paquetes.\n", sent);
    close(sockfd);
    return 0;
}
