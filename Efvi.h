/*
MIT License

Copyright (c) 2019 Meng Rao <raomeng1@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#pragma once
#include <etherfabric/vi.h>
#include <etherfabric/pd.h>
#include <etherfabric/memreg.h>
#include <etherfabric/capabilities.h>
#include <arpa/inet.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <net/if.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

class EfviReceiver
{
public:
  const char* getLastError() { return last_error_; };

  bool isClosed() { return dh < 0; }

protected:
  bool init(const char* interface) {
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

    if ((rc = ef_vi_alloc_from_pd(&vi, dh, &pd, dh, -1, N_BUF + 1, 0, NULL, -1, (enum ef_vi_flags)vi_flags)) < 0) {
      saveError("ef_vi_alloc_from_pd failed", rc);
      return false;
    }

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
      struct pkt_buf* pkt_buf = (struct pkt_buf*)(pkt_bufs + i * PKT_BUF_SIZE);
      pkt_buf->post_addr =
        ef_memreg_dma_addr(&memreg, i * PKT_BUF_SIZE) + 64; // reserve a cache line for saving ef_addr...
      if ((rc = ef_vi_receive_post(&vi, pkt_buf->post_addr, i)) < 0) {
        saveError("ef_vi_receive_post failed", rc);
        return false;
      }
    }
    return true;
  }

  void saveError(const char* msg, int rc) {
    snprintf(last_error_, sizeof(last_error_), "%s %s", msg, rc < 0 ? (const char*)strerror(-rc) : "");
  }

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

  static const int N_BUF = 512;
  static const int PKT_BUF_SIZE = 2048;
  struct pkt_buf
  {
    ef_addr post_addr;
  };

  struct ef_vi vi;
  char* pkt_bufs = nullptr;

  ef_driver_handle dh = -1;
  struct ef_pd pd;
  struct ef_memreg memreg;
  bool buf_mmapped;
  char last_error_[64] = "";
};

class EfviUdpReceiver : public EfviReceiver
{
public:
  bool init(const char* interface, const char* dest_ip, uint16_t dest_port, const char* subscribe_ip = "") {
    if (!EfviReceiver::init(interface)) {
      return false;
    }

    udp_prefix_len = 64 + ef_vi_receive_prefix_len(&vi) + 14 + 20 + 8;

    int rc;
    ef_filter_spec filter_spec;
    struct sockaddr_in sa_local;
    sa_local.sin_port = htons(dest_port);
    inet_pton(AF_INET, dest_ip, &(sa_local.sin_addr));
    ef_filter_spec_init(&filter_spec, EF_FILTER_FLAG_NONE);
    if ((rc = ef_filter_spec_set_ip4_local(&filter_spec, IPPROTO_UDP, sa_local.sin_addr.s_addr, sa_local.sin_port)) <
        0) {
      saveError("ef_filter_spec_set_ip4_local failed", rc);
      return false;
    }
    if ((rc = ef_vi_filter_add(&vi, dh, &filter_spec, NULL)) < 0) {
      saveError("ef_vi_filter_add failed", rc);
      return false;
    }

    if (subscribe_ip[0]) {
      if ((subscribe_fd_ = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        saveError("socket failed", -errno);
        return false;
      }

      struct ip_mreq group;
      inet_pton(AF_INET, subscribe_ip, &(group.imr_interface));
      inet_pton(AF_INET, dest_ip, &(group.imr_multiaddr));
      if (setsockopt(subscribe_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&group, sizeof(group)) < 0) {
        saveError("setsockopt IP_ADD_MEMBERSHIP failed", -errno);
        return false;
      }
    }

    return true;
  }

  ~EfviUdpReceiver() { close("destruct"); }

  void close(const char* reason) {
    if (subscribe_fd_ >= 0) {
      saveError(reason, 0);
      ::close(subscribe_fd_);
      subscribe_fd_ = -1;
    }
    EfviReceiver::close();
  }

  template<typename Handler>
  bool read(Handler handler) {
    ef_event evs;
    if (ef_eventq_poll(&vi, &evs, 1) == 0) return false;

    int id = EF_EVENT_RX_RQ_ID(evs);
    struct pkt_buf* pkt_buf = (struct pkt_buf*)(pkt_bufs + id * PKT_BUF_SIZE);
    int type = EF_EVENT_TYPE(evs);
    bool ret = false;
    if (type == EF_EVENT_TYPE_RX) {
      const char* data = (const char*)pkt_buf + udp_prefix_len;
      uint16_t len = ntohs(*(uint16_t*)(data - 4)) - 8;
      handler(data, len);
      ret = true;
    }
    ef_vi_receive_post(&vi, pkt_buf->post_addr, id);
    return ret;
  }

  template<typename Handler>
  bool recvfrom(Handler handler) {
    ef_event evs;
    if (ef_eventq_poll(&vi, &evs, 1) == 0) return false;

    struct sockaddr_in src_addr;

    int id = EF_EVENT_RX_RQ_ID(evs);
    struct pkt_buf* pkt_buf = (struct pkt_buf*)(pkt_bufs + id * PKT_BUF_SIZE);
    const char* data = (const char*)pkt_buf + udp_prefix_len;
    src_addr.sin_family = AF_INET;
    src_addr.sin_addr.s_addr = *(uint32_t*)(data - 16);
    src_addr.sin_port = *(uint16_t*)(data - 8);
    uint16_t len = ntohs(*(uint16_t*)(data - 4)) - 8;

    handler(data, len, src_addr);
    ef_vi_receive_post(&vi, pkt_buf->post_addr, id);
    return true;
  }

private:
  int udp_prefix_len;
  int subscribe_fd_ = -1;
};

class EfviEthReceiver : public EfviReceiver
{
public:
  bool init(const char* interface, bool promiscuous = false) {
    if (!EfviReceiver::init(interface)) {
      return false;
    }

    rx_prefix_len = ef_vi_receive_prefix_len(&vi);

    int rc;
    ef_filter_spec fs;
    ef_filter_spec_init(&fs, EF_FILTER_FLAG_NONE);
    if ((rc = ef_filter_spec_set_port_sniff(&fs, (int)promiscuous)) < 0) {
      saveError("ef_filter_spec_set_port_sniff failed", rc);
      return false;
    }
    if ((rc = ef_vi_filter_add(&vi, dh, &fs, NULL)) < 0) {
      saveError("ef_vi_filter_add failed", rc);
      return false;
    }

    return true;
  }

  ~EfviEthReceiver() { close(); }

  void close() { EfviReceiver::close(); }

  template<typename Handler>
  bool read(Handler handler) {
    ef_event evs;
    if (ef_eventq_poll(&vi, &evs, 1) == 0) return false;

    int id = EF_EVENT_RX_RQ_ID(evs);
    struct pkt_buf* pkt_buf = (struct pkt_buf*)(pkt_bufs + id * PKT_BUF_SIZE);
    int type = EF_EVENT_TYPE(evs);
    bool ret = false;
    if (type == EF_EVENT_TYPE_RX) {
      const char* data = (const char*)pkt_buf + 64 + rx_prefix_len;
      uint32_t len = EF_EVENT_RX_BYTES(evs) - rx_prefix_len;
      handler(data, len);
    }
    ef_vi_receive_post(&vi, pkt_buf->post_addr, id);
    return ret;
  }


private:
  int rx_prefix_len;
};

class EfviUdpSender
{
public:
  bool init(const char* interface, const char* local_ip, uint16_t local_port, const char* dest_ip, uint16_t dest_port) {

    struct sockaddr_in local_addr;
    struct sockaddr_in dest_addr;
    uint8_t local_mac[6];
    uint8_t dest_mac[6];
    local_addr.sin_port = htons(local_port);
    inet_pton(AF_INET, local_ip, &(local_addr.sin_addr));
    dest_addr.sin_port = htons(dest_port);
    inet_pton(AF_INET, dest_ip, &(dest_addr.sin_addr));

    if ((0xff & dest_addr.sin_addr.s_addr) < 224) { // unicast
      char dest_mac_addr[64];
      if (!getMacFromARP(interface, dest_ip, dest_mac_addr)) return false;
      // std::cout << "dest_mac_addr: " << dest_mac_addr << std::endl;
      if (strlen(dest_mac_addr) != 17) {
        saveError("invalid dest_mac_addr", 0);
        return false;
      }
      for (int i = 0; i < 6; ++i) {
        dest_mac[i] = hexchartoi(dest_mac_addr[3 * i]) * 16 + hexchartoi(dest_mac_addr[3 * i + 1]);
      }
    }
    else { // multicast
      dest_mac[0] = 0x1;
      dest_mac[1] = 0;
      dest_mac[2] = 0x5e;
      dest_mac[3] = 0x7f & (dest_addr.sin_addr.s_addr >> 8);
      dest_mac[4] = 0xff & (dest_addr.sin_addr.s_addr >> 16);
      dest_mac[5] = 0xff & (dest_addr.sin_addr.s_addr >> 24);
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
      init_udp_pkt(&(pkt->eth), local_addr, local_mac, dest_addr, dest_mac);
    }

    uint16_t* ip4 = (uint16_t*)&((struct pkt_buf*)pkt_bufs)->ip4;
    ipsum_cache = 0;
    for (int i = 0; i < 10; i++) {
      ipsum_cache += ip4[i];
    }
    ipsum_cache = (ipsum_cache >> 16u) + (ipsum_cache & 0xffff);
    ipsum_cache += (ipsum_cache >> 16u);

    return true;
  }

  ~EfviUdpSender() { close(); }

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

  bool write(const char* data, uint32_t size) {
    struct pkt_buf* pkt = (struct pkt_buf*)(pkt_bufs + buf_index_ * PKT_BUF_SIZE);
    struct ci_ether_hdr* eth = &pkt->eth;
    struct ci_ip4_hdr* ip4 = (struct ci_ip4_hdr*)(eth + 1);
    struct ci_udp_hdr* udp = (struct ci_udp_hdr*)(ip4 + 1);
    uint16_t iplen = htons(28 + size);
    ip4->ip_tot_len_be16 = iplen;
    udp->udp_len_be16 = htons(8 + size);
    memcpy(pkt + 1, data, size);
    uint32_t frame_len = 42 + size;
    int rc;
    if (use_ctpio) {
      uint32_t ipsum = ipsum_cache + iplen;
      ipsum += (ipsum >> 16u);
      ip4->ip_check_be16 = ~ipsum & 0xffff;
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

private:
  bool getMacFromARP(const char* interface, const char* dest_ip, char* dest_mac) {
    FILE* arp_file = fopen("/proc/net/arp", "r");
    if (!arp_file) {
      saveError("Can't open /proc/net/arp", -errno);
      return false;
    }
    static const int LINE_LEN = 1024;
    char header[1024];
    if (!fgets(header, sizeof(header), arp_file)) {
      saveError("Invalid file /proc/net/arp", 0);
      return false;
    }
    char ip[64], hw[64], device[64];
    while (3 == fscanf(arp_file, "%63s %*s %*s %63s %*s %63s", ip, hw, device)) {
      if (!strcmp(ip, dest_ip) && !strcmp(interface, device)) {
        strcpy(dest_mac, hw);
        return true;
      }
    }
    saveError("Can't find dest ip from arp cache, please ping dest ip first", 0);
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

  struct ci_udp_hdr
  {
    uint16_t udp_source_be16;
    uint16_t udp_dest_be16;
    uint16_t udp_len_be16;
    uint16_t udp_check_be16;
  };

  struct pkt_buf
  {
    ef_addr post_addr;
    ci_ether_hdr eth;
    ci_ip4_hdr ip4;
    ci_udp_hdr udp;
  };
#pragma pack(pop)

  void init_udp_pkt(void* buf, struct sockaddr_in& local_addr, uint8_t* local_mac, struct sockaddr_in& dest_addr,
                    uint8_t* dest_mac) {
    struct ci_ether_hdr* eth = (struct ci_ether_hdr*)buf;
    struct ci_ip4_hdr* ip4 = (struct ci_ip4_hdr*)(eth + 1);
    struct ci_udp_hdr* udp = (struct ci_udp_hdr*)(ip4 + 1);

    eth->ether_type = htons(0x0800);
    memcpy(eth->ether_shost, local_mac, 6);
    memcpy(eth->ether_dhost, dest_mac, 6);

    ci_ip4_hdr_init(ip4, 0, 0, 0, IPPROTO_UDP, local_addr.sin_addr.s_addr, dest_addr.sin_addr.s_addr);
    ci_udp_hdr_init(udp, ip4, local_addr.sin_port, dest_addr.sin_port, 0);
  }

  inline int update_udp_pkt(void* buf, uint32_t paylen) {
    struct ci_ether_hdr* eth = (struct ci_ether_hdr*)buf;
    struct ci_ip4_hdr* ip4 = (struct ci_ip4_hdr*)(eth + 1);
    struct ci_udp_hdr* udp = (struct ci_udp_hdr*)(ip4 + 1);
    uint16_t iplen = htons(28 + paylen);
    ip4->ip_tot_len_be16 = iplen;
    uint32_t ipsum = ipsum_cache + iplen;
    ipsum += (ipsum >> 16u);
    ip4->ip_check_be16 = ~ipsum & 0xffff;
    udp->udp_len_be16 = htons(8 + paylen);
    return 42 + paylen;
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
  }

  void ci_udp_hdr_init(struct ci_udp_hdr* udp, struct ci_ip4_hdr* ip, unsigned sport_be16, unsigned dport_be16,
                       int payload_len) {
    udp->udp_source_be16 = sport_be16;
    udp->udp_dest_be16 = dport_be16;
    udp->udp_len_be16 = htons(sizeof(*udp) + payload_len);
    udp->udp_check_be16 = 0;
  }

  static const int N_BUF = 4;
  static const int PKT_BUF_SIZE = 2048;
  struct ef_vi vi;
  ef_driver_handle dh = -1;
  struct ef_pd pd;
  struct ef_memreg memreg;
  char* pkt_bufs = nullptr;
  bool use_ctpio = false;
  uint32_t ipsum_cache;
  uint32_t buf_index_ = 0;
  bool buf_mmapped;
  char last_error_[64] = "";
};
