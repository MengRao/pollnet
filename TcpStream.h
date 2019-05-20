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

template<bool WaitForResend = false, uint32_t BUFSIZE = 1 << 20>
class TcpStream
{
public:
  // use "0.0.0.0" for wildcard ip and 0 for wildcard port
  void initFilter(const std::string& src_ip, uint16_t src_port, const std::string& dst_ip, uint16_t dst_port) {
    inet_pton(AF_INET, src_ip.data(), &filter_src_ip);
    inet_pton(AF_INET, dst_ip.data(), &filter_dst_ip);
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
    }
    if (!init_stream) {
      init_stream = true;
      expect_seq = seq;
      head = tail = 0;
    }
    int32_t seq_diff = seq - expect_seq;
    if (seq_diff) {
      if (seq_diff > 0 && !WaitForResend) {
        head = tail = 0;
        expect_seq = seq;
      }
      else {
        return false;
      }
    }

    uint32_t header_len = sizeof(IpHeader) + tcp_header.dataOffset * 4;
    const char* new_data = data + IPHeaderPos + header_len;
    uint32_t new_size = ntohs(ip_header.totalLength) - header_len;

    expect_seq += new_size + tcp_header.synFlag;

    if (new_size == 0) {
      return false;
    }
    if (new_size + tail > BUFSIZE) {
      return false;
    }
    if (tail == 0) {
      uint32_t remaining = handler(new_data, new_size);
      if (remaining) {
        new_data += new_size - remaining;
        memcpy(recvbuf, new_data, remaining);
        tail = remaining;
      }
    }
    else {
      memcpy(recvbuf + tail, new_data, new_size);
      tail += new_size;
      uint32_t remaining = handler(recvbuf + head, tail - head);
      if (remaining == 0) {
        head = tail = 0;
      }
      else {
        head = tail - remaining;
        if (head >= BUFSIZE / 2) {
          memcpy(recvbuf, recvbuf + head, remaining);
          head = 0;
          tail = remaining;
        }
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
  uint32_t expect_seq;
  uint32_t head;
  uint32_t tail;
  char recvbuf[BUFSIZE];
};
