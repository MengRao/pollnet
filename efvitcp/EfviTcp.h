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
#include <limits>
#include "TcpClient.h"
#include "TcpServer.h"

// This is a wrapper class for pollnet interface
template<typename Conf>
class EfviTcpClient
{
  struct ClientConf
  {
    static const uint32_t ConnSendBufCnt = 1024;
    static const bool SendBuf1K = true;
    static const uint32_t ConnRecvBufSize = Conf::RecvBufSize;
    static const uint32_t MaxConnCnt = 1;
    static const uint32_t MaxTimeWaitConnCnt = 1;
    static const uint32_t RecvBufCnt = 512;
    static const uint32_t SynRetries = 3;
    static const uint32_t TcpRetries = 10;
    static const uint32_t DelayedAckMS = 10;
    static const uint32_t MinRtoMS = 100;
    static const uint32_t MaxRtoMS = 30 * 1000;
    static const bool WindowScaleOption = false;
    static const bool TimestampOption = false;
    static const int CongestionControlAlgo = 0; // 0: no cwnd, 1: new reno, 2: cubic
    static const uint32_t UserTimerCnt = 2;
    struct UserData : public Conf::UserData
    {
      const char* err_ = nullptr;
    };
  };
  using TcpClient = efvitcp::TcpClient<ClientConf>;
  using TcpConn = typename TcpClient::Conn;

public:
  EfviTcpClient()
    : conn(*(Conn*)&client.getConn()) {}

  struct Conn : public TcpConn
  {
    bool isConnected() { return this->isEstablished(); }

    const char* getLastError() { return this->err_; };

    void close(const char* reason) {
      this->err_ = reason;
      TcpConn::close();
    }

    int writeSome(const void* data, uint32_t size, bool more = false) {
      int ret = this->send(data, size, more);
      if (Conf::SendTimeoutSec) this->setUserTimer(0, Conf::SendTimeoutSec * 1000);
      return ret;
    }

    // blocking write is not supported currently
    bool writeNonblock(const void* data, uint32_t size, bool more = false) {
      if ((uint32_t)writeSome(data, size, more) != size) {
        close("send buffer full");
        return false;
      }
      return true;
    }

  };

  const char* getLastError() { return conn.err_; };

  bool isConnected() { return conn.isEstablished(); }

  bool init(const char* interface, const char* server_ip, uint16_t server_port) {
    if ((conn.err_ = client.init(interface))) return false;
    server_ip_ = server_ip;
    server_port_ = server_port;
    return true;
  }

  bool writeNonblock(const void* data, uint32_t size, bool more = false) {
    return conn.writeNonblock(data, size, more);
  }

  void close(const char* reason) { conn.close(reason); }

  void allowReconnect() { next_conn_ts_ = 0; }

  template<typename Handler>
  void poll(Handler& handler) {
    if (conn.isClosed()) {
      int64_t now = time(0);
      if (now < next_conn_ts_) return;
      if (Conf::ConnRetrySec)
        next_conn_ts_ = now + Conf::ConnRetrySec;
      else
        next_conn_ts_ = std::numeric_limits<int64_t>::max(); // disable reconnect
      if ((conn.err_ = client.connect(server_ip_.c_str(), server_port_))) {
        handler.onTcpConnectFailed();
        return;
      }
    }

    struct TmpHandler
    {
      TmpHandler(Handler& h_, Conn& conn_)
        : handler(h_)
        , conn(conn_) {}

      void onConnectionRefused() {
        conn.err_ = "connection refused";
        handler.onTcpConnectFailed();
      }
      void onConnectionReset(TcpConn&) {
        conn.err_ = "connection reset";
        handler.onTcpDisconnect(conn);
      }
      void onConnectionTimeout(TcpConn&) {
        conn.err_ = "connection timeout";
        if (!conn.isEstablished()) handler.onTcpConnectFailed();
        else
          handler.onTcpDisconnect(conn);
      }
      void onConnectionClosed(TcpConn&) {
        conn.err_ = "connection closed";
        handler.onTcpDisconnect(conn);
      }
      void onFin(TcpConn&, uint8_t* data, uint32_t size) {
        if (size) handler.onTcpData(conn, data, size);
        conn.close("remote close");
        handler.onTcpDisconnect(conn);
      }
      void onMoreSendable(TcpConn&) {}
      void onUserTimeout(TcpConn&, uint32_t timer_id) {
        if (timer_id == 0)
          handler.onSendTimeout(conn);
        else
          handler.onRecvTimeout(conn);
      }
      void onConnectionEstablished(TcpConn&) {
        if (Conf::SendTimeoutSec) conn.setUserTimer(0, Conf::SendTimeoutSec * 1000);
        if (Conf::RecvTimeoutSec) conn.setUserTimer(1, Conf::RecvTimeoutSec * 1000);
        handler.onTcpConnected(conn);
      }
      uint32_t onData(TcpConn&, const uint8_t* data, uint32_t size) {
        if (Conf::RecvTimeoutSec) conn.setUserTimer(1, Conf::RecvTimeoutSec * 1000);
        return handler.onTcpData(conn, data, size);
      }

      Handler& handler;
      Conn& conn;
    } tmp_handler(handler, conn);
    client.poll(tmp_handler);
  }

