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
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <net/ethernet.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <memory>

template<typename Conf>
class SocketTcpConnection : public Conf::UserData
{
public:
  ~SocketTcpConnection() { close("destruct"); }

  const char* getLastError() { return last_error_; };

  bool isConnected() { return fd_ >= 0; }

  bool getPeername(struct sockaddr_in& addr) {
    socklen_t addr_len = sizeof(addr);
    return ::getpeername(fd_, (struct sockaddr*)&addr, &addr_len) == 0;
  }

  void close(const char* reason, bool check_errno = false) {
    if (fd_ >= 0) {
      saveError(reason, check_errno);
      ::close(fd_);
      fd_ = -1;
    }
  }

  bool write(const void* data_, uint32_t size, bool more = false) {
    const uint8_t* data = (const uint8_t*)data_;
    int flags = MSG_NOSIGNAL;
    if (more) flags |= MSG_MORE;
    do {
      int sent = ::send(fd_, data, size, flags);
      if (sent < 0) {
        if (errno != EAGAIN) {
          close("send error", true);
          return false;
        }
        continue;
      }
      data += sent;
      size -= sent;
    } while (size != 0);
    if (Conf::SendTimeoutSec) send_ts_ = time(0);
    return true;
  }

  bool writeNonblock(const void* data, uint32_t size, bool more = false) {
    int flags = MSG_NOSIGNAL;
    if (more) flags |= MSG_MORE;
    int sent = ::send(fd_, data, size, flags);
    if (sent != (int)size) {
      close("send error", true);
      return false;
    }
    if (Conf::SendTimeoutSec) send_ts_ = time(0);
    return true;
  }

protected:
  template<typename ServerConf>
  friend class SocketTcpServer;

  bool connect(struct sockaddr_in& server_addr) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      saveError("socket error", true);
      return false;
    }
    if (::connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
      saveError("connect error", true);
      ::close(fd);
      return false;
    }
    return open(time(0), fd);
  }

  template<typename Handler>
  void pollConn(int64_t now, Handler& handler) {
    if (Conf::SendTimeoutSec && now >= send_ts_ + Conf::SendTimeoutSec) {
      handler.onSendTimeout(*this);
      send_ts_ = now;
    }
    bool got_data = read([&](const uint8_t* data, uint32_t size) { return handler.onTcpData(*this, data, size); });
    if (Conf::RecvTimeoutSec) {
      if (!got_data && now >= expire_ts_) {
        handler.onRecvTimeout(*this);
        got_data = true;
      }
      if (got_data) expire_ts_ = now + Conf::RecvTimeoutSec;
    }
  }

  template<typename Handler>
  bool read(Handler handler) {
    int ret = ::read(fd_, recvbuf_ + tail_, Conf::RecvBufSize - tail_);
    if (ret <= 0) {
      if (ret < 0 && errno == EAGAIN) return false;
      if (ret < 0)
        close("read error", true);
      else
        close("remote close");
      return false;
    }
    tail_ += ret;

    uint32_t remaining = handler(recvbuf_ + head_, tail_ - head_);
    if (remaining == 0) {
      head_ = tail_ = 0;
    }
    else {
      head_ = tail_ - remaining;
      if (head_ >= Conf::RecvBufSize / 2) {
        memcpy(recvbuf_, recvbuf_ + head_, remaining);
        head_ = 0;
        tail_ = remaining;
      }
      else if (tail_ == Conf::RecvBufSize) {
        close("recv buf full");
      }
    }
    return true;
  }

  bool open(int64_t now, int fd) {
    fd_ = fd;
    head_ = tail_ = 0;
    send_ts_ = now;
    expire_ts_ = now + Conf::RecvTimeoutSec;

    int flags = fcntl(fd_, F_GETFL, 0);
    if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
      close("fcntl O_NONBLOCK error", true);
      return false;
    }

    int yes = 1;
    if (setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0) {
      close("setsockopt TCP_NODELAY error", true);
      return false;
    }

    return true;
  }

  void saveError(const char* msg, bool check_errno) {
    snprintf(last_error_, sizeof(last_error_), "%s %s", msg, check_errno ? (const char*)strerror(errno) : "");
  }

  int fd_ = -1;
  int64_t send_ts_ = 0;
  int64_t expire_ts_ = 0;
  uint32_t head_;
  uint32_t tail_;
  uint8_t recvbuf_[Conf::RecvBufSize];
  char last_error_[64] = "";
};

template<typename Conf>
class SocketTcpClient : public SocketTcpConnection<Conf>
{
public:
  using Conn = SocketTcpConnection<Conf>;

  bool init(const char* interface, const char* server_ip, uint16_t server_port) {
    server_addr_.sin_family = AF_INET;
    inet_pton(AF_INET, server_ip, &(server_addr_.sin_addr));
    server_addr_.sin_port = htons(server_port);
    bzero(&(server_addr_.sin_zero), 8);
    return true;
  }

