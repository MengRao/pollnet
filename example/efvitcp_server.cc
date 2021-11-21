#include <bits/stdc++.h>
using namespace std;
#include "../efvitcp/TcpServer.h"

struct Conf
{
  static const uint32_t ConnSendBufCnt = 512;
  static const bool SendBuf1K = true;
  static const uint32_t ConnRecvBufSize = 40960;
  static const uint32_t MaxConnCnt = 200;
  static const uint32_t MaxTimeWaitConnCnt = 100;
  static const uint32_t RecvBufCnt = 512;
  static const uint32_t SynRetries = 3;
  static const uint32_t TcpRetries = 10;
  static const uint32_t DelayedAckMS = 10;
  static const uint32_t MinRtoMS = 100;
  static const uint32_t MaxRtoMS = 30 * 1000;
  static const bool WindowScaleOption = false;
  static const bool TimestampOption = false;
  static const int CongestionControlAlgo = 0; // 0: no cwnd, 1: new reno, 2: cubic
  static const uint32_t UserTimerCnt = 1;
  struct UserData
  {
  };
};
using TcpServer = efvitcp::TcpServer<Conf>;
using TcpConn = TcpServer::Conn;

volatile bool running = true;

void my_handler(int s) {
  running = false;
}

class Server
{
public:
  int run(const char* interface, uint16_t server_port) {
    const char* err = server.init(interface);
    if (err) {
      cout << err << endl;
      return 1;
    }
    err = server.listen(server_port);
    if (err) {
      cout << err << endl;
      return 1;
    }
    cout << "listen ok" << endl;
    while (running) {
      server.poll(*this);
    }
    return 0;
  }

  bool allowNewConnection(uint32_t ip, uint16_t port_be) {
    struct in_addr in;
    in.s_addr = ip;
    const char* s = inet_ntoa(in);
    cout << "allowNewConnection: " << s << ":" << ntohs(port_be) << endl;
    return true;
  }

  void onConnectionEstablished(TcpConn& conn) {
    struct sockaddr_in addr;
    conn.getPeername(addr);
    cout << "onConnectionEstablished, id: " << conn.getConnId() << " from: " << inet_ntoa(addr.sin_addr) << ":"
         << ntohs(addr.sin_port) << endl;
    cout << "sendable: " << conn.getSendable() << endl;
    conn.setUserTimer(0, 10 * 1000);
  }

  uint32_t onData(TcpConn& conn, uint8_t* data, uint32_t size) {
    if (conn.send(data, size) != size) {
      conn.close();
      return 0;
    }
    conn.setUserTimer(0, 10 * 1000);
    return 0;
  }

  void onConnectionReset(TcpConn& conn) { cout << "onConnectionReset" << endl; }

  void onConnectionClosed(TcpConn& conn) { cout << "onConnectionClosed" << endl; }

  void onFin(TcpConn& conn, uint8_t* data, uint32_t size) {
    cout << "onFin, remaining data size:" << size << endl;
    conn.sendFin();
  }

  void onConnectionTimeout(TcpConn& conn) {
    cout << "onConnectionTimeout, established: " << conn.isEstablished() << endl;
  }

  void onMoreSendable(TcpConn& conn) { /*cout << "onMoreSendable: " << conn.getSendable() << endl;*/
  }

  void onUserTimeout(TcpConn& conn, uint32_t timer_id) {
    cout << "onUserTimeout: " << timer_id << ", conn_id: " << conn.getConnId() << endl;
    conn.close();
  }

  TcpServer server;
};

// Server server;

int main(int argc, const char** argv) {
  struct sigaction sigIntHandler;
  sigIntHandler.sa_handler = my_handler;
  sigemptyset(&sigIntHandler.sa_mask);
  sigIntHandler.sa_flags = 0;
  sigaction(SIGINT, &sigIntHandler, NULL);
  sigaction(SIGTERM, &sigIntHandler, NULL);
  sigaction(SIGPIPE, &sigIntHandler, NULL);

  if (argc < 3) {
    cout << "usage: " << argv[0] << " interface server_port" << endl;
    return 1;
  }
  const char* interface = argv[1];
  uint16_t server_port = atoi(argv[2]);
  Server* server = new Server();
  return server->run(interface, server_port);
}