  TcpClient client;
  std::string server_ip_;
  uint16_t server_port_;
  Conn& conn;
  int64_t next_conn_ts_ = 0;
};

template<typename Conf>
class EfviTcpServer
{
  struct ServerConf
  {
    static const uint32_t ConnSendBufCnt = 1024;
    static const bool SendBuf1K = true;
    static const uint32_t ConnRecvBufSize = Conf::RecvBufSize;
    static const uint32_t MaxConnCnt = Conf::MaxConns;
    static const uint32_t MaxTimeWaitConnCnt = Conf::MaxConns;
    static const uint32_t RecvBufCnt = 512;
    static const uint32_t SynRetries = 3;
    static const uint32_t TcpRetries = 10;
    static const uint32_t DelayedAckMS = 10;
    static const uint32_t MinRtoMS = 100;
    static const uint32_t MaxRtoMS = 30 * 1000;
    static const bool WindowScaleOption = false;
    static const bool TimestampOption = false;
    static const int CongestionControlAlgo = 0; // 0: no cwnd, 1: new reno, 2: cubic
    static const uint32_t UserTimerCnt = 2;
    struct UserData : public Conf::UserData
    {
      const char* err_ = nullptr;
    };
  };
  using TcpServer = efvitcp::TcpServer<ServerConf>;
  using TcpConn = typename TcpServer::Conn;

public:
  EfviTcpServer() {}

  struct Conn : public TcpConn
  {
    bool isConnected() { return this->isEstablished(); }

    const char* getLastError() { return this->err_; };

    void close(const char* reason) {
      this->err_ = reason;
      TcpConn::close();
    }

    // blocking write is not supported currently
    bool writeNonblock(const void* data, uint32_t size, bool more = false) {
      if (this->send(data, size, more) != size) {
        close("send buffer full");
        return false;
      }
      if (Conf::SendTimeoutSec) this->setUserTimer(0, Conf::SendTimeoutSec * 1000);
      return true;
    }
  };

  const char* getLastError() { return err_; };

  bool init(const char* interface, const char* server_ip, uint16_t server_port) {
    if ((err_ = server_.init(interface))) return false;
    if ((err_ = server_.listen(server_port))) return false;
    return true;
  }

  void close(const char* reason) {
    if (reason) err_ = reason;
    server_.close();
  }

  bool isClosed() { return err_; }

  uint32_t getConnCnt() { return server_.getConnCnt(); }

  template<typename Handler>
  void foreachConn(Handler handler) {
    server_.foreachConn([&](TcpConn& conn) { handler(*(Conn*)&conn); });
  }

  template<typename Handler>
  void poll(Handler& handler) {
    struct TmpHandler
    {
      TmpHandler(Handler& h_)
        : handler(h_) {}

      bool allowNewConnection(uint32_t ip, uint16_t port_be) { return true; }
      void onConnectionReset(TcpConn& conn) {
        conn.err_ = "connection reset";
        handler.onTcpDisconnect(*(Conn*)&conn);
      }
      void onConnectionTimeout(TcpConn& conn) {
        conn.err_ = "connection timeout";
        if (conn.isEstablished()) handler.onTcpDisconnect(*(Conn*)&conn);
      }
      void onConnectionClosed(TcpConn& conn) {
        conn.err_ = "connection closed";
        handler.onTcpDisconnect(*(Conn*)&conn);
      }
      void onFin(TcpConn& conn, uint8_t* data, uint32_t size) {
        Conn& c = *(Conn*)&conn;
        if (size) handler.onTcpData(c, data, size);
        c.close("remote close");
        handler.onTcpDisconnect(c);
      }
      void onMoreSendable(TcpConn&) {}
      void onUserTimeout(TcpConn& conn, uint32_t timer_id) {
        if (timer_id == 0)
          handler.onSendTimeout(*(Conn*)&conn);
        else
          handler.onRecvTimeout(*(Conn*)&conn);
      }
      void onConnectionEstablished(TcpConn& conn) {
        if (Conf::SendTimeoutSec) conn.setUserTimer(0, Conf::SendTimeoutSec * 1000);
        if (Conf::RecvTimeoutSec) conn.setUserTimer(1, Conf::RecvTimeoutSec * 1000);
        handler.onTcpConnected(*(Conn*)&conn);
      }
      uint32_t onData(TcpConn& conn, const uint8_t* data, uint32_t size) {
        if (Conf::RecvTimeoutSec) conn.setUserTimer(1, Conf::RecvTimeoutSec * 1000);
        return handler.onTcpData(*(Conn*)&conn, data, size);
      }

      Handler& handler;
    } tmp_handler(handler);
    server_.poll(tmp_handler);
  }

  const char* err_ = "Closed";
  TcpServer server_;
};
