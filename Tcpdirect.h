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
#include <string.h>
#include <stdio.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <zf/zf.h>
#include <time.h>
#include <memory>

namespace {
bool _zf_inited = false;

int _zf_init() {
  if (!_zf_inited) {
    int rc = zf_init();
    if (rc < 0) {
      return rc;
    }
    _zf_inited = true;
  }
  return 0;
}
} // namespace

template<typename Conf>
class TcpdirectTcpConnection : public Conf::UserData
{
public:
  ~TcpdirectTcpConnection() { close("destruct"); }

  const char* getLastError() { return last_error_; };

  bool isConnected() { return zock_ && zft_state(zock_) == TCP_ESTABLISHED; }

  bool getPeername(struct sockaddr_in& addr) {
    socklen_t addr_len = sizeof(addr);
    zft_getname(zock_, nullptr, nullptr, (struct sockaddr*)&addr, &addr_len);
    return true;
  }

  void close(const char* reason) {
    if (zock_) {
      saveError(reason, 0);
      zft_free(zock_);
      zock_ = nullptr;
    }
  }

  bool write(const void* data_, uint32_t size, bool more = false) {
    const uint8_t* data = (const uint8_t*)data_;
    int flags = 0;
    if (more) flags |= MSG_MORE;
    do {
      int sent = zft_send_single(zock_, data, size, flags);
      if (sent < 0) {
        if (sent != -EAGAIN && sent != -ENOMEM) {
          saveError("zft_send_single error", sent);
          return false;
        }
        zf_reactor_perform(stack_);
        continue;
      }
      data += sent;
      size -= sent;
    } while (size != 0);
    if (Conf::SendTimeoutSec) send_ts_ = time(0);
    return true;
  }

  bool writeNonblock(const void* data, uint32_t size, bool more = false) {
    int flags = 0;
    if (more) flags |= MSG_MORE;
    if (zft_send_single(zock_, data, size, flags) != size) {
      close("zft_send_single failed");
      return false;
    }
    if (Conf::SendTimeoutSec) send_ts_ = time(0);
    return true;
  }

protected:
  template<typename ServerConf>
  friend class TcpdirectTcpServer;

  bool connect(struct zf_attr* attr, struct sockaddr_in& server_addr) {
    int rc;
    struct zft_handle* tcp_handle;
    if ((rc = zft_alloc(stack_, attr, &tcp_handle)) < 0) {
      saveError("zft_alloc error", rc);
      return false;
    }

    close("reconnect");
    if ((rc = zft_connect(tcp_handle, (struct sockaddr*)&server_addr, sizeof(server_addr), &zock_)) < 0) {
      saveError("zft_connect error", rc);
      zft_handle_free(tcp_handle);
      return false;
    }
    while (zft_state(zock_) == TCP_SYN_SENT) zf_reactor_perform(stack_);
    if (zft_state(zock_) != TCP_ESTABLISHED) {
      saveError("zft_state error", 0);
      return false;
    }
    open(time(0), zock_, stack_);
    return true;
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
    struct
    {
      uint8_t
        msg[sizeof(struct zft_msg)]; // prevent newer gcc from erroring "flexible array member not at end of struct"
      struct iovec iov;
    } msg;
    struct zft_msg* zm = (struct zft_msg*)msg.msg;
    zm->iovcnt = 1;

    zf_reactor_perform(stack_);

    zft_zc_recv(zock_, zm, 0);
    if (zm->iovcnt == 0) return false;

    const uint8_t* new_data = (const uint8_t*)msg.iov.iov_base;
    uint32_t new_size = msg.iov.iov_len;

    if (new_size == 0) {
      zft_zc_recv_done(zock_, zm);
      close("remote close");
      return false;
    }

    if (new_size + tail_ > Conf::RecvBufSize) {
      zft_zc_recv_done(zock_, zm);
      close("recv buf full");
      return false;
    }

    if (tail_ == 0) {
      uint32_t remaining = handler(new_data, new_size);
      if (remaining) {
        new_data += new_size - remaining;
        memcpy(recvbuf_, new_data, remaining);
        tail_ = remaining;
      }
    }
    else {
      memcpy(recvbuf_ + tail_, new_data, new_size);
      tail_ += new_size;
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
      }
    }
    if (zock_) { // this could have been closed
      zft_zc_recv_done(zock_, zm);
    }
    return true;
  }


  bool open(int64_t now, struct zft* zock, struct zf_stack* stack) {
    zock_ = zock;
    stack_ = stack;
    head_ = tail_ = 0;
    send_ts_ = now;
    expire_ts_ = now + Conf::RecvTimeoutSec;
    return true;
  }

  void saveError(const char* msg, int rc) {
    snprintf(last_error_, sizeof(last_error_), "%s %s", msg, rc < 0 ? (const char*)strerror(-rc) : "");
  }

  struct zft* zock_ = nullptr;
  struct zf_stack* stack_ = nullptr; // stack_ is managed by TcpdirectTcpClient or TcpdirectTcpServer

  int64_t send_ts_ = 0;
  int64_t expire_ts_ = 0;
  uint32_t head_;
  uint32_t tail_;
  uint8_t recvbuf_[Conf::RecvBufSize];
  char last_error_[64] = "";
};

