#include <bits/stdc++.h>

#ifdef USE_SOLARFLARE
#include "../Tcpdirect.h"
using TcpServer = TcpdirectTcpServer<>;
#else
#include "../Socket.h"
using TcpServer = SocketTcpServer<>;
#endif

using namespace std;

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
  const char* interface = argv[1];
  const char* server_ip = argv[2];

  TcpServer server;
  if (!server.init(interface, server_ip, 1234)) {
    cout << server.getLastError() << endl;
    return 1;
  }

  vector<TcpServer::TcpConnectionPtr> conns;
  while (running) {
    auto new_conn = server.accept();
    if (new_conn) {
      struct sockaddr_in addr;
      new_conn->getPeername(addr);
      conns.push_back(move(new_conn));
      cout << "new connection from: " << inet_ntoa(addr.sin_addr) << ":" << ntohs(addr.sin_port)
           << ", total connections: " << conns.size() << endl;
    }
    auto it = conns.begin();
    while (it != conns.end()) {
      (*it)->read([&](const uint8_t* data, uint32_t size) {
        // simply echo data back
        (*it)->writeNonblock(data, size);
        return 0;
      });
      if ((*it)->isConnected()) {
        it++;
      }
      else {
        swap(*it, conns.back());
        conns.pop_back();
        cout << "client disconnected, total connections: " << conns.size() << endl;
      }
    }
  }
  // destruct all conns before server
  conns.clear();

  return 0;
}

