#pragma once
#include <etherfabric/vi.h>
#include <etherfabric/pd.h>
#include <etherfabric/memreg.h>
#include <onload/extensions.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <iostream>
#include <iomanip>

#define CI_BSWAP_BE16(v) __builtin_bswap16((uint16_t)(v)) 
#define CI_IP4_IHL_VERSION(ihl)  ((4u << 4u) | ((ihl) >> 2u))

class EfviUdpClient
{
public:
  bool init(const std::string& dest_ip, uint16_t dest_port, const std::string& interface,
            const std::string& subscribe_ip, const std::string mac = "", int buf_size = N_BUF * PKT_BUF_SIZE) {
    /*if ((subscribe_fd_ = onload_socket_nonaccel(AF_INET, SOCK_DGRAM, 0)) < 0) {
      saveError("onload_socket_nonaccel failed", -errno);
      return false;
    }*/

    bzero(&local_addr, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(dest_port);
    inet_pton(AF_INET, subscribe_ip.c_str(), &(local_addr.sin_addr));
    /*
    if (setsockopt(subscribe_fd_, IPPROTO_IP, IP_MULTICAST_IF, (char *)&local_addr.sin_addr, sizeof(local_addr.sin_addr)) < 0){
      saveError("setsockopt IP_MULTICAST_IF failed", -errno);
      return false;
    }
    */

    bzero(&peer_addr, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(dest_port);
    inet_pton(AF_INET, dest_ip.c_str(), &(peer_addr.sin_addr));
    if ((0xff & peer_addr.sin_addr.s_addr) < 224){
      if (12 != mac.size()){
        std::cout << "can't find route mac for unicast" << std::endl;
        return false;
      }
      for(int i = 0; i < 6; ++i){
        route_mac[i] = hexchartoi(mac[2*i]) * 16 + hexchartoi(mac[2*i+1]); 
      }      
    }

    int rc;
    if ((rc = ef_driver_open(&dh)) < 0) {
      saveError("ef_driver_open failed", rc);
      return false;
    }
    if ((rc = ef_pd_alloc_by_name(&pd, dh, interface.c_str(), EF_PD_DEFAULT)) < 0) {
      saveError("ef_pd_alloc_by_name failed", rc);
      return false;
    }

    int vi_flags = EF_VI_FLAGS_DEFAULT;

    buf_num_ = buf_size / PKT_BUF_SIZE;
    if ((rc = ef_vi_alloc_from_pd(&vi, dh, &pd, dh, -1, 0, buf_num_ + 1, NULL, -1, (enum ef_vi_flags)vi_flags)) < 0) {
      saveError("ef_vi_alloc_from_pd failed", rc);
      return false;
    }
    std::cout <<"txq_size: " << ef_vi_transmit_capacity(&vi) << ", rxq_size: " << ef_vi_receive_capacity(&vi) << ", evq_size: " << ef_eventq_capacity(&vi) << std::endl;

    udp_prefix_len = 64 + ef_vi_receive_prefix_len(&vi) + 14 + 20 + 8;

    size_t alloc_size = buf_num_ * PKT_BUF_SIZE;
    buf_mmapped = true;
    pkt_bufs = (char*)mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB, -1, 0);
    if (pkt_bufs == MAP_FAILED) {
      // std::cout << "mmap() with MAP_HUGETLB failed. try using posix_memalign" << std::endl;
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
    
    #if 0
    //Store DMA address of the packet buffer memory
    dma_buf_addr = ef_memreg_dma_addr(&memreg, 0);
    init_udp_pkt(pkt_bufs, &vi, dh);
    #else
    for (int i = 0; i < buf_num_; i++) {
      struct pkt_buf* pkt = (struct pkt_buf*)(pkt_bufs + i * PKT_BUF_SIZE);
      pkt->post_addr = ef_memreg_dma_addr(&memreg, i * PKT_BUF_SIZE) + sizeof(ef_addr); // reserve a cache line for saving ef_addr...
      init_udp_pkt(&(pkt->eth), &vi, dh);
    }
    #endif    

    return true;
  }

  ~EfviUdpClient() { close("destruct"); }

  const std::string& getLastError() { return last_error_; };

  bool isConnected() { return subscribe_fd_ >= 0 && dh >= 0; }

  void close(const char* reason) {
    if (subscribe_fd_ >= 0) {
	  saveError(reason, 0);
      ::close(subscribe_fd_);
      subscribe_fd_ = -1;
    }
    if (dh >= 0) {
      ef_driver_close(dh);
      dh = -1;
    }
    if (pkt_bufs) {
      if (buf_mmapped) {
        munmap(pkt_bufs, buf_num_ * PKT_BUF_SIZE);
      }
      else {
        free(pkt_bufs);
      }
      pkt_bufs = nullptr;
    }
  }

  uint8_t hexchartoi(char c){
    if(c >= '0' && c <= '9')
      return c - '0';
    else if(c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    else
      return 0;
  }

  bool write(const char* data, uint32_t size) {
    if(buf_index_ == buf_num_)
      buf_index_ = 0;
 
    struct pkt_buf* pkt = (struct pkt_buf*)(pkt_bufs + buf_index_ * PKT_BUF_SIZE);
    uint32_t frame_len = update_udp_pkt(&(pkt->eth), size);
    memcpy(pkt + 1, data, size);
    int rc = ef_vi_transmit(&vi, pkt->post_addr, frame_len, buf_index_++);
    if(rc != 0){
      std::cout << "write failed" << std::endl;
      return false;
    }
    // std::cout << "write ok" << std::endl;
    read();
    // ef_vi_transmit_push(&vi);
    return true;
  }

  bool read() {
    ef_event evs[buf_num_];
    ef_request_id ids[buf_num_];
    int events = ef_eventq_poll(&vi, evs, buf_num_);
    
    for(int i = 0; i < events; ++i){
      if(EF_EVENT_TYPE_TX == EF_EVENT_TYPE(evs[i])){
        ef_vi_transmit_unbundle(&vi, &evs[i], ids);
        // std::cout << "read EF_EVENT_TYPE_TX event" << std::endl;
      }
    }
    return true;
  }

private:
  static const int N_BUF = 8;
  static const int PKT_BUF_SIZE = 2048;
  static const int EV_POLL_BATCH_SIZE = 1;

  #pragma pack(push, 1) 
  struct ci_ether_hdr 
  {
    uint8_t   ether_dhost[6];
    uint8_t   ether_shost[6];
    uint16_t  ether_type;
  };

  struct ci_ip4_hdr 
  {
    uint8_t   ip_ihl_version;
    uint8_t   ip_tos;
    uint16_t  ip_tot_len_be16;
    uint16_t  ip_id_be16;
    uint16_t  ip_frag_off_be16;
    uint8_t   ip_ttl;
    uint8_t   ip_protocol;
    uint16_t  ip_check_be16;
    uint32_t  ip_saddr_be32;
    uint32_t  ip_daddr_be32;
    /* ...options... */  
  }; 

  struct ci_udp_hdr 
  {
    uint16_t  udp_source_be16;
    uint16_t  udp_dest_be16;
    uint16_t  udp_len_be16;
    uint16_t  udp_check_be16;
  };

  struct pkt_buf
  {
    ef_addr post_addr;
    ci_ether_hdr eth;
    ci_ip4_hdr ip4;
    ci_udp_hdr udp;
  };
  #pragma pack(pop)

  void saveError(const char* msg, int rc) {
    last_error_ = msg;
    if (rc < 0) {
      last_error_ += ": ";
      last_error_ += strerror(-rc);
    }
  }

  int init_udp_pkt(void* buf, ef_vi *vi, ef_driver_handle dh){
    int ip_len = sizeof(struct ci_ip4_hdr) + sizeof(struct ci_udp_hdr);
    struct ci_ether_hdr* eth = (struct ci_ether_hdr*)buf;
    struct ci_ip4_hdr* ip4 = (struct ci_ip4_hdr*)(eth + 1);
    struct ci_udp_hdr* udp = (struct ci_udp_hdr*)(ip4 + 1);
  
    eth->ether_type = htons(0x0800);
    if((0xff & peer_addr.sin_addr.s_addr) < 224){
      //unicast
      memcpy(eth->ether_dhost, route_mac, 6);
    }else{
      //multicast
      eth->ether_dhost[0] = 0x1;
      eth->ether_dhost[1] = 0;
      eth->ether_dhost[2] = 0x5e;
      eth->ether_dhost[3] = 0x7f & (peer_addr.sin_addr.s_addr >> 8);
      eth->ether_dhost[4] = 0xff & (peer_addr.sin_addr.s_addr >> 16);
      eth->ether_dhost[5] = 0xff & (peer_addr.sin_addr.s_addr >> 24);
    }
    ef_vi_get_mac(vi, dh, eth->ether_shost);
  
    ci_ip4_hdr_init(ip4, 0, ip_len, 0, IPPROTO_UDP,
                    local_addr.sin_addr.s_addr,
                    peer_addr.sin_addr.s_addr);
    ci_udp_hdr_init(udp, ip4, local_addr.sin_port,
                    peer_addr.sin_port, 0);
    
    return 42;
  }

  int update_udp_pkt(void* buf, uint32_t paylen){
    struct ci_ether_hdr* eth = (struct ci_ether_hdr*)buf;
    struct ci_ip4_hdr* ip4 = (struct ci_ip4_hdr*)(eth + 1);
    struct ci_udp_hdr* udp = (struct ci_udp_hdr*)(ip4 + 1);
    ip4->ip_tot_len_be16 = CI_BSWAP_BE16(28 + paylen);
    udp->udp_len_be16 = CI_BSWAP_BE16(8 + paylen);
    //sizeof(eth) + sizeof(ip4) + sizeof(udp)
    return 42 + paylen;
  } 

  void ci_ip4_hdr_init(struct ci_ip4_hdr* ip, int opts_len, int tot_len, int id_be16,
                     int protocol, unsigned saddr_be32, unsigned daddr_be32){ 
    ip->ip_ihl_version = CI_IP4_IHL_VERSION(sizeof(*ip) + opts_len);
    ip->ip_tos = 0;
    ip->ip_tot_len_be16 = CI_BSWAP_BE16(tot_len);
    ip->ip_id_be16 = id_be16;
    ip->ip_frag_off_be16 = 0;
    ip->ip_ttl = 64;
    ip->ip_protocol = protocol;
    ip->ip_saddr_be32 = saddr_be32;
    ip->ip_daddr_be32 = daddr_be32;
    ip->ip_check_be16 = 0;
  }
  
  
  void ci_udp_hdr_init(struct ci_udp_hdr* udp, struct ci_ip4_hdr* ip,
                       unsigned sport_be16, unsigned dport_be16,
                       int payload_len){
    udp->udp_source_be16 = sport_be16;
    udp->udp_dest_be16 = dport_be16;
    udp->udp_len_be16 = CI_BSWAP_BE16(sizeof(*udp) + payload_len);
    udp->udp_check_be16 = 0;
  }

  struct ef_vi vi;
  char* pkt_bufs = nullptr;
  int udp_prefix_len;

  ef_driver_handle dh = -1;
  ef_addr dma_buf_addr;
  struct ef_pd pd;
  struct ef_memreg memreg;
  bool buf_mmapped;
  std::string last_error_;
  int buf_num_ = N_BUF;
  int buf_index_ = 0;
  int subscribe_fd_ = -1;
  struct sockaddr_in local_addr;
  struct sockaddr_in peer_addr;
  uint8_t route_mac[6];
};

