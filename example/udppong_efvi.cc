#include <bits/stdc++.h>
#include "../Efvi.h"
#include "timestamp.h"
using UdpReceiver = EfviUdpReceiver;
using UdpSender = EfviUdpSender;

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
    cout << "usage: " << argv[0] << " interface local_ip local_port dest_ip dest_port" << endl;
    return 1;
  }
  const char* interface = argv[1];
  const char* local_ip = argv[2];
  int local_port = stoi(argv[3]);
  const char* dest_ip = argv[4];
  int dest_port = stoi(argv[5]);

  UdpReceiver receiver;
  UdpSender sender;
  if (!receiver.init(interface, local_ip, local_port)) {
    cout << receiver.getLastError() << endl;
    return 1;
  }
  if (!sender.init(interface, local_ip, local_port, dest_ip, dest_port)) {
    cout << sender.getLastError() << endl;
    return 1;
  }

  while (running) {
    receiver.read([&](const char* data, uint32_t len) { sender.write(data, len); });
  }

  return 0;
}



