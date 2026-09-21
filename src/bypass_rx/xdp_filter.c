#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/udp.h>
#include <bpf/bpf_helpers.h>

// Mapa BPF que conecta el driver con nuestro socket AF_XDP en userspace
struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __type(key, int);
    __type(value, int);
} xsks_map SEC(".maps");

SEC("xdp")
int xdp_market_data_filter(struct xdp_md *ctx) {
    void *data_end = (void *)(long)ctx->data_end;
    void *data = (void *)(long)ctx->data;

    // 1. Parsear cabecera Ethernet
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;

    if (eth->h_proto != __builtin_bswap16(ETH_P_IP))
        return XDP_PASS;

    // 2. Parsear cabecera IPv4
    struct iphdr *ip = (void *)(eth + 1);
    if ((void *)(ip + 1) > data_end)
        return XDP_PASS;

    if (ip->protocol != IPPROTO_UDP)
        return XDP_PASS;

    // 3. Parsear cabecera UDP
    struct udphdr *udp = (void *)((char *)ip + (ip->ihl * 4));
    if ((void *)(udp + 1) > data_end)
        return XDP_PASS;

    // 4. Si el puerto destino es 12345 (Market Data), redirigir directamente al socket AF_XDP
    if (udp->dest == __builtin_bswap16(12345)) {
        return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
    }

    // Cualquier otro paquete sigue su curso normal al stack del kernel
    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
