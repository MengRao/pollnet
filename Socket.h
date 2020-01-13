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
#include <memory>

template<uint32_t RecvBufSize>
class SocketTcpConnection
{
public:
  ~SocketTcpConnection() { close("destruct"); }

  const char* getLastError() { return last_error_; };

  bool isConnected() { return fd_ >= 0; }

  bool connect(const char* server_ip, uint16_t server_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      saveError("socket error", true);
      return false;
    }
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, server_ip, &(server_addr.sin_addr));
    server_addr.sin_port = htons(server_port);
    bzero(&(server_addr.sin_zero), 8);
    if (::connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
      saveError("connect error", true);
      ::close(fd);
      return false;
    }
    return open(fd);
  }

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

  bool write(const char* data, uint32_t size, bool more = false) {
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
    return true;
  }

  bool writeNonblock(const char* data, uint32_t size, bool more = false) {
    int flags = MSG_NOSIGNAL;
    if (more) flags |= MSG_MORE;
    int sent = ::send(fd_, data, size, flags);
    if (sent != size) {
      close("send error", true);
      return false;
    }
    return true;
  }

  template<typename Handler>
  bool read(Handler handler) {
    int ret = ::read(fd_, recvbuf_ + tail_, RecvBufSize - tail_);
    if (ret <= 0) {
      if (ret < 0 && errno == EAGAIN) return false;
      if (ret < 0) {
        close("read error", true);
      }
      else {
        close("remote close");
      }
      return false;
    }
    tail_ += ret;

    uint32_t remaining = handler(recvbuf_ + head_, tail_ - head_);
    if (remaining == 0) {
      head_ = tail_ = 0;
    }
    else {
      head_ = tail_ - remaining;
      if (head_ >= RecvBufSize / 2) {
        memcpy(recvbuf_, recvbuf_ + head_, remaining);
        head_ = 0;
        tail_ = remaining;
      }
      else if (tail_ == RecvBufSize) {
        close("recv buf full");
      }
    }
    return true;
  }

protected:

  template<uint32_t>
  friend class SocketTcpServer;

  bool open(int fd) {
    fd_ = fd;
    head_ = tail_ = 0;

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
  uint32_t head_;
  uint32_t tail_;
  char recvbuf_[RecvBufSize];
  char last_error_[64] = "";
};

template<uint32_t RecvBufSize = 4096>
class SocketTcpClient : public SocketTcpConnection<RecvBufSize>
{
public:
  bool connect(const char* interface, const char* server_ip, uint16_t server_port) {
    return SocketTcpConnection<RecvBufSize>::connect(server_ip, server_port);
  }

};

template<uint32_t RecvBufSize = 4096>
class SocketTcpServer
{
public:
  using TcpConnection = SocketTcpConnection<RecvBufSize>;
  using TcpConnectionPtr = std::unique_ptr<TcpConnection>;

  bool init(const char* interface, const char* server_ip, uint16_t server_port) {
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

  TcpConnectionPtr accept() {
    struct sockaddr_in clientaddr;
    socklen_t addr_len = sizeof(clientaddr);
    int fd = ::accept(listenfd_, (struct sockaddr*)&(clientaddr), &addr_len);
    if (fd < 0) {
      return TcpConnectionPtr();
    }
    TcpConnectionPtr conn(new TcpConnection());
    if (!conn->open(fd)) {
      return TcpConnectionPtr();
    }
    return conn;
  }

  bool accept2(TcpConnection& conn) {
    struct sockaddr_in clientaddr;
    socklen_t addr_len = sizeof(clientaddr);
    int fd = ::accept(listenfd_, (struct sockaddr*)&(clientaddr), &addr_len);
    if (fd < 0) {
      return false;
    }
    if (!conn.open(fd)) {
      return false;
    }
    return true;
  }

private:
  void saveError(const char* msg) { snprintf(last_error_, sizeof(last_error_), "%s %s", msg, strerror(errno)); }

  int listenfd_ = -1;
  char last_error_[64] = "";
};

class SocketUdpReceiver
{
public:
  bool init(const char* interface, const char* dest_ip, uint16_t dest_port, const char* subscribe_ip = nullptr) {
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

    if (subscribe_ip) {
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

private:
  void saveError(const char* msg) { snprintf(last_error_, sizeof(last_error_), "%s %s", msg, strerror(errno)); }

  static const uint32_t RecvBufSize = 1500;
  int fd_ = -1;
  char buf[RecvBufSize];
  char last_error_[64] = "";
};

class SocketEthReceiver
{
public:
  bool init(const char* interface) {
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
  char buf[RecvBufSize];
  char last_error_[64] = "";
};

