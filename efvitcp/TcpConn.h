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
#include "Core.h"

namespace efvitcp {

template<typename Conf>
class TcpConn : public Conf::UserData
{
public:
  // typename Conf::UserData user_data;

  uint32_t getConnId() { return conn_id; }

  void getPeername(struct sockaddr_in& addr) {
    SendBuf* buf = getSendBuf(0);
    addr.sin_addr.s_addr = buf->ip_hdr.dst_ip;
    addr.sin_port = buf->tcp_hdr.dst_port;
  }

  bool isEstablished() { return established; }

  bool isClosed() { return fin_received && !established; }

  inline uint32_t getSendable() {
    if (fin_sent) return 0;
    return (send_una + Conf::ConnSendBufCnt - 1 - data_next) * smss - data_next_size;
  }

  inline uint32_t getImmediatelySendable() {
    if (data_next != send_next) return 0;
    uint32_t wnd_sendable = std::max(0, (int)(both_wnd_seq - getSendSeq(send_next) - data_next_size));
    return std::min(wnd_sendable, getSendable());
  }

  uint32_t send(const void* data, uint32_t size, bool more = false) {
    if (fin_sent) return 0;
    return sendPartial(data, size, !more);
  }

  uint32_t sendv(const iovec* iov, int iovcnt) {
    if (fin_sent) return 0;
    uint32_t total_send_size = 0;
    for (int i = 0; i < iovcnt; i++) {
      total_send_size += sendPartial(iov[i].iov_base, iov[i].iov_len, i + 1 == iovcnt);
    }
    return total_send_size;
  }

  void sendFin() {
    if (fin_sent) return;
    if (send_una + Conf::ConnSendBufCnt - 1 == data_next) {
      close();
      return;
    }
    fin_sent = 1;
    SendBuf* next = getSendBuf(data_next);
    data_sum.add(ntohs(1)); // fin flag
    data_next_size++;
    if (data_next == send_next && data_next_size == 1) {
      next->tcp_hdr.fin = 1;
      sendBuf(0, data_sum, next);
      advanceNext(1);
      advanceData();
    }
  }

  void setUserTimer(uint32_t timer_id, uint32_t duration) {
    TimerNode& timer = timers[2 + timer_id];
    timer.unlink();
    if (duration) core->addTimer(duration, &timer);
  }

  void close() {
    if (isClosed()) return;
    SendBuf* ack = getAckBuf();
    if (established && ack) {
      ack->tcp_hdr.seq_num = getSendBuf(send_next)->tcp_hdr.seq_num;
      ack->tcp_hdr.rst = 1;
      sendBuf(0, ntohs(4), ack); // rst flag
    }
    onClose();
  }

#ifdef EFVITCP_DEBUG
  void dump(const char* note) {
    SendBuf* buf = getSendBuf(0);
    cout << note << ": conn_id: " << conn_id << ", key: " << connHashKey(buf->ip_hdr.dst_ip, buf->tcp_hdr.dst_port)
         << ", established: " << established << ", fin_sent: " << fin_sent << ", fin_received: " << fin_received
         << ", cwnd: " << cwnd << ", ssthresh: " << ssthresh << ", cong_av: " << cong_av << ", fast_re: " << fast_re
         << ", dup_ack_cnt: " << dup_ack_cnt << ", send_una: " << send_una << ", send_next: " << send_next
         << ", data_next: " << data_next << ", data_next_size: " << data_next_size
         << ", ack_seq: " << (recv_buf_seq + segs[0].second)
         << ", ack_end_seq: " << (recv_buf_seq + segs[recv_seg_cnt - 1].second)
         << ", send_next_seq: " << getSendSeq(send_next) << ", both_wnd_seq: " << both_wnd_seq << ", rto: " << rto
         << ", srtt: " << srtt << ", rttvar: " << rttvar << ", recent_ts: " << recent_ts
         << ", in_recover: " << in_recover << ", recover: " << recover << ", retries: " << retries
         << ", src port: " << htons(buf->tcp_hdr.src_port) << ", dst port: " << htons(buf->tcp_hdr.dst_port) << endl;
  }

  void shortDump(const char* note = "") {
    SendBuf* buf = getSendBuf(0);
    cout << note << ", src port: " << htons(buf->tcp_hdr.src_port) << ", dst port: " << htons(buf->tcp_hdr.dst_port)
         << endl;
  }
#endif

private:
  TcpConn()
    : established(0)
    , fin_received(1) {}

  template<typename C>
  friend class TcpClient;

  template<typename C>
  friend class TcpServer;

