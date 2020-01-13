#include <bits/stdc++.h>
#include "../TcpStream.h"

#ifdef USE_SOLARFLARE
#include "../Efvi.h"
using EthReceiver = EfviEthReceiver;
#else
#include "../Socket.h"
using EthReceiver = SocketEthReceiver;
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

  if (argc < 6) {
    cout << "usage: " << argv[0] << " interface src_ip src_port dst_ip dst_port" << endl;
    exit(1);
  }
  const char* interface = argv[1];
  const char* src_ip = argv[2];
  int src_port = stoi(argv[3]);
  const char* dst_ip = argv[4];
  int dst_port = stoi(argv[5]);

  EthReceiver receiver;

  if (!receiver.init(interface)) {
    cout << receiver.getLastError() << endl;
    return 1;
  }

  TcpStream<> stream;
  stream.initFilter(src_ip, src_port, dst_ip, dst_port);

  while (running) {
    receiver.read([&](const char* eth_data, uint32_t eth_size) {
      if (stream.filterPacket(eth_data, eth_size)) {
        stream.handlePacket(eth_data, eth_size, [](const char* tcp_data, uint32_t tcp_size) {
          cout << "got tcp payload size: " << tcp_size << endl;
          return 0;
        });
      }
    });
  }

  return 0;
}


