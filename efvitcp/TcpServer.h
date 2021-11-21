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
#include "TcpConn.h"

namespace efvitcp {
template<typename Conf>
class TcpServer
{
public:
  using Conn = TcpConn<Conf>;

  ~TcpServer() { close(); }

  const char* init(const char* interface) {
    const char* err = core.init(interface);
    if (err) return err;
    for (uint32_t i = 0; i < Conf::MaxConnCnt; i++) {
      conns[i].init(&core, (uint8_t*)core.getSendBuf(i * Conf::ConnSendBufCnt), i);
    }
    return nullptr;
  }

  void close() {
    for (auto& conn : conns) {
      conn.close();
    }
    core.delFilter();
  }

  uint32_t getConnCnt() { return core.conn_cnt; }

  template<typename Handler>
  void foreachConn(Handler handler) {
    for (auto& conn : conns) {
      if (conn.isEstablished()) handler(conn);
    }
  }

  const char* listen(uint16_t server_port) {
    server_port_be = htons(server_port);
    const char* err;
    if ((err = core.setServerFilter(server_port_be))) return err;
    return nullptr;
  }

  template<typename EventHandler>
  void poll(EventHandler& handler) {
    core.pollTime([&](TimerNode* node) {
      Conn& conn = conns[node->conn_id];
      return conn.onTimer(handler, node);
    });

    core.pollNet([&](uint64_t key, ConnHashEntry* entry, EtherHeader* eth_hdr) {
      IpHeader* ip_hdr = (IpHeader*)(eth_hdr + 1);
      TcpHeader* tcp_hdr = (TcpHeader*)(ip_hdr + 1);
      if (entry->key != key) {
        if (tcp_hdr->rst) return;
        if (tcp_hdr->ack || !tcp_hdr->syn || (core.conn_cnt == Conf::MaxConnCnt) ||
            !handler.allowNewConnection(ip_hdr->src_ip, tcp_hdr->src_port)) {
          core.rspRst(eth_hdr);
          return;
        }
        uint32_t conn_id = core.conns[core.conn_cnt++];
        core.addConnEntry(entry, key, conn_id);
        Conn& conn = conns[conn_id];
        conn.reset(server_port_be, eth_hdr->src_mac, ip_hdr->src_ip, tcp_hdr->src_port);
        conn.onSyn(ip_hdr);
        conn.sendSyn();
        return;
      }
      Conn& conn = conns[entry->conn_id];
      if (!conn.established) { // Syn Received
        if (tcp_hdr->syn && !tcp_hdr->ack) {
          conn.resendUna(false);
          return;
        }
        if (tcp_hdr->rst && ntohl(tcp_hdr->seq_num) == conn.recv_buf_seq + conn.segs[0].second) {
          conn.onClose();
          return;
        }
        if (!tcp_hdr->ack) return;
        if (tcp_hdr->ack_num != conn.getSendBuf(conn.send_next)->tcp_hdr.seq_num) {
          core.rspRst(eth_hdr);
          return;
        }
        conn.onEstablished(handler, ip_hdr);
      }
      conn.onPack(handler, ip_hdr);
    });
  }

private:
  Core<Conf> core;
  uint16_t server_port_be;
  Conn conns[Conf::MaxConnCnt];
};

} // namespace efvitcp