  void init(Core<Conf>* core_, uint8_t* send_buf_, uint32_t conn_id_) {
    core = core_;
    send_buf = send_buf_;
    conn_id = conn_id_;
    for (auto& timer : timers) {
      timer.conn_id = conn_id;
    }
  }

  void reset(uint16_t src_port_be, uint8_t* dst_mac, uint32_t dst_ip, uint16_t dst_port_be) {
    flags = 0;
    recv_buf_seq = segs[0].first = segs[0].second = send_una = send_next = data_next_size = dup_ack_cnt = 0;
    data_next = 1;
    smss = 536;
    recv_seg_cnt = 1;
    data_sum = 0;
    SendBuf* syn = getSendBuf(0);
    memcpy(syn->eth_hdr.dst_mac, dst_mac, 6);
    syn->ip_hdr.dst_ip = dst_ip;
    syn->ip_hdr.tot_len = 0;
    syn->ip_hdr.checksum = 0;
    ipsum = 0;
    ipsum.add<sizeof(IpHeader)>(&syn->ip_hdr);
    tcpsum = 0;
    tcpsum.add(syn->ip_hdr.src_ip);
    tcpsum.add(syn->ip_hdr.dst_ip);
    tcpsum.add(ntohs(0x6)); // [zero protocol]
    tcpsum.setVar<false>(syn->tcp_hdr.src_port, src_port_be);
    tcpsum.setVar<false>(syn->tcp_hdr.dst_port, dst_port_be);
    send_wnd_seq = genISN(core->local_ip, src_port_be, dst_ip, dst_port_be); // keep send_win=0 before established
    syn->tcp_hdr.seq_num = htonl(send_wnd_seq);
    if (Conf::WindowScaleOption) {
      has_ws = 1;
      recv_wnd_shift = std::min(14, std::max(0, getMSB(Conf::ConnRecvBufSize) - 16));
      send_wnd_shift = 0;
    }
    if (Conf::TimestampOption) {
      has_ts = 1;
      recent_ts = 0;
      uint8_t* opt = (uint8_t*)(syn + 1);
      opt[0] = 1; // nop
      opt[1] = 1; // nop
      opt[2] = 8;
      opt[3] = 10;
      tcpsum.add<4>(opt);
    }
  }

  void updateBothWndSeq() {
    if (Conf::CongestionControlAlgo == 0) {
      both_wnd_seq = send_wnd_seq;
      return;
    }
    uint32_t cwnd_seq = getSendSeq(send_una) + cwnd;
    both_wnd_seq = (int)(cwnd_seq - send_wnd_seq) < 0 ? cwnd_seq : send_wnd_seq;
  }

  void sendSyn() {
    rto = 1000;
    SendBuf* syn = getSendBuf(send_next);
    CSum sum = 0;
    syn->tcp_hdr.offset_flags = 0;
    syn->tcp_hdr.syn = 1;
    syn->tcp_hdr.ack = pending_ack;
    uint8_t* opt = (uint8_t*)(syn + 1);
    if (hasTs()) opt += 12;
    uint8_t* additional_opt = opt;

    // add MSS
    opt[0] = 2;
    opt[1] = 4;
    *(uint16_t*)&opt[2] = htons(RecvMSS);
    sum.add<4>(opt);
    opt += 4;

    if (Conf::WindowScaleOption && has_ws) {
      opt[0] = 1; // nop
      opt[1] = 3;
      opt[2] = 3;
      opt[3] = recv_wnd_shift;
      sum.add<4>(opt);
      opt += 4;
    }

    uint16_t opt_len = opt - (uint8_t*)(syn + 1);
    syn->tcp_hdr.data_offset = (opt_len + 20) >> 2;
    sum.add(syn->tcp_hdr.offset_flags);
    sendBuf(opt - additional_opt, sum, syn);
    advanceNext(1);
  }

  // return remain
  uint32_t sendPartial(const void* d, uint32_t size, bool last) {
    const uint8_t* data = (const uint8_t*)d;
    while (size && send_una + Conf::ConnSendBufCnt - 1 != data_next) {
      SendBuf* next = getSendBuf(data_next);
      uint32_t size_to_append = std::min(smss - data_next_size, size);
      data_sum.add(copyAndSum((uint8_t*)(next + 1) + (hasTs() ? 12 : 0) + data_next_size, data, size_to_append));
      data += size_to_append;
      size -= size_to_append;
      data_next_size += size_to_append;
      bool can_send =
        data_next == send_next && (int)(ntohl(next->tcp_hdr.seq_num) + data_next_size - both_wnd_seq) <= 0;
      if (data_next_size == smss || (last && can_send)) {
        if (can_send) {
          sendBuf(data_next_size, data_sum, next);
          advanceNext(data_next_size);
        }
        else
          next->tcp_hdr.ack_num = data_sum.sum;
        advanceData();
      }
    }
    return data - (const uint8_t*)d;
  }

