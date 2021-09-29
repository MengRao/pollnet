#include <bits/stdc++.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include "../Efvi.h"
#include "Statistic.h"
#include "timestamp.h"

using namespace std;

#pragma pack(push, 1)
struct ci_ether_hdr
{
  uint8_t ether_dhost[6];
  uint8_t ether_shost[6];
  uint16_t ether_type;
};

struct ci_ip4_hdr
{
  uint8_t ip_ihl_version;
  uint8_t ip_tos;
  uint16_t ip_tot_len_be16;
  uint16_t ip_id_be16;
  uint16_t ip_frag_off_be16;
  uint8_t ip_ttl;
  uint8_t ip_protocol;
  uint16_t ip_check_be16;
  uint32_t ip_saddr_be32;
  uint32_t ip_daddr_be32;
  /* ...options... */
};

struct ci_icmp
{
  uint8_t type;
  uint8_t code;
  uint16_t check;
  uint16_t identifier;
  uint16_t seq_num;
  uint64_t ns;
  char pad[48]; // make total size 64 bytes
};

struct pkt_buf
{
  ef_addr post_addr;
  ci_ether_hdr eth;
  ci_ip4_hdr ip4;
  ci_icmp icmp;
};
#pragma pack(pop)

class EfviPingSender
{
public:
  bool init(const char* dest_ip) {
    uint8_t local_mac[6];
    uint8_t dest_mac[6];
    uint32_t local_ip_addr;

    inet_pton(AF_INET, dest_ip, &dest_ip_addr);

    char dest_mac_addr[64];
    if (!getMacFromARP(dest_ip, dest_mac_addr, interface)) {
      char gw[64];
      if (!getGW(dest_ip, gw) || !getMacFromARP(gw, dest_mac_addr, interface)) {
        saveError("Can't find dest ip from arp cache, please ping dest ip first", 0);
        return false;
      }
    }

    if (strlen(dest_mac_addr) != 17) {
      saveError("invalid dest_mac_addr", 0);
      return false;
    }
    for (int i = 0; i < 6; ++i) {
      dest_mac[i] = hexchartoi(dest_mac_addr[3 * i]) * 16 + hexchartoi(dest_mac_addr[3 * i + 1]);
    }

    {
      int fd = socket(AF_INET, SOCK_DGRAM, 0);
      struct ifreq ifr;
      ifr.ifr_addr.sa_family = AF_INET;
      strcpy(ifr.ifr_name, interface);
      ioctl(fd, SIOCGIFADDR, &ifr);
      ::close(fd);
      local_ip_addr = ((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr.s_addr;
    }

    int rc;
    if ((rc = ef_driver_open(&dh)) < 0) {
      saveError("ef_driver_open failed", rc);
      return false;
    }
    if ((rc = ef_pd_alloc_by_name(&pd, dh, interface, EF_PD_DEFAULT)) < 0) {
      saveError("ef_pd_alloc_by_name failed", rc);
      return false;
    }

    int vi_flags = EF_VI_FLAGS_DEFAULT;
    int ifindex = if_nametoindex(interface);
    unsigned long capability_val = 0;
    if (ef_vi_capabilities_get(dh, ifindex, EF_VI_CAP_CTPIO, &capability_val) == 0 && capability_val) {
      use_ctpio = true;
      vi_flags |= EF_VI_TX_CTPIO;
    }

    if ((rc = ef_vi_alloc_from_pd(&vi, dh, &pd, dh, -1, 0, N_BUF + 1, NULL, -1, (enum ef_vi_flags)vi_flags)) < 0) {
      saveError("ef_vi_alloc_from_pd failed", rc);
      return false;
    }
    ef_vi_get_mac(&vi, dh, local_mac);

    size_t alloc_size = N_BUF * PKT_BUF_SIZE;
    buf_mmapped = true;
    pkt_bufs = (char*)mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB, -1, 0);
    if (pkt_bufs == MAP_FAILED) {
      buf_mmapped = false;
      rc = posix_memalign((void**)&pkt_bufs, 4096, alloc_size);
      if (rc != 0) {
        saveError("posix_memalign failed", -rc);
        return false;
      }
    }

    if ((rc = ef_memreg_alloc(&memreg, dh, &pd, dh, pkt_bufs, alloc_size) < 0)) {
      saveError("ef_memreg_alloc failed", rc);
      return false;
    }

    for (int i = 0; i < N_BUF; i++) {
      struct pkt_buf* pkt = (struct pkt_buf*)(pkt_bufs + i * PKT_BUF_SIZE);
      pkt->post_addr = ef_memreg_dma_addr(&memreg, i * PKT_BUF_SIZE) + sizeof(ef_addr);
      init_ping_pkt(&(pkt->eth), local_ip_addr, local_mac, dest_ip_addr, dest_mac);
    }

    return true;
  }

  ~EfviPingSender() { close(); }

  const char* getLastError() { return last_error_; };

  bool isClosed() { return dh < 0; }

  void close() {
    if (dh >= 0) {
      ef_driver_close(dh);
      dh = -1;
    }
    if (pkt_bufs) {
      if (buf_mmapped) {
        munmap(pkt_bufs, N_BUF * PKT_BUF_SIZE);
      }
      else {
        free(pkt_bufs);
      }
      pkt_bufs = nullptr;
    }
  }

  bool write(uint16_t seq_num, int64_t ns) {
    struct pkt_buf* pkt = (struct pkt_buf*)(pkt_bufs + buf_index_ * PKT_BUF_SIZE);
    struct ci_ether_hdr* eth = &pkt->eth;
    struct ci_ip4_hdr* ip4 = (struct ci_ip4_hdr*)(eth + 1);
    struct ci_icmp* icmp = (struct ci_icmp*)(ip4 + 1);
    icmp->seq_num = htons(seq_num);
    icmp->ns = ns;

    uint16_t* icmp_16 = (uint16_t*)&icmp->seq_num;
    uint32_t icmpsum = icmpsum_cache;
    for (int i = 0; i < 5; i++) {
      icmpsum += icmp_16[i];
    }
    icmpsum += (icmpsum >> 16u);
    icmp->check = ~icmpsum & 0xffff;
    uint32_t frame_len = 98;

    int rc;
    if (use_ctpio) {
      ef_vi_transmit_ctpio(&vi, &pkt->eth, frame_len, 40);
      rc = ef_vi_transmit_ctpio_fallback(&vi, pkt->post_addr, frame_len, buf_index_);
    }
    else {
      rc = ef_vi_transmit(&vi, pkt->post_addr, frame_len, buf_index_);
    }

    buf_index_ = (buf_index_ + 1) % N_BUF;
    ef_event evs[N_BUF];
    ef_request_id ids[N_BUF];
    int events = ef_eventq_poll(&vi, evs, N_BUF);
    for (int i = 0; i < events; ++i) {
      if (EF_EVENT_TYPE_TX == EF_EVENT_TYPE(evs[i])) {
        ef_vi_transmit_unbundle(&vi, &evs[i], ids);
      }
    }
    return rc == 0;
  }

  bool getMacFromARP(const char* dest_ip, char* dest_mac, char* interface) {
    FILE* arp_file = fopen("/proc/net/arp", "r");
    if (!arp_file) {
      saveError("Can't open /proc/net/arp", -errno);
      return false;
    }
    char header[1024];
    if (!fgets(header, sizeof(header), arp_file)) {
      saveError("Invalid file /proc/net/arp", 0);
      fclose(arp_file);
      return false;
    }
    char ip[64], hw[64], device[64];
    while (3 == fscanf(arp_file, "%63s %*s %*s %63s %*s %63s", ip, hw, device)) {
      if (!strcmp(ip, dest_ip)) {
        strcpy(dest_mac, hw);
        strcpy(interface, device);
        fclose(arp_file);
        return true;
      }
    }
    fclose(arp_file);
    return false;
  }

  bool getGW(const char* ip, char* gw) {
    char buf[1024];
    sprintf(buf, "/usr/sbin/ip route get %s", ip);
    FILE* pipe = popen(buf, "r");
    if (!pipe) return false;
    while (fgets(buf, sizeof(buf), pipe)) {
      if (sscanf(buf, "%*s via %s", gw) == 1) {
        fclose(pipe);
        return true;
      }
    }
    fclose(pipe);
    return false;
  }

  void saveError(const char* msg, int rc) {
    snprintf(last_error_, sizeof(last_error_), "%s %s", msg, rc < 0 ? (const char*)strerror(-rc) : "");
  }

  uint8_t hexchartoi(char c) {
    if (c >= '0' && c <= '9')
      return c - '0';
    else if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    else if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    else
      return 0;
  }

  void init_ping_pkt(void* buf, uint32_t local_ip_addr, uint8_t* local_mac, uint32_t dest_ip_addr, uint8_t* dest_mac) {
    struct ci_ether_hdr* eth = (struct ci_ether_hdr*)buf;
    struct ci_ip4_hdr* ip4 = (struct ci_ip4_hdr*)(eth + 1);
    struct ci_icmp* icmp = (struct ci_icmp*)(ip4 + 1);

    eth->ether_type = htons(0x0800);
    memcpy(eth->ether_shost, local_mac, 6);
    memcpy(eth->ether_dhost, dest_mac, 6);

    ci_ip4_hdr_init(ip4, 0, 84, 0, IPPROTO_ICMP, local_ip_addr, dest_ip_addr);
    ci_icmp_init(icmp);
  }

  void ci_ip4_hdr_init(struct ci_ip4_hdr* ip, int opts_len, int tot_len, int id_be16, int protocol, unsigned saddr_be32,
                       unsigned daddr_be32) {
#define CI_IP4_IHL_VERSION(ihl) ((4u << 4u) | ((ihl) >> 2u))
    ip->ip_ihl_version = CI_IP4_IHL_VERSION(sizeof(*ip) + opts_len);
    ip->ip_tos = 0;
    ip->ip_tot_len_be16 = htons(tot_len);
    ip->ip_id_be16 = id_be16;
    ip->ip_frag_off_be16 = 0x0040;
    ip->ip_ttl = 64;
    ip->ip_protocol = protocol;
    ip->ip_saddr_be32 = saddr_be32;
    ip->ip_daddr_be32 = daddr_be32;
    ip->ip_check_be16 = 0;

    uint16_t* ip4 = (uint16_t*)ip;
    uint32_t ipsum = 0;
    for (int i = 0; i < 10; i++) {
      ipsum += ip4[i];
    }
    ipsum = (ipsum >> 16u) + (ipsum & 0xffff);
    ipsum += (ipsum >> 16u);
    ip->ip_check_be16 = ~ipsum & 0xffff;
  }

  void ci_icmp_init(struct ci_icmp* icmp) {
    icmp->type = 8;
    icmp->code = 0;
    icmp->check = 0;
    icmp->identifier = 0x1234;
    icmp->seq_num = 0;
    icmp->ns = 0;

    uint16_t* icmp_16 = (uint16_t*)icmp;
    icmpsum_cache = 0;
    for (int i = 0; i < 32; i++) {
      icmpsum_cache += icmp_16[i];
    }
    icmpsum_cache = (icmpsum_cache >> 16u) + (icmpsum_cache & 0xffff);
    icmpsum_cache += (icmpsum_cache >> 16u);
  }

  static const int N_BUF = 16;
  static const int PKT_BUF_SIZE = 2048;
  struct ef_vi vi;
  ef_driver_handle dh = -1;
  struct ef_pd pd;
  struct ef_memreg memreg;
  char* pkt_bufs = nullptr;
  bool use_ctpio = false;
  uint32_t icmpsum_cache;
  uint32_t buf_index_ = 0;
  bool buf_mmapped;
  char last_error_[64] = "";
  char interface[32];
  uint32_t dest_ip_addr;
};

volatile bool running = true;

void my_handler(int s) {
  running = false;
}

int main(int argc, char** argv) {
  struct sigaction sigIntHandler;
  sigIntHandler.sa_handler = my_handler;
  sigemptyset(&sigIntHandler.sa_mask);
  sigIntHandler.sa_flags = 0;
  sigaction(SIGINT, &sigIntHandler, NULL);

  if (argc < 2) {
    cout << "usage: " << argv[0] << " dest_ip [pack_per_sec=1]" << endl;
    return 1;
  }

  const char* dest_ip = argv[1];
  int pack_per_sec = 1;
  if (argc >= 3) {
    pack_per_sec = stoi(argv[2]);
  }

  const uint64_t send_interval = 1000000000 / pack_per_sec;

  EfviPingSender sender;
  EfviEthReceiver receiver;
  if (!sender.init(dest_ip)) {
    cout << sender.getLastError() << endl;
    return 1;
  }
  if (!receiver.init(sender.interface)) {
    cout << receiver.getLastError() << endl;
    return 1;
  }

  uint16_t seq = 1;
  uint64_t send_time = 0;

  Statistic<uint64_t> sta, write_sta;
  sta.reserve(10000);

  while (running) {
    receiver.read([&](const char* eth_data, uint32_t eth_size) {
      struct ci_ether_hdr* eth = (struct ci_ether_hdr*)eth_data;
      struct ci_ip4_hdr* ip4 = (struct ci_ip4_hdr*)(eth + 1);
      struct ci_icmp* icmp = (struct ci_icmp*)(ip4 + 1);
      if (eth->ether_type != 0x0008) return;
      if (ip4->ip_protocol != IPPROTO_ICMP) return;
      if (ip4->ip_saddr_be32 != sender.dest_ip_addr) return;
      if (icmp->type != 0) return;
      uint64_t lat = getns() - icmp->ns;
      cout << (eth_size - 34) << " bytes from " << dest_ip << ": icmp_seq=" << ntohs(icmp->seq_num)
           << " ttl=" << (int)ip4->ip_ttl << " time=" << lat << " ns" << endl;
      sta.add(lat);
    });
    uint64_t now = getns();
    if (now - send_time > send_interval) {
      sender.write(seq++, now);
      send_time = now;
    }
  }
  sta.print(cout);
  return 0;
}

