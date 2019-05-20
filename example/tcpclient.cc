#include <bits/stdc++.h>

#ifdef USE_SOLARFLARE
#include "../Tcpdirect.h"
using TcpClient = TcpdirectTcpClient<>;
#else
#include "../Socket.h"
using TcpClient = SocketTcpClient<>;
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
  std::string interface = argv[1];
  std::string server_ip = argv[2];

  TcpClient client;
  if (!client.init(interface, server_ip, 1234)) {
    cout << client.getLastError() << endl;
    return 1;
  }

  Packet pack;
  uint64_t last_connect_time = 0;
  while (running) {
    auto now = getns();
    if (!client.isConnected()) {
      // retry connecting once per sec
      if (now - last_connect_time < 1000000000) continue;
      last_connect_time = now;
      if (!client.connect()) {
        cout << "connect error: " << client.getLastError() << endl;
        continue;
      }
      now = getns();
    }
    // send new pack once per sec
    if (now - pack.ts >= 1000000000) {
      pack.val++;
      pack.ts = now;
      client.writeNonblock((const char*)&pack, sizeof(pack));
    }
    client.read([](const char* data, uint32_t size) {
      auto now = getns();
      while (size >= sizeof(Packet)) {
        const Packet& recv_pack = *(const Packet*)data;
        auto lat = now - recv_pack.ts;
        cout << "recv val: " << recv_pack.val << " latency: " << lat << endl;
        data += sizeof(Packet);
        size -= sizeof(Packet);
      }
      return size;
    });
    if (!client.isConnected()) {
      cout << "connection closed: " << client.getLastError() << endl;
    }
  }

  return 0;
}
