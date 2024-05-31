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
class TcpClient
{
public:
  struct CliConf : public Conf
  {
    static const int MaxConnCnt = 1;
  };
  using Conn = TcpConn<CliConf>;

  ~TcpClient() { close(); }

  const char* init(const char* interface) {
    const char* err = core.init(interface);
    if (err) return err;
    conn.init(&core, (uint8_t*)core.getSendBuf(0), 0);
    return nullptr;
  }

  void close() {
    conn.close();
    core.delFilter();
  }

  Conn& getConn() { return conn; }

  const char* connect(const char* server_ip, uint16_t server_port, uint16_t local_port = 0) {
    if (core.conn_cnt) return "Connection already exists";
    uint32_t remote_ip;
    if (!inet_pton(AF_INET, server_ip, &remote_ip)) return "Invalid server ip";
    uint8_t dst_mac[6];
    const char* err;
    if ((err = getDestMac(server_ip, dst_mac))) return err;
    server_port = htons(server_port);
    local_port = htons(local_port);
    if ((err = core.setClientFilter(local_port, remote_ip, server_port))) return err;
    conn.reset(local_port, dst_mac, remote_ip, server_port);
    conn.sendSyn();
    uint64_t key = connHashKey(remote_ip, server_port);
    core.conn_cnt++;
    core.addConnEntry(core.findConnEntry(key), key, 0);
    return nullptr;
  }

  template<typename EventHandler>
  void poll(EventHandler& handler, int64_t ns = 0) {
    core.pollTime([&](TimerNode* node) { return conn.onTimer(handler, node); }, ns);

    core.pollNet([&](uint64_t key, ConnHashEntry* entry, EtherHeader* eth_hdr) {
      IpHeader* ip_hdr = (IpHeader*)(eth_hdr + 1);
      TcpHeader* tcp_hdr = (TcpHeader*)(ip_hdr + 1);
      if (entry->key != key) {
        core.rspRst(eth_hdr);
        return;
      }
      if (!conn.established) { // Syn Sent
        bool ack_ok = tcp_hdr->ack && tcp_hdr->ack_num == conn.getSendBuf(conn.send_next)->tcp_hdr.seq_num;
        if (!ack_ok) {
          core.rspRst(eth_hdr);
          return;
        }
        if (tcp_hdr->rst) {
          handler.onConnectionRefused();
          conn.onClose();
          return;
        }
        if (!tcp_hdr->syn) return;
        conn.onSyn(ip_hdr);
        conn.onEstablished(handler, ip_hdr);
      }
      conn.onPack(handler, ip_hdr);
    });
  }

private:
  const char* getDestMac(const char* dest_ip, uint8_t* dest_mac) {
#define IPRouteGetCmd "/usr/sbin/ip route get"
#define NetARPFile "/proc/net/arp"
    char buf[1024];
    char gw_ip[64];
    char ip[64];
    bool found = false;
    sprintf(buf, IPRouteGetCmd " %s", dest_ip);
    FILE* pipe = popen(buf, "r");
    if (!pipe) return IPRouteGetCmd " failed";
    while (fgets(buf, sizeof(buf), pipe)) {
      const char* str = buf;
      if (char* pos = strstr(buf, " via ")) {
        str = pos + 5;
      }
      if (sscanf(str, "%s dev %*s src %*s", gw_ip) == 1) {
        found = true;
        break;
      }
    }
    pclose(pipe);
    if (!found) return "Can't find gw by " IPRouteGetCmd;

    FILE* arp_file = fopen(NetARPFile, "r");
    if (!arp_file) return "Can't open " NetARPFile;
    if (!fgets(buf, sizeof(buf), arp_file)) {
      fclose(arp_file);
      return "Invalid file " NetARPFile;
    }
    char hw[64];
    found = false;
    while (2 == fscanf(arp_file, "%63s %*s %*s %63s %*s %*s", ip, hw)) {
      if (!strcmp(ip, gw_ip)) {
        found = true;
        break;
      }
    }
    fclose(arp_file);
    if (!found) return "Can't find dest ip from arp cache, please ping dest ip first";
    bool valid_mac = strlen(hw) == 17;
    auto hexchartoi = [&valid_mac](char c) -> uint8_t {
      if (c >= '0' && c <= '9')
        return c - '0';
      else if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      else if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      else {
        valid_mac = false;
        return 0;
      }
    };
    if (valid_mac) {
      for (int i = 0; i < 6; ++i) {
        dest_mac[i] = hexchartoi(hw[3 * i]) * 16 + hexchartoi(hw[3 * i + 1]);
      }
    }
    if (!valid_mac) return "Invalid dest mac addr";
    return nullptr;
  }

  Core<CliConf> core;
  Conn conn;
};

} // namespace efvitcp