  CSum copyAndSum(uint8_t* dst, const uint8_t* src, uint32_t size) {
    constexpr bool IsLittle = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
    uint64_t sum = 0;
    if ((uint64_t)dst & 1) {
      uint8_t n = *src++;
      *dst++ = n;
      sum += n << (IsLittle ? 8 : 0);
      size--;
    }
    while (size >= 8) {
      uint64_t n = *(const uint64_t*)src;
      *(uint64_t*)dst = n;
      sum += n >> 32;
      sum += n & 0xffffffff;
      src += 8;
      dst += 8;
      size -= 8;
    }
    if (size >= 4) {
      uint32_t n = *(const uint32_t*)src;
      *(uint32_t*)dst = n;
      sum += n;
      src += 4;
      dst += 4;
      size -= 4;
    }
    if (size >= 2) {
      uint16_t n = *(const uint16_t*)src;
      *(uint16_t*)dst = n;
      sum += n;
      src += 2;
      dst += 2;
      size -= 2;
    }
    if (size) {
      uint8_t n = *src;
      *dst = n;
      sum += n << (IsLittle ? 0 : 8);
    }
    CSum ret = (uint32_t)(sum >> 32);
    ret.add((uint32_t)sum);
    return ret;
  }

  uint16_t getRecvWindowSize() {
    uint32_t window_size = Conf::ConnRecvBufSize - segs[0].second;
    if (Conf::WindowScaleOption && established)
      window_size >>= recv_wnd_shift;
    else
      window_size = std::min(65535u, window_size);
    return htons(window_size);
  }

  void sendBuf(uint32_t opt_data_size, CSum sum, SendBuf* buf) {
    sum.add(buf->tcp_hdr.seq_num);
    sum.setVar<false>(buf->tcp_hdr.ack_num, updateLastAck());
    sum.setVar<false>(buf->tcp_hdr.window_size, getRecvWindowSize());
    if (hasTs()) {
      uint8_t* opt = (uint8_t*)(buf + 1);
      sum.setVar<false>(*(uint32_t*)(opt + 4), htonl(core->now_ts));
      sum.setVar<false>(*(uint32_t*)(opt + 8), htonl(recent_ts));
      opt_data_size += 12;
    }
    sum.add(tcpsum);
    buf->setOptDataLen(opt_data_size, ipsum, sum);
    core->send(buf);
  }

  void advanceNext(uint32_t inc_seq) {
    SendBuf* next = getSendBuf(send_next);
    next->send_ts = core->now_ts;
    if (send_next == send_una) startResendTimer();
    uint32_t seq = ntohl(next->tcp_hdr.seq_num);
    getSendBuf(++send_next)->tcp_hdr.seq_num = htonl(seq + inc_seq);
  }

  void advanceData() {
    data_next++;
    data_sum = 0;
    data_next_size = 0;
  }

  void startResendTimer() { core->addTimer(rto, &timers[0]); }

  void sendAck(bool immediate) {
    SendBuf* ack = getAckBuf();
    if ((Conf::DelayedAckMS == 0 || immediate) && ack) {
      ack->tcp_hdr.seq_num = getSendBuf(send_next)->tcp_hdr.seq_num;
      sendBuf(0, 0, ack);
      return;
    }
    if (timers[1].isUnlinked()) core->addTimer(std::max(1u, Conf::DelayedAckMS), &timers[1]);
  }

  void onSyn(IpHeader* ip_hdr) {
    TcpHeader* tcp_hdr = (TcpHeader*)(ip_hdr + 1);
    uint8_t* opt = (uint8_t*)(tcp_hdr + 1);
    uint8_t* data = (uint8_t*)tcp_hdr + (tcp_hdr->data_offset << 2);
    recv_buf_seq = ntohl(tcp_hdr->seq_num) + 1;
    pending_ack = 1;
    has_ws = has_ts = 0;
    while (opt < data) {
      uint8_t kind = *opt++;
      if (kind <= 1) continue;
      uint8_t length = *opt++;
      switch (kind) {
        case 2: {
          if (length == 4) {
            smss = std::min((uint16_t)(core->SendMTU - 40), ntohs(*(uint16_t*)opt));
#ifdef EFVITCP_DEBUG
            cout << "smss: " << smss << endl;
#endif
          }
          break;
        }
        case 3: {
          if (Conf::WindowScaleOption && length == 3) {
            has_ws = 1;
            send_wnd_shift = std::min((uint8_t)14, *opt);
#ifdef EFVITCP_DEBUG
            cout << "send win shift: " << (int)send_wnd_shift << endl;
#endif
          }
          break;
        }
        case 8: {
          if (Conf::TimestampOption && length == 10) {
            has_ts = 1;
            recent_ts = ntohl(*(uint32_t*)opt);
#ifdef EFVITCP_DEBUG
            cout << "has_ts" << endl;
#endif
          }
          break;
        }
      }
      if (length > 2) opt += length - 2;
    }
    if (!has_ws) recv_wnd_shift = 0;
    if (Conf::TimestampOption) {
      if (!has_ts)
        tcpsum.sub(*(uint32_t*)(getSendBuf(send_una) + 1)); // sub ts opt header
      else
        smss -= 12;
    }
  }

