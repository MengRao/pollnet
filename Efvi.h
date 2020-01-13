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
#include <arpa/inet.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

class EfviReceiver
{
public:
  const char* getLastError() { return last_error_; };

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
  bool init(const char* interface, const char* dest_ip, uint16_t dest_port, const char* subscribe_ip = nullptr) {
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

    if (subscribe_ip) {
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
    const char* data = (const char*)pkt_buf + udp_prefix_len;
    uint16_t len = ntohs(*(uint16_t*)(data - 4)) - 8;
    handler(data, len);
    ef_vi_receive_post(&vi, pkt_buf->post_addr, id);
    return true;
  }

  template<typename Handler>
  bool recvfrom(Handler handler) {
    ef_event evs;
    if (ef_eventq_poll(&vi, &evs, 1) == 0) return false;

    struct sockaddr_in src_addr;

    int id = EF_EVENT_RX_RQ_ID(evs);
    struct pkt_buf* pkt_buf = (struct pkt_buf*)(pkt_bufs + id * PKT_BUF_SIZE);
    const char* data = (const char*)pkt_buf + udp_prefix_len;
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
  bool init(const char* interface) {
    if (!EfviReceiver::init(interface)) {
      return false;
    }

    rx_prefix_len = ef_vi_receive_prefix_len(&vi);

    int rc;
    ef_filter_spec fs;
    ef_filter_spec_init(&fs, EF_FILTER_FLAG_NONE);
    if ((rc = ef_filter_spec_set_port_sniff(&fs, 1)) < 0) {
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
    const char* data = (const char*)pkt_buf + 64 + rx_prefix_len;
    uint32_t len = EF_EVENT_RX_BYTES(evs) - rx_prefix_len;
    handler(data, len);
    ef_vi_receive_post(&vi, pkt_buf->post_addr, id);
    return true;
  }


private:
  int rx_prefix_len;
};