template<typename Conf>
class TcpdirectTcpClient : public TcpdirectTcpConnection<Conf>
{
public:
  using Conn = TcpdirectTcpConnection<Conf>;

  ~TcpdirectTcpClient() {
    this->close("destruct");
    if (this->stack_) {
      zf_stack_free(this->stack_);
    }
  }

  bool init(const char* interface, const char* server_ip, uint16_t server_port) {
    server_addr_.sin_family = AF_INET;
    inet_pton(AF_INET, server_ip, &(server_addr_.sin_addr));
    server_addr_.sin_port = htons(server_port);
    bzero(&(server_addr_.sin_zero), 8);

    int rc;
    if ((rc = _zf_init()) < 0) {
      this->saveError("zf_init error", rc);
      return false;
    }

    if (!attr_) {
      if ((rc = zf_attr_alloc(&attr_)) < 0) {
        this->saveError("zf_attr_alloc error", rc);
        return false;
      }
      zf_attr_set_str(attr_, "interface", interface);
      zf_attr_set_int(attr_, "reactor_spin_count", 1);
    }

    if (!this->stack_ && (rc = zf_stack_alloc(attr_, &this->stack_)) < 0) {
      this->saveError("zf_stack_alloc error", rc);
      zf_attr_free(attr_);
      attr_ = nullptr;
      return false;
    }
    return true;
  }

  template<typename Handler>
  void poll(Handler& handler) {
    int64_t now = time(0);
    if (!this->isConnected()) {
      if (now < next_conn_ts_) return;
      next_conn_ts_ = now + Conf::ConnRetrySec;
      if (!this->connect(attr_, server_addr_)) {
        handler.onTcpConnectFailed(*this);
        return;
      }
      handler.onTcpConnected(*this);
    }
    this->pollConn(now, handler);
    if (!this->isConnected()) handler.onTcpDisconnect(*this);
  }

private:
  struct zf_attr* attr_ = nullptr;
  int64_t next_conn_ts_ = 0;
  struct sockaddr_in server_addr_;
};

template<typename Conf>
class TcpdirectTcpServer
{
public:
  using Conn = TcpdirectTcpConnection<Conf>;

  bool init(const char* interface, const char* server_ip, uint16_t server_port) {
    for (uint32_t i = 0; i < Conf::MaxConns; i++) conns_[i] = conns_data_ + i;
    int rc;
    if ((rc = _zf_init()) < 0) {
      saveError("zf_init error", rc);
      return false;
    }

    struct zf_attr* attr;
    if ((rc = zf_attr_alloc(&attr)) < 0) {
      saveError("zf_attr_alloc error", rc);
      return false;
    }
    zf_attr_set_str(attr, "interface", interface);
    zf_attr_set_int(attr, "reactor_spin_count", 1);

    if ((rc = zf_stack_alloc(attr, &stack_)) < 0) {
      saveError("zf_stack_alloc error", rc);
      zf_attr_free(attr);
      return false;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET; // IPv4
    servaddr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &(servaddr.sin_addr));

    if ((rc = zftl_listen(stack_, (struct sockaddr*)&servaddr, sizeof(servaddr), attr, &listener_)) < 0) {
      saveError("zftl_listen error", rc);
      return false;
    }

    return true;
  }

  void close(const char* reason) {
    if (listener_) {
      zftl_free(listener_);
      listener_ = nullptr;
      saveError(reason, 0);
    }
    if (stack_) {
      zf_stack_free(stack_);
      stack_ = nullptr;
    }
  }

  const char* getLastError() { return last_error_; };

  ~TcpdirectTcpServer() { close("destruct"); }

  bool isClosed() { return listener_ == nullptr; }

  uint32_t getConnCnt() { return conns_cnt_; }

  template<typename Handler>
  void poll(Handler& handler) {
    int64_t now = time(0);
    if (conns_cnt_ < Conf::MaxConns) {
      Conn& conn = *conns_[conns_cnt_];
      struct zft* zock;
      zf_reactor_perform(stack_);
      if (zftl_accept(listener_, &zock) >= 0) {
        conn.open(now, zock, stack_);
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
  void saveError(const char* msg, int rc) {
    snprintf(last_error_, sizeof(last_error_), "%s %s", msg, rc < 0 ? (const char*)strerror(-rc) : "");
  }

  struct zf_stack* stack_ = nullptr;
  struct zftl* listener_ = nullptr;
  uint32_t conns_cnt_ = 0;
  Conn* conns_[Conf::MaxConns];
  Conn conns_data_[Conf::MaxConns];
  char last_error_[64] = "";
};

