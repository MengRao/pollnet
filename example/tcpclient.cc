#include <bits/stdc++.h>

struct ClientConf
{
  static const uint32_t RecvBufSize = 4096;
  static const uint32_t ConnRetrySec = 5;
  static const uint32_t SendTimeoutSec = 1;
  static const uint32_t RecvTimeoutSec = 3;
  struct UserData
  {
  };
};

#ifdef USE_TCPDIRECT
#include "../Tcpdirect.h"
using TcpClient = TcpdirectTcpClient<ClientConf>;
#else
#ifdef USE_EFVI
#include "../efvitcp/EfviTcp.h"
using TcpClient = EfviTcpClient<ClientConf>;
#else
#include "../Socket.h"
using TcpClient = SocketTcpClient<ClientConf>;
#endif
#endif

#include "timestamp.h"

using namespace std;

struct Packet
{
  uint64_t ts = 0;
  uint64_t val = 0;
};

volatile bool running = true;

void my_handler(int s) {
  running = false;
}

TcpClient client;
Packet pack;

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
  if (!client.init(interface, server_ip, 1234)) {
    cout << client.getLastError() << endl;
    exit(1);
  }

  while (running) {
    struct
    {
      void onTcpConnectFailed() { cout << "onTcpConnectFailed, " << client.getLastError() << endl; }
      void onTcpConnected(TcpClient::Conn& conn) { cout << "onTcpConnected" << endl; }
      void onSendTimeout(TcpClient::Conn& conn) {
        pack.val++;
        pack.ts = getns();
        conn.writeNonblock(&pack, sizeof(pack));
      }
      uint32_t onTcpData(TcpClient::Conn& conn, const uint8_t* data, uint32_t size) {
        auto now = getns();
        while (size >= sizeof(Packet)) {
          const Packet& recv_pack = *(const Packet*)data;
          auto lat = now - recv_pack.ts;
          cout << "recv val: " << recv_pack.val << " latency: " << lat << endl;
          data += sizeof(Packet);
          size -= sizeof(Packet);
        }
        return size;
      }
      void onRecvTimeout(TcpClient::Conn& conn) {
        cout << "onRecvTimeout" << endl;
        conn.close("timeout");
      }
      void onTcpDisconnect(TcpClient::Conn& conn) { cout << "onDisconnect: " << conn.getLastError() << endl; }
    } handler;
    client.poll(handler);
  }

  return 0;
}
