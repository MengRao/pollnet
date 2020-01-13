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
#include <arpa/inet.h>

template<bool WaitForResend = true, uint32_t BUFSIZE = 1 << 20>
class TcpStream
{
public:
  // use "0.0.0.0" for wildcard ip and 0 for wildcard port
  void initFilter(const char* src_ip, uint16_t src_port, const char* dst_ip, uint16_t dst_port) {
    inet_pton(AF_INET, src_ip, &filter_src_ip);
    inet_pton(AF_INET, dst_ip, &filter_dst_ip);
    filter_src_port = htons(src_port);
    filter_dst_port = htons(dst_port);
  }

  bool filterPacket(const char* data, uint32_t size) {
    EtherHeader& ether_header = *(EtherHeader*)data;
    IpHeader& ip_header = *(IpHeader*)(data + IPHeaderPos);
    TcpHeader& tcp_header = *(TcpHeader*)(data + TcpHeaderPos);

    if (ether_header.etherType != 0x0008) return false;
    if (ip_header.protocol != 6) return false;
    if (filter_src_ip && filter_src_ip != ip_header.ipSrc) return false;
    if (filter_dst_ip && filter_dst_ip != ip_header.ipDst) return false;
    if (filter_src_port && filter_src_port != tcp_header.portSrc) return false;
    if (filter_dst_port && filter_dst_port != tcp_header.portDst) return false;
    return true;
  }

  template<typename Handler>
  bool handlePacket(const char* data, uint32_t size, Handler handler) {
    IpHeader& ip_header = *(IpHeader*)(data + IPHeaderPos);
    TcpHeader& tcp_header = *(TcpHeader*)(data + TcpHeaderPos);

    uint32_t seq = ntohl(tcp_header.sequenceNumber);
    if (tcp_header.synFlag) {
      init_stream = false;
      seq++;
    }
    if (!init_stream) {
      init_stream = true;
      buf_seq = seq;
      n_seg = 1;
      segs[0].first = segs[0].second = 0;
    }

    uint32_t header_len = sizeof(IpHeader) + tcp_header.dataOffset * 4;
    const char* new_data = data + IPHeaderPos + header_len;
    uint32_t new_size = ntohs(ip_header.totalLength) - header_len;
    uint32_t loc = seq - buf_seq; // location of seq in buffer
    uint32_t loc_end = loc + new_size;
    int32_t diff = loc - segs[0].second;
    if (diff < 0) {
      loc -= diff;
      new_data -= diff;
      new_size += diff;
    }
    if ((int32_t)new_size <= 0) return false; // obsolete data
    if (loc_end > BUFSIZE) return false;      // buff full
    if (!WaitForResend && loc > segs[0].second) {
      segs[0].first = segs[0].second = loc;
    }
    uint32_t i = 0;
    while (i < n_seg && segs[i].second < loc) i++;
    uint32_t j = i;
    while (j < n_seg && segs[j].first <= loc_end) j++;

    if (i == j) {                         // insert new seg
      if (n_seg == MAX_SEG) return false; // too many segs
      for (j = n_seg; j > i; j--) {
        segs[j] = segs[j - 1];
      }
      segs[i].first = loc;
      segs[i].second = loc_end;
      n_seg++;
    }
    else { // merge segs
      segs[i].first = std::min(segs[i].first, loc);
      segs[i].second = std::max(segs[j - 1].second, loc_end);
      uint32_t i2 = i + 1;
      if (i2 < j) {
        for (; j < n_seg; i2++, j++) {
          segs[i2] = segs[j];
        }
        n_seg = i2;
      }
    }

    if (segs[0].first == loc && segs[0].second == loc_end) {
      uint32_t remaining = handler(new_data, new_size); // zero copy
      segs[0].first = segs[0].second - remaining;
      if (remaining) {
        uint32_t consumed = new_size - remaining;
        memcpy(recvbuf + segs[0].first, new_data + consumed, remaining);
      }
    }
    else {
      memcpy(recvbuf + loc, new_data, new_size);
      if (i != 0) return false; // no new data for handler
      uint32_t remaining = handler(recvbuf + segs[0].first, segs[0].second - segs[0].first);
      segs[0].first = segs[0].second - remaining;
    }

    if (segs[0].first >= BUFSIZE / 2) {
      uint32_t total_size = segs[n_seg - 1].second - segs[0].first;
      if (total_size) {
        memcpy(recvbuf, recvbuf + segs[0].first, total_size);
      }
      uint32_t diff = segs[0].first;
      buf_seq += diff;
      for (i = 0; i < n_seg; i++) {
        segs[i].first -= diff;
        segs[i].second -= diff;
      }
    }
    return true;
  }

private:
  struct EtherHeader
  {
    /** Destination MAC */
    uint8_t dstMac[6];
    /** Source MAC */
    uint8_t srcMac[6];
    /** EtherType */
    uint16_t etherType;
  };