  template<typename Handler>
  void poll(Handler& handler) {
    int64_t now = time(0);
    if (!this->isConnected()) {
      if (now < next_conn_ts_) return;
      next_conn_ts_ = now + Conf::ConnRetrySec;
      if (!this->connect(server_addr_)) {
        handler.onTcpConnectFailed();
        return;
      }
      handler.onTcpConnected(*this);
    }
    this->pollConn(now, handler);
    if (!this->isConnected()) handler.onTcpDisconnect(*this);
  }

private:
  int64_t next_conn_ts_ = 0;
  struct sockaddr_in server_addr_;
};

template<typename Conf>
class SocketTcpServer
{
public:
  using Conn = SocketTcpConnection<Conf>;

  bool init(const char* interface, const char* server_ip, uint16_t server_port) {
    for (uint32_t i = 0; i < Conf::MaxConns; i++) conns_[i] = conns_data_ + i;
    listenfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd_ < 0) {
      saveError("socket error");
      return false;
    }

    int flags = fcntl(listenfd_, F_GETFL, 0);
    if (fcntl(listenfd_, F_SETFL, flags | O_NONBLOCK) < 0) {
      close("fcntl O_NONBLOCK error");
      return false;
    }

    int yes = 1;
    if (setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
      close("setsockopt SO_REUSEADDR error");
      return false;
    }

    struct sockaddr_in local_addr;
    local_addr.sin_family = AF_INET;
    inet_pton(AF_INET, server_ip, &(local_addr.sin_addr));
    local_addr.sin_port = htons(server_port);
    bzero(&(local_addr.sin_zero), 8);
    if (bind(listenfd_, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
      close("bind error");
      return false;
    }
    if (listen(listenfd_, 5) < 0) {
      close("listen error");
      return false;
    }

    return true;
  };

  void close(const char* reason) {
    if (listenfd_ >= 0) {
      saveError(reason);
      ::close(listenfd_);
      listenfd_ = -1;
    }
  }

  const char* getLastError() { return last_error_; };

  ~SocketTcpServer() { close("destruct"); }

  bool isClosed() { return listenfd_ < 0; }

  uint32_t getConnCnt() { return conns_cnt_; }

  template<typename Handler>
  void foreachConn(Handler handler) {
    for (uint32_t i = 0; i < conns_cnt_; i++) {
      Conn& conn = *conns_[i];
      handler(conn);
    }
  }

  template<typename Handler>
  void poll(Handler& handler) {
    int64_t now = time(0);
    if (conns_cnt_ < Conf::MaxConns) {
      Conn& conn = *conns_[conns_cnt_];
      struct sockaddr_in clientaddr;
      socklen_t addr_len = sizeof(clientaddr);
      int fd = ::accept(listenfd_, (struct sockaddr*)&(clientaddr), &addr_len);
      if (fd >= 0 && conn.open(now, fd)) {
        conns_cnt_++;
        handler.onTcpConnected(conn);
      }
    }
    for (uint32_t i = 0; i < conns_cnt_;) {
      Conn& conn = *conns_[i];
      conn.pollConn(now, handler);
      if (conn.isConnected())
        i++;
      else {
        std::swap(conns_[i], conns_[--conns_cnt_]);
        handler.onTcpDisconnect(conn);
      }
    }
  }

private:
  void saveError(const char* msg) { snprintf(last_error_, sizeof(last_error_), "%s %s", msg, strerror(errno)); }

  int listenfd_ = -1;
  uint32_t conns_cnt_ = 0;
  Conn* conns_[Conf::MaxConns];
  Conn conns_data_[Conf::MaxConns];
  char last_error_[64] = "";
};

template<uint32_t RecvBufSize = 1500>
class SocketUdpReceiver
{
public:
  bool init(const char* interface, const char* dest_ip, uint16_t dest_port, const char* subscribe_ip = "") {
    if ((fd_ = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
      saveError("socket error");
      return false;
    }
    int flags = fcntl(fd_, F_GETFL, 0);
    if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
      close("fcntl O_NONBLOCK error");
      return false;
    }
    int optval = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int)) < 0) {
      close("setsockopt SO_REUSEADDR error");
      return false;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET; // IPv4
    servaddr.sin_port = htons(dest_port);
    inet_pton(AF_INET, dest_ip, &(servaddr.sin_addr));
    if (bind(fd_, (const struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
      close("bind failed");
      return false;
    }

    if (subscribe_ip[0]) {
      struct ip_mreq group;
      inet_pton(AF_INET, subscribe_ip, &(group.imr_interface));
      inet_pton(AF_INET, dest_ip, &(group.imr_multiaddr));

      if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&group, sizeof(group)) < 0) {
        close("setsockopt IP_ADD_MEMBERSHIP failed");
        return false;
      }
    }

    return true;
  }

  ~SocketUdpReceiver() { close("destruct"); }

  uint16_t getLocalPort() {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    getsockname(fd_, (struct sockaddr*)&addr, &addrlen);
    return ntohs(addr.sin_port);
  }

  const char* getLastError() { return last_error_; };

  bool isClosed() { return fd_ < 0; }

  void close(const char* reason) {
    if (fd_ >= 0) {
      saveError(reason);
      ::close(fd_);
      fd_ = -1;
    }
  }

  template<typename Handler>
  bool read(Handler handler) {
    int n = ::read(fd_, buf, RecvBufSize);
    if (n > 0) {
      handler(buf, n);
      return true;
    }
    return false;
  }

  template<typename Handler>
  bool recvfrom(Handler handler) {
    struct sockaddr_in src_addr;
    socklen_t addrlen = sizeof(src_addr);
    int n = ::recvfrom(fd_, buf, RecvBufSize, 0, (struct sockaddr*)&src_addr, &addrlen);
    if (n > 0) {
      handler(buf, n, src_addr);
      return true;
    }
    return false;
  }

  bool sendto(const void* data, uint32_t size, const sockaddr_in& dst_addr) {
    return ::sendto(fd_, data, size, 0, (const struct sockaddr*)&dst_addr, sizeof(dst_addr)) == size;
  }