  bool hasTs() { return Conf::TimestampOption && has_ts; }

  uint32_t getSendSeq(uint32_t idx) { return ntohl(getSendBuf(idx)->tcp_hdr.seq_num); }

  uint32_t getRMSS() { return RecvMSS - (hasTs() ? 12 : 0); }

  void updateRto() { rto = std::max(Conf::MinRtoMS, srtt + std::max(1u, rttvar << 2)); }

  template<typename EventHandler>
  void onEstablished(EventHandler& handler, IpHeader* ip_hdr) {
    TcpHeader* tcp_hdr = (TcpHeader*)(ip_hdr + 1);
    uint32_t window_size = ntohs(tcp_hdr->window_size);
    if (Conf::WindowScaleOption && !tcp_hdr->syn) window_size <<= send_wnd_shift;
    send_wnd_seq = ntohl(tcp_hdr->ack_num) + window_size;
    if (Conf::CongestionControlAlgo) {
      cwnd = smss * (smss > 1095 ? 3 : 4);
      ssthresh = 1 << 30;
    }
    updateBothWndSeq();
    established = 1;
    SendBuf* syn = getSendBuf(send_una);
    syn->tcp_hdr.offset_flags = 0;
    syn->tcp_hdr.data_offset = hasTs() ? 8 : 5;
    syn->tcp_hdr.psh = 1;
    syn->tcp_hdr.ack = 1;
    tcpsum.add(syn->tcp_hdr.offset_flags);

    srtt = std::max(1u, core->now_ts - syn->send_ts);
    rttvar = srtt >> 1;
    updateRto();

    for (uint32_t i = send_next; i < Conf::ConnSendBufCnt; i++) {
      SendBuf* buf = getSendBuf(i);
      memcpy(buf->eth_hdr.dst_mac, syn->eth_hdr.dst_mac, 6);
      buf->ip_hdr.dst_ip = syn->ip_hdr.dst_ip;
      buf->tcp_hdr.src_port = syn->tcp_hdr.src_port;
      buf->tcp_hdr.dst_port = syn->tcp_hdr.dst_port;
      buf->tcp_hdr.offset_flags = syn->tcp_hdr.offset_flags;
      if (hasTs()) memcpy(buf + 1, syn + 1, 4); // copy ts opt header
    }

    handler.onConnectionEstablished(*this);
#ifdef EFVITCP_DEBUG
    dump("onEstab");
#endif
  }

  void onClose(bool enterTW = false) {
    if (isClosed()) return;
    fin_sent = fin_received = 1;
    established = 0;
    for (auto& timer : timers) timer.unlink();
    SendBuf* buf = getSendBuf(0);
    uint64_t key = connHashKey(buf->ip_hdr.dst_ip, buf->tcp_hdr.dst_port);
    if (enterTW) {
      buf->tcp_hdr.seq_num = getSendBuf(send_next)->tcp_hdr.seq_num;
      buf->tcp_hdr.ack_num = htonl(recv_buf_seq + segs[0].second);
      core->enterTW(key, buf, has_ts, recent_ts);
    }
    else
      core->delConnEntry(key);
  }