  struct IpHeader
  {
    uint8_t internetHeaderLength : 4,
      /** IP version number, has the value of 4 for IPv4 */
      ipVersion : 4;
    /** type of service, same as Differentiated Services Code Point (DSCP)*/
    uint8_t typeOfService;
    /** Entire packet (fragment) size, including header and data, in bytes */
    uint16_t totalLength;
    /** Identification field. Primarily used for uniquely identifying the group of fragments of a single IP datagram*/
    uint16_t ipId;
    /** Fragment offset field, measured in units of eight-byte blocks (64 bits) */
    uint16_t fragmentOffset;
    /** An eight-bit time to live field helps prevent datagrams from persisting (e.g. going in circles) on an internet.
     * In practice, the field has become a hop count */
    uint8_t timeToLive;
    /** Defines the protocol used in the data portion of the IP datagram. Must be one of ::IPProtocolTypes */
    uint8_t protocol;
    /** Error-checking of the header */
    uint16_t headerChecksum;
    /** IPv4 address of the sender of the packet */
    uint32_t ipSrc;
    /** IPv4 address of the receiver of the packet */
    uint32_t ipDst;
  };

  struct TcpHeader
  {
    uint16_t portSrc;
    uint16_t portDst;
    uint32_t sequenceNumber;
    uint32_t ackNumber;
    uint16_t reserved : 4,
      /** Specifies the size of the TCP header in 32-bit words */
      dataOffset : 4,
      /** FIN flag */
      finFlag : 1,
      /** SYN flag */
      synFlag : 1,
      /** RST flag */
      rstFlag : 1,
      /** PSH flag */
      pshFlag : 1,
      /** ACK flag */
      ackFlag : 1,
      /** URG flag */
      urgFlag : 1,
      /** ECE flag */
      eceFlag : 1,
      /** CWR flag */
      cwrFlag : 1;
    /** The size of the receive window, which specifies the number of window size units (by default, bytes) */
    uint16_t windowSize;
    /** The 16-bit checksum field is used for error-checking of the header and data */
    uint16_t headerChecksum;
    /** If the URG flag (@ref tcphdr#urgFlag) is set, then this 16-bit field is an offset from the sequence number
     * indicating the last urgent data byte */
    uint16_t urgentPointer;
  };

  static const int IPHeaderPos = sizeof(EtherHeader);
  static const int TcpHeaderPos = IPHeaderPos + sizeof(IpHeader); // we assume Ip header is fixed 20 bytes
  bool init_stream = false;
  uint32_t filter_src_ip;
  uint32_t filter_dst_ip;
  uint16_t filter_src_port;
  uint16_t filter_dst_port;
  uint32_t buf_seq;
  static const int MAX_SEG = 5;
  uint32_t n_seg;
  std::pair<uint32_t, uint32_t> segs[MAX_SEG];
  char recvbuf[BUFSIZE];
};
