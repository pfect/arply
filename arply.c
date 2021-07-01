#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <netinet/in.h>

#include <linux/filter.h>
#include <linux/if_arp.h>

#define COUNT(x) (sizeof(x) / sizeof((x)[0]))

volatile sig_atomic_t arply_quit;

struct arply_addr {
    unsigned char ll[ETH_ALEN];
    unsigned char ip[4];
};

union arply_pkt {
    struct {
        struct ethhdr eth;
        struct arphdr arp;
        struct arply_addr s, t;
    } x;
    unsigned char buf[1UL << 16];
};

struct arply {
    int fd;
    struct arply_addr addr;
    unsigned index;
};

static void
arply_sa_handler()
{
    arply_quit = 1;
}

static int
arply_init(struct arply *arply, char *name, char *ip)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, name, sizeof(ifr.ifr_name) - 1);

    arply->fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));

    if (arply->fd == -1) {
        perror("socket");
        return 1;
    }
    if (ioctl(arply->fd, SIOCGIFINDEX, &ifr) || ifr.ifr_ifindex <= 0) {
        fprintf(stderr, "No interface %s found!\n", ifr.ifr_name);
        return 1;
    }
    arply->index = ifr.ifr_ifindex;

    if (ioctl(arply->fd, SIOCGIFHWADDR, &ifr)) {
        fprintf(stderr, "Unable to find the hwaddr of %s\n", ifr.ifr_name);
        return 1;
    }
    memcpy(&arply->addr.ll,
           &ifr.ifr_hwaddr.sa_data, ETH_ALEN);

    if (inet_pton(AF_INET, ip, &arply->addr.ip) != 1) {
        fprintf(stderr, "Unable to parse ip %s\n", ip);
        return 1;
    }
    return 0;
}

static int
arply_listen(struct arply *arply)
{
    struct sockaddr_ll sll = {
        .sll_family = AF_PACKET,
        .sll_protocol = htons(ETH_P_ALL),
        .sll_ifindex = arply->index,
    };
    if (bind(arply->fd, (struct sockaddr *)&sll, sizeof(sll)) == -1) {
        perror("bind");
        return 1;
    }
    struct sock_filter filter[] = {
        {0x28, 0, 0,    0x0000000c},
        {0x15, 0, 3,    0x00000806},
        {0x28, 0, 0,    0x00000014},
        {0x15, 0, 1, ARPOP_REQUEST},
        {0x06, 0, 0,    0x00040000},
        {0x06, 0, 0,    0x00000000},
    };
    struct sock_fprog bpf = {
        .len = COUNT(filter),
        .filter = filter,
    };
    if (setsockopt(arply->fd, SOL_SOCKET, SO_ATTACH_FILTER,
                   &bpf, sizeof(bpf)) == -1) {
        perror("setsockopt(SO_ATTACH_FILTER)");
        return 1;
    }
    return 0;
}

static int
arply_recv(struct arply *arply, union arply_pkt *pkt)
{
    ssize_t r = recv(arply->fd, pkt, sizeof(*pkt), 0);

    if (r < (ssize_t)sizeof(pkt->x)) {
        if (r == (ssize_t)-1)
            perror("recv");
        return -1;
    }
    if ((pkt->x.arp.ar_op != htons(ARPOP_REQUEST)) ||
        (pkt->x.arp.ar_hln != sizeof(pkt->x.s.ll)) ||
        (pkt->x.arp.ar_pln != sizeof(pkt->x.s.ip)))
        return -1;

    return 0;
}

static void
arply_set_signal(void)
{
    struct sigaction sa = {
        .sa_flags = 0,
    };
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = arply_sa_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGALRM, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

int
main(int argc, char **argv)
{
    arply_set_signal();

    if (argc != 3) {
        printf("usage: %s IFNAME IP\n", argv[0]);
        return 1;
    }
    struct arply arply;

    if (arply_init(&arply, argv[1], argv[2]))
        return 1;

    printf("Start replying ARP Request:\n"
           " src %02x:%02x:%02x:%02x:%02x:%02x ip %d.%d.%d.%d\n",
           arply.addr.ll[0], arply.addr.ll[1],
           arply.addr.ll[2], arply.addr.ll[3],
           arply.addr.ll[4], arply.addr.ll[5],
           arply.addr.ip[0], arply.addr.ip[1],
           arply.addr.ip[2], arply.addr.ip[3]);

    if (arply_listen(&arply))
        return 1;

    union arply_pkt pkt;

    struct pollfd fd = {
        .fd = arply.fd,
        .events = POLLIN,
    };
    while (!arply_quit) {
        int p = poll(&fd, 1, -1);

        if (p <= 0) {
            if (p == -1 && errno != EINTR) {
                perror("poll");
                return 1;
            }
            continue;
        }
        if ((fd.revents & POLLIN) && !arply_recv(&arply, &pkt)) {
            if (memcmp(pkt.x.t.ip, arply.addr.ip, sizeof(pkt.x.t.ip)))
                continue;

            memcpy(&pkt.x.t, &pkt.x.s, sizeof(pkt.x.t));
            memcpy(&pkt.x.s, &arply.addr, sizeof(pkt.x.s));
            memcpy(pkt.x.eth.h_dest, pkt.x.eth.h_source, sizeof(pkt.x.eth.h_dest));
            memcpy(pkt.x.eth.h_source, arply.addr.ll, sizeof(pkt.x.eth.h_source));
            pkt.x.arp.ar_op = htons(ARPOP_REPLY);

            if (send(arply.fd, &pkt.x, sizeof(pkt.x), 0) == -1) {
                switch (errno) {
                case EINTR:     /* FALLTHRU */
                case EAGAIN:    /* FALLTHRU */
                case ENETDOWN:
                    break;
                default:
                    perror("send(packet)");
                    return 1;
                }
            }
        }
    }
}
