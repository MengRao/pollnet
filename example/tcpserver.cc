#include <bits/stdc++.h>
#include <netinet/in.h>

struct ServerConf
{
  static const uint32_t RecvBufSize = 4096;
  static const uint32_t MaxConns = 10;
  static const uint32_t SendTimeoutSec = 0;
  static const uint32_t RecvTimeoutSec = 10;
  struct UserData
  {
    struct sockaddr_in addr;
  };
};

#ifdef USE_TCPDIRECT
#include "../Tcpdirect.h"
using TcpServer = TcpdirectTcpServer<ServerConf>;
#else
#ifdef USE_EFVI
#include "../efvitcp/EfviTcp.h"
using TcpServer = EfviTcpServer<ServerConf>;
#else
#include "../Socket.h"
using TcpServer = SocketTcpServer<ServerConf>;
#endif
#endif

using namespace std;

volatile bool running = true;

void my_handler(int s) {
  running = false;
}

TcpServer server;

int main(int argc, char** argv) {
  struct sigaction sigIntHandler;

  sigIntHandler.sa_handler = my_handler;
  sigemptyset(&sigIntHandler.sa_mask);
  sigIntHandler.sa_flags = 0;

  sigaction(SIGINT, &sigIntHandler, NULL);

  if (argc < 3) {
    cout << "usage: " << argv[0] << " interface server_ip" << endl;
    exit(1);
  }
  const char* interface = argv[1];
  const char* server_ip = argv[2];

  if (!server.init(interface, server_ip, 1234)) {
    cout << server.getLastError() << endl;
    return 1;
  }

  while (running) {
    struct
    {
      void onTcpConnected(TcpServer::Conn& conn) {
        conn.getPeername(conn.addr);
        cout << "new connection from: " << inet_ntoa(conn.addr.sin_addr) << ":" << ntohs(conn.addr.sin_port)
             << ", total connections: " << server.getConnCnt() << endl;
        /*
        server.foreachConn([&](TcpServer::Conn& conn) {
          cout << "current connection from: " << inet_ntoa(conn.addr.sin_addr) << ":" << ntohs(conn.addr.sin_port)
               << endl;
        });
        */
      }
      void onSendTimeout(TcpServer::Conn& conn) {
        cout << "onSendTimeout should not be called as SendTimeoutSec=0" << endl;
        exit(1);
      }
      uint32_t onTcpData(TcpServer::Conn& conn, const uint8_t* data, uint32_t size) {
        conn.writeNonblock(data, size);
        return 0;
      }
      void onRecvTimeout(TcpServer::Conn& conn) {
        cout << "onRecvTimeout" << endl;
        conn.close("timeout");
      }
      void onTcpDisconnect(TcpServer::Conn& conn) {
        cout << "client disconnected: " << inet_ntoa(conn.addr.sin_addr) << ":" << ntohs(conn.addr.sin_port)
             << ", reason: " << conn.getLastError() << ", total connections: " << server.getConnCnt() << endl;
      }
    } handler;
    server.poll(handler);
  }

  return 0;
}

