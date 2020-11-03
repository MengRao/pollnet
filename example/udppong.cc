#include <bits/stdc++.h>
#include "../Socket.h"
#include "timestamp.h"
using UdpReceiver = SocketUdpReceiver<>;

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
    cout << "usage: " << argv[0] << " local_ip local_port" << endl;
    return 1;
  }

  const char* local_ip = argv[1];
  int local_port = stoi(argv[2]);

  UdpReceiver receiver;
  if (!receiver.init("", local_ip, local_port)) {
    cout << receiver.getLastError() << endl;
    return 1;
  }

  while (running) {
    receiver.recvfrom(
      [&](const char* data, uint32_t len, const struct sockaddr_in& addr) { receiver.sendto(data, len, addr); });
  }

  return 0;
}