  template<typename EventHandler>
  void onPack(EventHandler& handler, IpHeader* ip_hdr) {
    TcpHeader* tcp_hdr = (TcpHeader*)(ip_hdr + 1);
    uint8_t* opt = (uint8_t*)(tcp_hdr + 1);
    uint8_t* data = (uint8_t*)tcp_hdr + (tcp_hdr->data_offset << 2);
    uint8_t* data_end = (uint8_t*)ip_hdr + std::min((int)ntohs(ip_hdr->tot_len), 1500);
    uint32_t seq_num = ntohl(tcp_hdr->seq_num) + tcp_hdr->syn;
    bool got_ts = false;
    uint32_t tsval, tsecr = 0;
    if (hasTs()) {
      if (opt + 12 <= data && *(uint32_t*)opt == ntohl(0x0101080a)) {
        got_ts = true;
        tsval = ntohl(*(uint32_t*)(opt + 4));
        tsecr = ntohl(*(uint32_t*)(opt + 8));
      }
      else {
        while (opt + 10 <= data) {
          uint8_t kind = *opt++;
          if (kind <= 1) continue;
          uint8_t length = *opt++;
          if (kind == 8 && length == 10) {
            got_ts = true;
            tsval = ntohl(*(uint32_t*)opt);
            tsecr = ntohl(*(uint32_t*)(opt + 4));
            break;
          }
          if (length > 2) opt += length - 2;
        }
      }
      if (got_ts && (int)(seq_num - last_ack_seq) <= 0 && (int)(tsval - recent_ts) >= 0) recent_ts = tsval;
    }
#ifdef EFVITCP_DEBUG
    SendBuf* buf = getSendBuf(0);
    uint64_t key = connHashKey(buf->ip_hdr.dst_ip, buf->tcp_hdr.dst_port);
    uint64_t pack_key = connHashKey(ip_hdr->src_ip, tcp_hdr->src_port);
    if (buf->tcp_hdr.dst_port != tcp_hdr->src_port || key != pack_key) {
      cout << "invalid key, buf->tcp_hdr.dst_port: " << htons(buf->tcp_hdr.dst_port)
           << ", tcp_hdr->src_port: " << htons(tcp_hdr->src_port) << ", key: " << key << ", pack_key: " << pack_key
           << endl;
      dumpPack(ip_hdr);
      dump("");
      exit(1);
    }
    if (data_end < data) {
      cout << "nagetive data size!!! tcp_hdr->data_offset: " << tcp_hdr->data_offset
           << ", ntohs(ip_hdr->tot_len): " << ntohs(ip_hdr->tot_len) << endl;
      exit(1);
    }
#endif
    // first check sequence number
    uint32_t data_buf_loc = seq_num - recv_buf_seq;
    uint32_t data_end_loc = data_buf_loc + (data_end - data) + tcp_hdr->fin;
    if ((((int)(data_buf_loc - segs[0].second) < 0 || (int)(data_buf_loc - Conf::ConnRecvBufSize) >= 0) &&
         ((int)(data_end_loc - segs[0].second) <= 0 || (int)(data_end_loc - Conf::ConnRecvBufSize) > 0)) ||
        (Conf::TimestampOption && got_ts && (int)(tsval - recent_ts) < 0)) {
      if (!tcp_hdr->rst) sendAck(false);
      return;
    }

    // second check the RST bit
    if (tcp_hdr->rst) {
      if (!fin_sent || !fin_received) handler.onConnectionReset(*this);
      onClose();
      return;
    }

    // fifth check the ACK field
    if (!tcp_hdr->ack) return;
    uint32_t ack_num = ntohl(tcp_hdr->ack_num);
#ifdef EFVITCP_DEBUG
    if ((int)(ack_num - getSendSeq(send_next)) > 0) {
      cout << "abnomal ack_num!!!!, ack_num: " << ack_num << endl;
      dumpPack(ip_hdr);
      dump("");
      exit(1);
    }
#endif
    bool window_updated = false;
    if (!fin_sent) {
      uint32_t window_size = ntohs(tcp_hdr->window_size);
      if (Conf::WindowScaleOption && !tcp_hdr->syn) window_size <<= send_wnd_shift;
      uint32_t new_send_win_seq = ack_num + window_size;
      if ((int)(new_send_win_seq - send_wnd_seq) > 0) {
        window_updated = true;
        send_wnd_seq = new_send_win_seq;
      }
    }
    uint32_t old_una = send_una;
    while (send_una != send_next && (int)(ack_num - getSendSeq(send_una + 1)) >= 0) send_una++;
    uint32_t tmp_cwnd = 0;
    if (old_una != send_una) { // acked new data
      dup_ack_cnt = 0;
      retries = 0;
      SendBuf* acked = getSendBuf(old_una);
      uint32_t newly_acked_size = getSendSeq(send_una) - ntohl(acked->tcp_hdr.seq_num);
      uint32_t send_ts = (Conf::TimestampOption && got_ts) ? tsecr : acked->send_ts;
      int rtt = std::max(1, (int)(core->now_ts - send_ts));
      rttvar -= (int)(rttvar - std::abs(rtt - (int)srtt)) >> 2;
      srtt -= (int)(srtt - rtt) >> 3;
#ifdef EFVITCP_DEBUG
      if ((int)rttvar < 0 || (int)srtt < 0) {
        cout << "abnormal rtt: " << rtt << ", srtt: " << (int)srtt << ", rttvar: " << (int)rttvar << endl;
        dump("abnormal rtt");
        exit(1);
      }
#endif
      updateRto();
      timers[0].unlink();
      if (send_una != send_next) startResendTimer();
      if (in_recover) {
        if ((int)(send_una - recover) < 0) { // partial ack
          resendUna(false);
        }
        else { // full ack
          if ((int)(send_una - recover) > 0) in_recover = 0;
          fast_re = 0;
          if (Conf::CongestionControlAlgo) cwnd = ssthresh;
        }
      }
      else if (Conf::CongestionControlAlgo) {
        if (cwnd < ssthresh) //  slow start
          cwnd += std::min(smss, newly_acked_size);
        else if (cwnd < (1 << 30)) { // cong av
#ifdef EFVITCP_DEBUG
          if (cwnd > (1 << 30) + 500000000) {
            dump("abnomal cwnd");
            exit(1);
          }
#endif
          if (!cong_av) {
            cong_av_ts = core->now_ts;
            cong_av = 1;
          }
          uint64_t duration = core->now_ts - cong_av_ts;
          if (Conf::CongestionControlAlgo == 1) cwnd = std::min(1ul << 30, ssthresh + smss * duration / srtt);
          if (Conf::CongestionControlAlgo == 2) {
            // W_est(t) = W_max*0.75 + [3*(1-0.75)/(1+0.75)] * (t/RTT)
            uint64_t w_est = ssthresh + (3 * smss * duration) / (7 * srtt);
            int64_t t_k = (duration >> 10) - cubic_k;
            // W_cubic(t) = 0.25 *(t-K)^3 + W_max
            uint64_t w_cubic = (((t_k * t_k * t_k) >> 2) + cubic_wmax) * smss;
            if (w_est > w_cubic)
              cwnd = std::min(1ul << 30, w_est);
            else if (w_cubic > cwnd)
              cwnd += smss * (w_cubic - cwnd) / cwnd;
          }
        }
      }
    }
    else if (send_una != send_next && !window_updated && data_buf_loc == data_end_loc) { // duplicate ack
      if (++dup_ack_cnt == 3 && !in_recover) {
        fast_re = in_recover = 1;
        recover = send_next;
        resendUna(true);
        if (Conf::CongestionControlAlgo) cong_av = 0;
        if (Conf::CongestionControlAlgo == 1) cwnd = ssthresh + 3 * smss;
        if (Conf::CongestionControlAlgo == 2) {
          cubic_wmax = cwnd / smss;
          cwnd = ssthresh;
          // K = cubic_root(W_max*(1-0.75)/0.25)
          uint32_t l = 1, r = 124;
          while (l <= r) {
            uint32_t m = (l + r) >> 1;
            if (m * m * m <= cubic_wmax)
              l = m + 1;
            else
              r = m - 1;
          }
          cubic_k = r;
        }
      }
      else if (Conf::CongestionControlAlgo) {
        if (fast_re)
          cwnd += smss;
        else { // should we check if dup_ack_cnt < 3?
          tmp_cwnd = dup_ack_cnt * smss;
          cwnd += tmp_cwnd;
        }
      }
    }
    updateBothWndSeq();
    // send unsent data if possible
    while (uint32_t data_size = (data_next == send_next ? data_next_size : smss)) {
      SendBuf* next = getSendBuf(send_next);
      if ((int)(ntohl(next->tcp_hdr.seq_num) + data_size - both_wnd_seq) > 0) break;
      if (data_next == send_next) {
        next->tcp_hdr.fin = fin_sent;
        sendBuf(data_size - next->tcp_hdr.fin, data_sum, next);
        advanceData();
      }
      else
        sendBuf(data_size, next->tcp_hdr.ack_num, next);
      advanceNext(data_size);
    }
    if (Conf::CongestionControlAlgo) {
      cwnd -= tmp_cwnd;
      updateBothWndSeq();
    }
    if (old_una != send_una) handler.onMoreSendable(*this);

    // seventh, process the segment text
    int seq_expect_diff = data_buf_loc - segs[0].second;
    if (seq_expect_diff < 0) { // skip old data
      data_buf_loc -= seq_expect_diff;
      data -= seq_expect_diff;
    }
    bool immediate_ack = false;
    if (!fin_received) {
      data_end_loc -= tcp_hdr->fin;
      bool fin_available = tcp_hdr->fin;
      if (data_end_loc > Conf::ConnRecvBufSize) {
        data_end_loc = Conf::ConnRecvBufSize;
        fin_available = false;
      }
      int data_size = data_end_loc - data_buf_loc; // data size in window
      if (data_size > 0) {                         // seg has data in recv window
        pending_ack = 1;
        if (recv_seg_cnt > 1) immediate_ack = true; // there was a hole
        uint32_t i = 0;
        while (i < recv_seg_cnt && segs[i].second < data_buf_loc) i++;
        uint32_t j = i;
        while (j < recv_seg_cnt && segs[j].first <= data_end_loc) j++;
        if (i == j) { // insert new seg
          if (i < MaxRecvSegs) {
            immediate_ack = true; // create a new hole
            if (recv_seg_cnt == MaxRecvSegs) recv_seg_cnt--;
            for (j = recv_seg_cnt; j > i; j--) {
              segs[j] = segs[j - 1];
            }
            segs[i].first = data_buf_loc;
            segs[i].second = data_end_loc;
            recv_seg_cnt++;
          }
        }
        else { // merge segs
          segs[i].first = std::min(segs[i].first, data_buf_loc);
          segs[i].second = std::max(segs[j - 1].second, data_end_loc);
          uint32_t i2 = i + 1;
          if (i2 < j) {
            for (; j < recv_seg_cnt; i2++, j++) {
              segs[i2] = segs[j];
            }
            recv_seg_cnt = i2;
          }
        }
        if (segs[0].second != data_end_loc) fin_available = false;

        if (((segs[0].first - data_buf_loc) | (segs[0].second - data_end_loc)) == 0) {
          uint32_t remaining = handler.onData(*this, data, data_size); // zero copy
          segs[0].first = segs[0].second - remaining;
          memcpy(recv_buf + segs[0].first, data + data_size - remaining, remaining);
        }
        else {
          memcpy(recv_buf + data_buf_loc, data, data_size);
          if (i == 0) {
            uint32_t remaining = handler.onData(*this, recv_buf + segs[0].first, segs[0].second - segs[0].first);
            segs[0].first = segs[0].second - remaining;
          }
        }
        if (segs[0].first >= getRMSS()) { // avoid SWS on receiver side: to advance right window by at least RMSS a time
          if (segs[0].first == segs[recv_seg_cnt - 1].second) {
            recv_buf_seq += segs[0].first;
            segs[0].first = segs[0].second = 0;
          }
          else if (segs[0].first >= Conf::ConnRecvBufSize / 2) {
            memcpy(recv_buf, recv_buf + segs[0].first, segs[recv_seg_cnt - 1].second - segs[0].first);
            uint32_t diff = segs[0].first;
            recv_buf_seq += diff;
            for (uint32_t i = 0; i < recv_seg_cnt; i++) {
              segs[i].first -= diff;
              segs[i].second -= diff;
            }
          }
        }
        if (segs[0].second == Conf::ConnRecvBufSize) { // oops, recv buf is full of unconsumed data, can't proceed
          handler.onConnectionReset(*this);
          close();
          return;
        }
      }
      else if (data_buf_loc != segs[0].second)
        fin_available = false;

      // eighth, check the FIN bit
      if (fin_available) {
#ifdef EFVITCP_DEBUG
        if (fin_sent && send_una != data_next) {
          dump("abnormal unack");
          exit(1);
        }
#endif
        pending_ack = fin_received = 1;
        immediate_ack = true;
        segs[0].second++; // for acking the fin
        handler.onFin(*this, recv_buf + segs[0].first, segs[0].second - segs[0].first - 1);
      }
    }
    if (pending_ack) sendAck(immediate_ack || recv_buf_seq + segs[0].second - last_ack_seq >= 2 * getRMSS());
    if (fin_sent && fin_received && established && (send_una == data_next)) {
      handler.onConnectionClosed(*this);
      onClose(true);
    }
  }

