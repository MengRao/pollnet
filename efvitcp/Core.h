/*
MIT License

Copyright (c) 2021 Meng Rao <raomeng1@gmail.com>

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
#include <arpa/inet.h>
#include <net/if.h>
#include <etherfabric/vi.h>
#include <etherfabric/pd.h>
#include <etherfabric/memreg.h>
#include <etherfabric/capabilities.h>
#include <memory>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define EFVITCP_DEBUG 0

namespace efvitcp {

static const uint32_t RecvMSS = 1460;
static const uint32_t RecvBufSize = 2048;
static const uint32_t TsScale = 20; // ns -> ms
static const uint32_t TimerSlots = 256;
static const uint32_t TimeWaitTimeout = 60 * 1000;
static const uint64_t EmptyKey = (uint64_t)1 << 63;

struct EtherHeader
{
  uint8_t dst_mac[6];
  uint8_t src_mac[6];
  uint16_t ether_type;
};
struct IpHeader
{
  uint8_t header_len : 4, ip_ver : 4;
  uint8_t tos;
  uint16_t tot_len;
  uint16_t id;
  uint16_t frag_offset_flags;
  uint8_t ttl;
  uint8_t protocol;
  uint16_t checksum;
  uint32_t src_ip;
  uint32_t dst_ip;
};
struct TcpHeader
{
  uint16_t src_port;
  uint16_t dst_port;
  uint32_t seq_num;
  uint32_t ack_num;
  union
  {
    struct
    {
      uint16_t reserved : 4, data_offset : 4, fin : 1, syn : 1, rst : 1, psh : 1, ack : 1;
    };
    uint16_t offset_flags;
  };
  uint16_t window_size;
  uint16_t checksum;
  uint16_t urgent_pointer;
};

struct CSum
{
  CSum(uint32_t s = 0)
    : sum(s) {}

  inline uint16_t fold() {
    uint32_t res = (sum >> 16) + (sum & 0xffff);
    res += res >> 16;
    return ~res;
  }

  inline void add(uint16_t a) { sum += a; }
  inline void add(uint32_t a) {
    sum += a >> 16;
    sum += a & 0xffff;
  }
  inline void add(CSum s) { sum += s.sum; }
  template<uint32_t Len> // Len must be even
  void add(const void* p) {
    for (uint32_t i = 0; i < Len; i += 2) {
      add(*(uint16_t*)((const char*)p + i));
    }
  }
#if EFVITCP_DEBUG
  void add(const void* p, uint32_t len) { // len must be even
    for (uint32_t i = 0; i < len; i += 2) {
      add(*(uint16_t*)((const char*)p + i));
    }
  }
#endif

  inline void sub(uint16_t a) {
    sum += 0xffff;
    sum -= a;
  }
  inline void sub(uint32_t a) {
    sum += 0x1fffe;
    sum -= a >> 16;
    sum -= a & 0xffff;
  }

  template<bool reset, typename T>
  void setVar(T& var, T val) {
    if (reset) sub(var);
    var = val;
    add(val);
  }

  uint32_t sum;
};

#pragma pack(push, 1)
struct RecvBuf
{
  ef_addr post_addr;
  uint16_t __pad; // to make ip_hdr 4 types aligned
};

struct SendBuf
{
  ef_addr post_addr;
  uint32_t send_ts;
  bool avail;
  uint8_t pad;
  EtherHeader eth_hdr;
  IpHeader ip_hdr;
  TcpHeader tcp_hdr;

  inline void setOptDataLen(uint16_t len, CSum ipsum, CSum tcpsum) {
    ip_hdr.tot_len = htons(40 + len);
    ipsum.add(ip_hdr.tot_len);
    ip_hdr.checksum = ipsum.fold();
    tcpsum.add(htons(20 + len));
    tcp_hdr.checksum = tcpsum.fold();
  }
};
#pragma pack(pop)

inline uint64_t connHashKey(uint32_t ip, uint16_t port) {
  uint64_t key = ntohl(ip);
  uint64_t p = ntohs(port);
  // Most systems(including linux) have ephemeral ports with msb set to 1
  return (key << 15) | (p & 0x7fff) | ((p & 0x8000) << 32);
}

inline constexpr int getMSB(uint32_t n) {
  return n == 0 ? 0 : getMSB(n >> 1) + 1;
}

struct ConnHashEntry
{
  uint64_t key;
  uint32_t conn_id;
};

struct TimerNode
{
  TimerNode()
    : prev(this)
    , next(this) {}

  bool isUnlinked() { return prev == this; }

  void unlink() {
    prev->next = next;
    next->prev = prev;
    prev = next = this;
  }
  TimerNode* prev;
  TimerNode* next;
  uint32_t conn_id;
  uint32_t expire_ts;
};

struct TimeWaitConn
{
  uint8_t dst_mac[6];
  bool has_ts;
  uint32_t dst_ip;
  uint16_t src_port;
  uint16_t dst_port;
  uint32_t seq_num;
  uint32_t ack_num;
  uint32_t tsecr;
  TimerNode timer;
};

#if EFVITCP_DEBUG
void dumpPack(IpHeader* ip_hdr) {
  TcpHeader* tcp_hdr = (TcpHeader*)(ip_hdr + 1);
  uint32_t seq_num = ntohl(tcp_hdr->seq_num) + tcp_hdr->syn;
  uint32_t ack_num = ntohl(tcp_hdr->ack_num);
  cout << "dump Pack: tcp_hdr->syn: " << tcp_hdr->syn << ", tcp_hdr->ack: " << tcp_hdr->ack
       << ", tcp_hdr->fin: " << tcp_hdr->fin << ", tcp_hdr->rst: " << tcp_hdr->rst << ", seq_num: " << seq_num
       << ", ack_num: " << ack_num << ", window_size: " << ntohs(tcp_hdr->window_size)
       << ", src_port: " << htons(tcp_hdr->src_port) << ", dst_port: " << htons(tcp_hdr->dst_port) << endl;
}
#endif

template<typename Conf>
class Core
{
public:
  static const uint32_t SendBufSize = Conf::SendBuf1K ? 1024 : 2048;
  static const uint32_t MaxSendMTU = SendBufSize - (uint32_t)(offsetof(SendBuf, ip_hdr));
  static const uint32_t SendMTU = MaxSendMTU < 1500 ? MaxSendMTU : 1500;
  static const uint32_t MaxTableSize = 1 << (1 + getMSB(Conf::MaxConnCnt + Conf::MaxTimeWaitConnCnt));
  static const uint32_t TotalTableSize = MaxTableSize + Conf::MaxConnCnt + Conf::MaxTimeWaitConnCnt;

  Core() = default;
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  ~Core() { destruct(); }

  void destruct() {
    ef_memreg_free(&memreg, dh);
    ef_vi_free(&vi, dh);
    ef_pd_free(&pd, dh);
    ef_driver_close(dh);
    dh = -1;
  }

  const char* init(const char* interface) {
    destruct();
    now_ts = getns() >> TsScale;
#if EFVITCP_DEBUG
    srand(now_ts);
#endif
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    ifr.ifr_addr.sa_family = AF_INET;
    strcpy(ifr.ifr_name, interface);
    int rc = ioctl(fd, SIOCGIFADDR, &ifr);
    ::close(fd);
    if (rc != 0) return "ioctl SIOCGIFADDR failed";
    local_ip = ((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr.s_addr;

    if (ef_driver_open(&dh) < 0) return "ef_driver_open failed";
    if (ef_pd_alloc_by_name(&pd, dh, interface, EF_PD_DEFAULT) < 0) return "ef_pd_alloc_by_name failed";
    int vi_flags = EF_VI_FLAGS_DEFAULT;
    int ifindex = if_nametoindex(interface);
    unsigned long capability_val = 0;
    if (ef_vi_capabilities_get(dh, ifindex, EF_VI_CAP_CTPIO, &capability_val) == 0 && capability_val) {
      use_ctpio = true;
      vi_flags |= EF_VI_TX_CTPIO;
    }
    if ((rc = ef_vi_alloc_from_pd(&vi, dh, &pd, dh, -1, Conf::RecvBufCnt + 1, std::min(2048ul, SendBufCnt), NULL, -1,
                                  (enum ef_vi_flags)vi_flags)) < 0)
      return "ef_vi_alloc_from_pd failed";
    ef_vi_get_mac(&vi, dh, local_mac);
    receive_prefix_len = ef_vi_receive_prefix_len(&vi);
    pkt_buf = (uint8_t*)((uint64_t)(pkt_buf_blk + TotalBufAlign) & ~(TotalBufAlign - 1));
    if (ef_memreg_alloc(&memreg, dh, &pd, dh, pkt_buf, TotalBufSize) < 0) return "ef_memreg_alloc failed";

    for (uint32_t i = 0; i < Conf::RecvBufCnt; i++) {
      RecvBuf* buf = (RecvBuf*)(pkt_buf + i * RecvBufSize);
      buf->post_addr = ef_memreg_dma_addr(&memreg, (uint8_t*)(buf + 1) - pkt_buf);
      if (ef_vi_receive_post(&vi, buf->post_addr, i) < 0) return "ef_vi_receive_post failed";
    }
    for (uint32_t i = 0; i < SendBufCnt; i++) {
      SendBuf* buf = getSendBuf(i);
      buf->post_addr = ef_memreg_dma_addr(&memreg, (uint8_t*)&buf->eth_hdr - pkt_buf);
      buf->avail = true;
      memcpy(buf->eth_hdr.src_mac, local_mac, 6);
      buf->eth_hdr.ether_type = ntohs(0x0800);
      buf->ip_hdr.header_len = 5;
      buf->ip_hdr.ip_ver = 4;
      buf->ip_hdr.tos = 0;
      buf->ip_hdr.id = 0;
      buf->ip_hdr.frag_offset_flags = ntohs(0x4000); // DF
      buf->ip_hdr.ttl = 64;
      buf->ip_hdr.protocol = 6;
      buf->ip_hdr.src_ip = local_ip;
    }
    SendBuf* rst = (SendBuf*)(pkt_buf + TotalBufSize - SendBufSize); // or tw ack
    rst_ipsum = 0;
    rst_ipsum.add<sizeof(IpHeader)>(&rst->ip_hdr);
    rst_tcpsum = 0;
    rst_tcpsum.add(rst->ip_hdr.src_ip);
    rst_tcpsum.add(ntohs(0x6));                // [zero protocol]
    *(uint32_t*)(rst + 1) = ntohl(0x0101080a); // ts header

    for (uint32_t i = 0; i < Conf::MaxConnCnt; i++) conns[i] = i;
    conn_cnt = 0;
    for (uint32_t i = 0; i < Conf::MaxTimeWaitConnCnt; i++) {
      tw_ids[i] = i;
      TimeWaitConn& tw = tws[i];
      tw.timer.conn_id = Conf::MaxConnCnt + i;
    }
    tw_cnt = 0;
    for (auto& entry : conn_tbl) entry.key = EmptyKey;
    tbl_mask = std::min(MaxTableSize, 128u) - 1;

    return nullptr;
  }

  int64_t getns() {
    timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000000000 + ts.tv_nsec;
  }

  void delFilter() { ef_vi_filter_del(&vi, dh, &filter_cookie); }

  const char* setClientFilter(uint16_t& local_port_be, uint32_t remote_ip, uint16_t remote_port_be) {
    delFilter();
    bool auto_gen = local_port_be == 0;
    int retries = auto_gen ? 10 : 1;
    const char* err;
    int rc;
    while (retries--) {
      if (auto_gen && (err = autoGetPort(local_port_be))) return err;
      ef_filter_spec filter_spec;
      ef_filter_spec_init(&filter_spec, EF_FILTER_FLAG_NONE);
      if (ef_filter_spec_set_ip4_full(&filter_spec, IPPROTO_TCP, local_ip, local_port_be, remote_ip, remote_port_be) <
          0)
        return "ef_filter_spec_set_ip4_full failed";
      if ((rc = ef_vi_filter_add(&vi, dh, &filter_spec, &filter_cookie)) < 0) {
        if (rc == -17) continue; // port exists
        return "ef_vi_filter_add failed";
      }
      return nullptr;
    }
    return "no available port";
  }

  const char* autoGetPort(uint16_t& port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "socket failed";
    struct sockaddr_in local_addr;
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = local_ip;
    local_addr.sin_port = 0;
    if (bind(fd, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
      ::close(fd);
      return "bind failed";
    }
    socklen_t addrlen = sizeof(local_addr);
    getsockname(fd, (struct sockaddr*)&local_addr, &addrlen);
    ::close(fd);
    port = local_addr.sin_port;
    return nullptr;
  }

  const char* setServerFilter(uint16_t local_port_be) {
    delFilter();
    ef_filter_spec filter_spec;
    ef_filter_spec_init(&filter_spec, EF_FILTER_FLAG_NONE);
    if (ef_filter_spec_set_ip4_local(&filter_spec, IPPROTO_TCP, local_ip, local_port_be) < 0)
      return "ef_filter_spec_set_ip4_local failed";
    if (ef_vi_filter_add(&vi, dh, &filter_spec, &filter_cookie) < 0) return "ef_vi_filter_add failed";
    return nullptr;
  }

  void sumRst(SendBuf* rst, bool has_ts) {
    CSum ipsum = rst_ipsum;
    ipsum.add(rst->ip_hdr.dst_ip);

    CSum tcpsum = rst_tcpsum;
    tcpsum.add(rst->ip_hdr.dst_ip);
    tcpsum.add(rst->tcp_hdr.src_port);
    tcpsum.add(rst->tcp_hdr.dst_port);
    tcpsum.add(rst->tcp_hdr.seq_num);
    tcpsum.add(rst->tcp_hdr.ack_num);
    tcpsum.add(rst->tcp_hdr.offset_flags);
    if (has_ts) tcpsum.add<12>(rst + 1);
    rst->setOptDataLen(has_ts ? 12 : 0, ipsum, tcpsum);
  }

  void rspRst(EtherHeader* eth_hdr) {
    SendBuf* rst = (SendBuf*)(pkt_buf + TotalBufSize - SendBufSize); // get last send buf
    IpHeader* ip_hdr = (IpHeader*)(eth_hdr + 1);
    TcpHeader* tcp_hdr = (TcpHeader*)(ip_hdr + 1);
    if (tcp_hdr->rst || !rst->avail) return;
    memcpy(rst->eth_hdr.dst_mac, eth_hdr->src_mac, 6);
    rst->ip_hdr.dst_ip = ip_hdr->src_ip;
    rst->tcp_hdr.src_port = tcp_hdr->dst_port;
    rst->tcp_hdr.dst_port = tcp_hdr->src_port;
    rst->tcp_hdr.rst = 1;
    rst->tcp_hdr.data_offset = 5;
    if (tcp_hdr->ack) {
      rst->tcp_hdr.ack = 0;
      rst->tcp_hdr.seq_num = tcp_hdr->ack_num;
    }
    else {
      rst->tcp_hdr.ack = 1;
      rst->tcp_hdr.seq_num = 0;
      uint32_t seg_len = ntohs(ip_hdr->tot_len) - 20 - (tcp_hdr->data_offset << 2) + tcp_hdr->syn + tcp_hdr->fin;
      rst->tcp_hdr.ack_num = htonl(ntohl(tcp_hdr->seq_num) + seg_len);
    }
    sumRst(rst, false);
    send(rst);
  }

  void ackTW(TimeWaitConn& tw) {
    SendBuf* ack = (SendBuf*)(pkt_buf + TotalBufSize - SendBufSize); // get last send buf
    if (!ack->avail) return;
    memcpy(ack->eth_hdr.dst_mac, tw.dst_mac, 6);
    ack->ip_hdr.dst_ip = tw.dst_ip;
    ack->tcp_hdr.src_port = tw.src_port;
    ack->tcp_hdr.dst_port = tw.dst_port;
    ack->tcp_hdr.rst = 0;
    ack->tcp_hdr.ack = 1;
    ack->tcp_hdr.seq_num = tw.seq_num;
    ack->tcp_hdr.ack_num = tw.ack_num;
    if (tw.has_ts) {
      ack->tcp_hdr.data_offset = 8;
      uint32_t* opt = (uint32_t*)(ack + 1);
      opt[1] = now_ts;
      opt[2] = tw.tsecr;
    }
    else
      ack->tcp_hdr.data_offset = 5;
    sumRst(ack, tw.has_ts);
    send(ack);
  }

#if EFVITCP_DEBUG
  void checksum(IpHeader* ip_hdr) {
    TcpHeader* tcp_hdr = (TcpHeader*)(ip_hdr + 1);
    CSum sum = 0;
    sum.add<sizeof(IpHeader)>(ip_hdr);
    uint16_t res = sum.fold();
    if (res) {
      cout << "invalid ip sum: " << res << endl;
      exit(1);
    }

    sum = 0;
    sum.add(ip_hdr->src_ip);
    sum.add(ip_hdr->dst_ip);
    sum.add(ntohs(0x6));
    uint16_t tcp_len = ntohs(ip_hdr->tot_len) - 20;
    sum.add(htons(tcp_len));
    sum.add(tcp_hdr, tcp_len);
    res = sum.fold();
    if (res) {
      cout << "invalid tcp sum: " << res << endl;
      exit(1);
    }
  }
#endif

  void send(SendBuf* buf) {
    uint32_t send_id = (uint64_t)((uint8_t*)buf - pkt_buf - RecvBufSize * Conf::RecvBufCnt) / SendBufSize;
    uint32_t frame_len = 14 + ntohs(buf->ip_hdr.tot_len);
#if EFVITCP_DEBUG
    checksum(&buf->ip_hdr);
    if (rand() % 100 < 3) {
      return; // drop rate at 3%
    }
#endif

    if (use_ctpio) {
      ef_vi_transmit_ctpio(&vi, &buf->eth_hdr, frame_len, 40);
      ef_vi_transmit_ctpio_fallback(&vi, buf->post_addr, frame_len, send_id);
    }
    else {
      ef_vi_transmit(&vi, buf->post_addr, frame_len, send_id);
    }
    buf->avail = false;
  }

  template<typename RecvHandler>
  void pollNet(RecvHandler recv_handler) {
    ef_event evs[64];
    ef_request_id tx_ids[EF_VI_TRANSMIT_BATCH];
    int n_ev = ef_eventq_poll(&vi, evs, 64);
    bool received = false;
    for (int i = 0; i < n_ev; i++) {
      switch (EF_EVENT_TYPE(evs[i])) {
        case EF_EVENT_TYPE_RX: {
          uint32_t id = EF_EVENT_RX_RQ_ID(evs[i]);
          RecvBuf* buf = (RecvBuf*)(pkt_buf + id * RecvBufSize);
          EtherHeader* eth_hdr = (EtherHeader*)((uint8_t*)(buf + 1) + receive_prefix_len);
          IpHeader* ip_hdr = (IpHeader*)(eth_hdr + 1);
          TcpHeader* tcp_hdr = (TcpHeader*)(ip_hdr + 1);
          uint64_t key = connHashKey(ip_hdr->src_ip, tcp_hdr->src_port);
          ConnHashEntry* entry = findConnEntry(key);
          if (entry->key == key && entry->conn_id >= Conf::MaxConnCnt) { // is tw
            TimeWaitConn& tw = tws[entry->conn_id - Conf::MaxConnCnt];
            bool seq_expected = tcp_hdr->seq_num == tw.ack_num;
            if (tcp_hdr->rst) {
              if (seq_expected) {
                tw.timer.unlink();
                delConnEntry(key);
              }
            }
            else if (!seq_expected || ntohs(ip_hdr->tot_len) - 20 - (tcp_hdr->data_offset << 2) + tcp_hdr->syn +
                                        tcp_hdr->fin) { // has data
              ackTW(tw);
              // do we need to restart timer?
            }
          }
          else {
            recv_handler(key, entry, eth_hdr);
          }

          ef_vi_receive_init(&vi, buf->post_addr, id);
          received = true;
          break;
        }
        case EF_EVENT_TYPE_RX_DISCARD: {
          uint32_t id = EF_EVENT_RX_RQ_ID(evs[i]);
          RecvBuf* buf = (RecvBuf*)(pkt_buf + id * RecvBufSize);
          ef_vi_receive_init(&vi, buf->post_addr, id);
          received = true;
          break;
        }
        case EF_EVENT_TYPE_TX:
        case EF_EVENT_TYPE_TX_ERROR: {
          int n_id = ef_vi_transmit_unbundle(&vi, &evs[i], tx_ids);
          for (int i = 0; i < n_id; i++) {
            uint32_t send_id = tx_ids[i];
            getSendBuf(send_id)->avail = true;
          }
          break;
        }
      }
    }
    if (received) ef_vi_receive_push(&vi);
  }

  inline SendBuf* getSendBuf(uint32_t id) {
    return (SendBuf*)(pkt_buf + RecvBufSize * Conf::RecvBufCnt + id * SendBufSize);
  }

  ConnHashEntry* findConnEntry(uint64_t key) {
    ConnHashEntry* entry = conn_tbl + (key & tbl_mask);
    while (entry->key < key) entry++;
    return entry;
  }

  uint32_t getTblSize() { return conn_cnt + tw_cnt; }

  void addConnEntry(ConnHashEntry* entry, uint64_t key, uint32_t conn_id) {
    while (entry->key != EmptyKey) {
      std::swap(entry->key, key);
      std::swap(entry->conn_id, conn_id);
      while ((++entry)->key < key)
        ;
    }
    entry->key = key;
    entry->conn_id = conn_id;
    tryExpandConnTbl();
  }

  void delConnEntry(uint64_t key) {
    ConnHashEntry* entry = findConnEntry(key);
#if EFVITCP_DEBUG
    if (entry->key != key) {
      cout << "delConnEntry failed, entry->key: " << entry->key << ", key: " << key << endl;
      printTbl();
      exit(1);
    }
#endif
    if (entry->conn_id < Conf::MaxConnCnt)
      conns[--conn_cnt] = entry->conn_id;
    else
      tw_ids[--tw_cnt] = entry->conn_id - Conf::MaxConnCnt;
#if EFVITCP_DEBUG
    cout << "delConnEntry, key: " << key << ", conn_id: " << entry->conn_id << ", conn_cnt: " << conn_cnt
         << ", tw_cnt: " << tw_cnt << endl;
#endif

    while (true) {
      ConnHashEntry* next = entry + 1;
      while (conn_tbl + (next->key & tbl_mask) > entry) next++;
      if (next->key == EmptyKey) break;
      entry->key = next->key;
      entry->conn_id = next->conn_id;
      entry = next;
    }
    entry->key = EmptyKey;
  }

  void enterTW(uint64_t key, SendBuf* buf, bool has_ts, uint32_t tsecr) {
    if (tw_cnt == Conf::MaxTimeWaitConnCnt) {
      delConnEntry(key);
      return;
    }
    ConnHashEntry* entry = findConnEntry(key);
#if EFVITCP_DEBUG
    if (entry->key != key) {
      cout << "enterTW failed, entry->key: " << entry->key << ", key: " << key << endl;
      printTbl();
      exit(1);
      return;
    }
#endif
    conns[--conn_cnt] = entry->conn_id;
    uint32_t tw_id = tw_ids[tw_cnt++];
#if EFVITCP_DEBUG
    cout << "enterTW, key: " << key << ", conn_cnt: " << conn_cnt << ", tw_cnt: " << tw_cnt
         << ", src port: " << htons(buf->tcp_hdr.src_port) << ", dst port: " << htons(buf->tcp_hdr.dst_port) << endl;
#endif
    entry->conn_id = Conf::MaxConnCnt + tw_id;
    TimeWaitConn& tw = tws[tw_id];
    memcpy(tw.dst_mac, buf->eth_hdr.dst_mac, 6);
    tw.has_ts = has_ts;
    tw.dst_ip = buf->ip_hdr.dst_ip;
    tw.src_port = buf->tcp_hdr.src_port;
    tw.dst_port = buf->tcp_hdr.dst_port;
    tw.seq_num = buf->tcp_hdr.seq_num;
    tw.ack_num = buf->tcp_hdr.ack_num;
    tw.tsecr = tsecr;
    addTimer(TimeWaitTimeout, &tw.timer);
  }

#if EFVITCP_DEBUG
  void printTbl() {
    for (uint32_t i = 0; i <= tbl_mask; i++) {
      ConnHashEntry* entry = conn_tbl + i;
      cout << "i: " << i << ", key: " << entry->key << ", orig_i: " << (entry->key & tbl_mask)
           << ", conn_id: " << entry->conn_id << endl;
    }
  }
#endif

  void tryExpandConnTbl() {
    if (getTblSize() * 2 <= tbl_mask) return;
#if EFVITCP_DEBUG
    cout << "tryExpandConnTbl, getTblSize(): " << getTblSize() << ", tbl_mask: " << tbl_mask << endl;
    printTbl();
#endif
    ConnHashEntry* end = conn_tbl + tbl_mask + 1;
    tbl_mask = tbl_mask * 2 + 1;
    ConnHashEntry* new_end = conn_tbl + tbl_mask + 1;
    while (end->key != EmptyKey) std::swap(*new_end++, *end++);
    auto rehash = [&](ConnHashEntry* entry, uint64_t cnt) {
      for (; cnt; entry++) {
        if (entry->key == EmptyKey) continue;
        ConnHashEntry* new_entry = findConnEntry(entry->key);
#if EFVITCP_DEBUG
        if (new_entry->key != entry->key && new_entry->key != EmptyKey) {
          cout << "invalid rehash key, new_entry->key: " << new_entry->key << ", entry->key: " << entry->key << endl;
          cout << "entry pos: " << (entry - conn_tbl) << ", new_entry pos: " << (new_entry - conn_tbl) << endl;
          exit(1);
        }
#endif
        std::swap(entry->key, new_entry->key);
        new_entry->conn_id = entry->conn_id;
        cnt--;
      }
    };
    uint64_t end_cnt = new_end - (conn_tbl + tbl_mask + 1);
    rehash(conn_tbl, getTblSize() - end_cnt);
    rehash(conn_tbl + tbl_mask + 1, end_cnt);
#if EFVITCP_DEBUG
    printTbl();
#endif
  }

  void addTimer(uint32_t duration_ts, TimerNode* node) {
    TimerNode* slot;
    if (duration_ts <= TimerSlots) {
      slot = &timer_slots[0][(now_ts + duration_ts) % TimerSlots];
    }
    else {
      duration_ts = std::min(duration_ts, TimerSlots * (TimerSlots + 1) - 1 - (now_ts % TimerSlots));
      node->expire_ts = now_ts + duration_ts;
      slot = &timer_slots[1][node->expire_ts / TimerSlots % TimerSlots];
#if EFVITCP_DEBUG
      uint32_t cur_long = now_ts / TimerSlots;
      uint32_t new_long = node->expire_ts / TimerSlots;
      if (new_long - cur_long > 256) {
        cout << "invalid add long timer, now_ts: " << now_ts << ", node->expire_ts: " << node->expire_ts
             << ", cur_long: " << cur_long << ", new_long: " << new_long << endl;
        exit(1);
      }
#endif
    }
    node->next = slot->next;
    node->prev = slot;
    slot->next->prev = node;
    slot->next = node;
  }

  template<typename TimerHandler>
  void pollTime(TimerHandler handler) {
    uint32_t ts = getns() >> TsScale;
    if (ts == now_ts) return;
#if EFVITCP_DEBUG
    if ((int)(ts - now_ts) < 0) {
      cout << "time going back!!, ts: " << ts << ", now_ts: " << now_ts << endl;
      exit(1);
    }
#endif
    if (++now_ts % TimerSlots == 0) {
      TimerNode* slot = &timer_slots[1][now_ts / TimerSlots % TimerSlots];
      for (TimerNode* node = slot->next; node != slot;) {
        TimerNode* next = node->next;
#if EFVITCP_DEBUG
        if (node->expire_ts - now_ts > 255) {
          cout << "invalid expire_ts: " << node->expire_ts << ", now_ts: " << now_ts << ", conn_id: " << node->conn_id
               << endl;
          exit(1);
        }
#endif
        addTimer(node->expire_ts - now_ts, node);
        node = next;
      }
      slot->prev = slot->next = slot;
    }
    TimerNode* node = &timer_slots[0][now_ts % TimerSlots];
    if (node->isUnlinked()) return;
    TimerNode dump_slot;
    dump_slot.next = node->next;
    node->next->prev = node->prev->next = &dump_slot;
    node->prev = node->next = node;
    while ((node = dump_slot.next) != &dump_slot) {
      node->unlink();
      if (node->conn_id >= Conf::MaxConnCnt) {
        TimeWaitConn& tw = tws[node->conn_id - Conf::MaxConnCnt];
        delConnEntry(connHashKey(tw.dst_ip, tw.dst_port));
      }
      else
        handler(node);
    }
  }

  static const uint64_t SendBufCnt = Conf::ConnSendBufCnt * Conf::MaxConnCnt + 1; // + 1 for rst
  static const uint64_t TotalBufSize = RecvBufSize * Conf::RecvBufCnt + SendBufSize * SendBufCnt;
  // efvi supports 61632 buffer entries with aligned block of 4KB, 64KB, 1MB or 4MB
  static const uint64_t TotalBufAlign =
    TotalBufSize <= 252444672
      ? (1 << 12)
      : (TotalBufSize <= 4039114752 ? (1 << 16) : (TotalBufSize <= 64625836032 ? (1 << 20) : (1 << 22)));

  uint8_t pkt_buf_blk[TotalBufSize + TotalBufAlign] = {};
  uint8_t* pkt_buf;
  ef_vi vi = {};
  ef_driver_handle dh = -1;
  ef_pd pd = {};
  ef_memreg memreg = {};
  ef_filter_cookie filter_cookie = {};
  bool use_ctpio = false;
  uint8_t local_mac[6];
  uint32_t local_ip;
  uint32_t receive_prefix_len;
  CSum rst_ipsum;
  CSum rst_tcpsum;
  uint32_t now_ts;
  uint32_t conn_cnt;
  uint32_t conns[Conf::MaxConnCnt];
  uint32_t tw_cnt;
  uint32_t tw_ids[Conf::MaxTimeWaitConnCnt];
  TimeWaitConn tws[Conf::MaxTimeWaitConnCnt];
  uint64_t tbl_mask;
  ConnHashEntry conn_tbl[TotalTableSize];
  TimerNode timer_slots[2][TimerSlots]; // a 2-level time wheel accommodating timeout in 65 secs
};

} // namespace efvitcp
