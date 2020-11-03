#include <bits/stdc++.h>

#ifdef USE_SOLARFLARE
#include "../Efvi.h"
using UdpReceiver = EfviUdpReceiver;
#else
#include "../Socket.h"
using UdpReceiver = SocketUdpReceiver<>;
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

  if (argc < 4) {
    cout << "usage: " << argv[0] << " interface dest_ip dest_port [sub_ip]" << endl;
    exit(1);
  }
  const char* interface = argv[1];
  const char* dest_ip = argv[2];
  int dest_port = stoi(argv[3]);
  const char* sub_ip = nullptr;
  if (argc >= 5) {
    sub_ip = argv[4];
  }

  UdpReceiver receiver;
  if (!receiver.init(interface, dest_ip, dest_port, sub_ip)) {
    cout << receiver.getLastError() << endl;
    return 1;
  }

  while (running) {
    receiver.recvfrom([](const char* data, uint32_t len, const struct sockaddr_in& addr) {
      cout << "got data size: " << len << " from " << inet_ntoa(addr.sin_addr) << ":" << ntohs(addr.sin_port) << endl;
    });
  }

  return 0;
}