  void resendUna(bool reset_ssthresh) {
    retries++;
    SendBuf* una = getSendBuf(send_una); // assume una->avail == true
#ifdef EFVITCP_DEBUG
    core->checksum(&una->ip_hdr);
#endif
    CSum sum = (uint16_t)~una->tcp_hdr.checksum;
    sum.setVar<true>(una->tcp_hdr.ack_num, updateLastAck());
    sum.setVar<true>(una->tcp_hdr.window_size, getRecvWindowSize());
    if (hasTs()) {
      uint8_t* opt = (uint8_t*)(una + 1);
      sum.setVar<true>(*(uint32_t*)(opt + 4), htonl(core->now_ts));
      sum.setVar<true>(*(uint32_t*)(opt + 8), htonl(recent_ts));
    }
    una->tcp_hdr.checksum = sum.fold();
    core->send(una);
    una->send_ts = core->now_ts;
    if (reset_ssthresh) {
      if (Conf::CongestionControlAlgo == 1)
        ssthresh = std::max((getSendSeq(send_next) - ntohl(una->tcp_hdr.seq_num)) >> 1, smss << 1);
      if (Conf::CongestionControlAlgo == 2) ssthresh = std::max(cwnd - (cwnd >> 2), smss << 1);
    }
  }

  template<typename EventHandler>
  void onTimer(EventHandler& handler, TimerNode* node) {
    // node is already unlinked
    uint32_t timer_id = node - timers;
    switch (timer_id) {
      case 0: { // resend
        if (retries >= std::min(31u, (!established ? Conf::SynRetries + 0 : Conf::TcpRetries))) {
          handler.onConnectionTimeout(*this);
          close();
          break;
        }
#ifdef EFVITCP_DEBUG
        if (retries >= 5) {
          dump("abnormal retries");
          if (retries >= 6) {
            cout << "exit" << endl;
            exit(1);
          }
        }
#endif
        resendUna(retries == 0);
        if (Conf::CongestionControlAlgo) {
          cwnd = smss;
          updateBothWndSeq();
          cong_av = 0;
          if (Conf::CongestionControlAlgo == 2) {
            cubic_k = 0;
            cubic_wmax = ssthresh / smss;
          }
        }
        fast_re = 0;
        in_recover = 1;
        recover = send_next;
        rto <<= 1;
        if (rto > Conf::MaxRtoMS) rto = Conf::MaxRtoMS;
        startResendTimer();
        break;
      }
      case 1: { // delayed ack
        sendAck(true);
        break;
      }
      default: {
        handler.onUserTimeout(*this, timer_id - 2);
        break;
      }
    }
  }