private:
  void saveError(const char* msg) { snprintf(last_error_, sizeof(last_error_), "%s %s", msg, strerror(errno)); }

  int fd_ = -1;
  uint8_t buf[RecvBufSize];
  char last_error_[64] = "";
};

class SocketUdpSender
{
public:
  bool init(const char* interface, const char* local_ip, uint16_t local_port, const char* dest_ip, uint16_t dest_port) {
    if ((fd_ = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
      saveError("socket error");
      return false;
    }
    int flags = fcntl(fd_, F_GETFL, 0);
    if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
      close("fcntl O_NONBLOCK error");
      return false;
    }

    struct sockaddr_in localaddr;
    memset(&localaddr, 0, sizeof(localaddr));
    localaddr.sin_family = AF_INET; // IPv4
    localaddr.sin_port = htons(local_port);
    inet_pton(AF_INET, local_ip, &(localaddr.sin_addr));
    if (bind(fd_, (const struct sockaddr*)&localaddr, sizeof(localaddr)) < 0) {
      close("bind failed");
      return false;
    }

    struct sockaddr_in destaddr;
    memset(&destaddr, 0, sizeof(destaddr));
    destaddr.sin_family = AF_INET; // IPv4
    destaddr.sin_port = htons(dest_port);
    inet_pton(AF_INET, dest_ip, &(destaddr.sin_addr));
    if (::connect(fd_, (struct sockaddr*)&destaddr, sizeof(destaddr)) < 0) {
      close("connect error");
      return false;
    }

    return true;
  }

  ~SocketUdpSender() { close("destruct"); }

  uint16_t getLocalPort() {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    getsockname(fd_, (struct sockaddr*)&addr, &addrlen);
    return ntohs(addr.sin_port);
  }

  const char* getLastError() { return last_error_; };

  bool isClosed() { return fd_ < 0; }

  void close(const char* reason) {
    if (fd_ >= 0) {
      saveError(reason);
      ::close(fd_);
      fd_ = -1;
    }
  }

  bool write(const void* data, uint32_t size) { return ::send(fd_, data, size, 0) == size; }

private:
  void saveError(const char* msg) { snprintf(last_error_, sizeof(last_error_), "%s %s", msg, strerror(errno)); }

  int fd_ = -1;
  char last_error_[64] = "";
};

class SocketEthReceiver
{
public:
  bool init(const char* interface, bool promiscuous = false) {
    fd_ = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd_ < 0) {
      saveError("socket error");
      return false;
    }

    int flags = fcntl(fd_, F_GETFL, 0);
    if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
      close("fcntl O_NONBLOCK error");
      return false;
    }

    struct sockaddr_ll socket_address;
    memset(&socket_address, 0, sizeof(socket_address));
    socket_address.sll_family = PF_PACKET;
    socket_address.sll_ifindex = if_nametoindex(interface);
    socket_address.sll_protocol = htons(ETH_P_ALL);

    if (bind(fd_, (struct sockaddr*)&socket_address, sizeof(socket_address)) < 0) {
      close("bind error");
      return false;
    }

    if (promiscuous) {
      struct packet_mreq mreq = {0};
      mreq.mr_ifindex = if_nametoindex(interface);
      mreq.mr_type = PACKET_MR_PROMISC;
      if (setsockopt(fd_, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        close("setsockopt PACKET_ADD_MEMBERSHIP");
        return false;
      }
    }
    return true;
  }

  ~SocketEthReceiver() { close("destruct"); }

  const char* getLastError() { return last_error_; };

  void close(const char* reason) {
    if (fd_ >= 0) {
      saveError(reason);
      ::close(fd_);
      fd_ = -1;
    }
  }

  template<typename Handler>
  bool read(Handler handler) {
    int n = ::read(fd_, buf, RecvBufSize);
    if (n > 0) {
      handler(buf, n);
      return true;
    }
    return false;
  }

private:
  void saveError(const char* msg) { snprintf(last_error_, sizeof(last_error_), "%s %s", msg, strerror(errno)); }
  static const uint32_t RecvBufSize = 1500;
  int fd_ = -1;
  uint8_t buf[RecvBufSize];
  char last_error_[64] = "";
};