  uint32_t updateLastAck() {
    timers[1].unlink();
    pending_ack = 0;
    last_ack_seq = recv_buf_seq + segs[0].second;
    return htonl(last_ack_seq);
  }

  SendBuf* getAckBuf() {
    for (uint32_t idx = send_una + Conf::ConnSendBufCnt - 1; idx != data_next - 1; idx--) {
      SendBuf* buf = getSendBuf(idx);
      if (buf->avail) {
        buf->tcp_hdr.fin = 0;
        return buf;
      }
    }
    return nullptr;
  }

  SendBuf* getSendBuf(uint32_t idx) {
    static_assert(Conf::ConnSendBufCnt && !(Conf::ConnSendBufCnt & (Conf::ConnSendBufCnt - 1)),
                  "ConnSendBufCnt must be a power of 2");
    return (SendBuf*)(send_buf + Core<Conf>::SendBufSize * (idx % Conf::ConnSendBufCnt));
  }

  uint32_t genISN(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port) {
    return connHashKey(dst_ip, dst_port) + core->now_ts;
  }

  Core<Conf>* core;
  uint8_t* send_buf;
  TimerNode timers[2 + Conf::UserTimerCnt];
  uint32_t send_una;
  uint32_t send_next;
  uint32_t recover;
  uint32_t data_next;
  uint32_t data_next_size;
  CSum data_sum;
  uint32_t conn_id;
  uint32_t rto;
  uint32_t srtt;
  uint32_t rttvar;
  union
  {
    struct
    {
      uint16_t established : 1, fin_sent : 1, fin_received : 1, pending_ack : 1, has_ws : 1, has_ts : 1, fast_re : 1,
        cong_av : 1, in_recover : 1, retries : 5;
    };
    uint16_t flags;
  };
  uint8_t recv_wnd_shift;
  uint8_t send_wnd_shift;
  uint32_t smss;
  uint32_t send_wnd_seq;
  uint32_t cwnd;
  uint32_t both_wnd_seq;
  uint32_t ssthresh;
  uint32_t last_ack_seq;
  uint32_t recent_ts;
  uint32_t cong_av_ts;
  uint32_t dup_ack_cnt;
  uint32_t cubic_wmax;
  uint32_t cubic_k;
  CSum ipsum;
  CSum tcpsum; // cache sum for next
  static const uint32_t MaxRecvSegs = 5;
  uint32_t recv_seg_cnt;
  uint32_t recv_buf_seq;
  std::pair<uint32_t, uint32_t> segs[MaxRecvSegs];
  uint8_t recv_buf[Conf::ConnRecvBufSize];
};

} // namespace efvitcp
