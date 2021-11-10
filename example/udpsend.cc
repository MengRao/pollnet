#include <bits/stdc++.h>

#ifdef USE_SOLARFLARE
#include "../Efvi.h"
using UdpReceiver = EfviUdpReceiver;
using UdpSender = EfviUdpSender;
#else
#include "../Socket.h"
using UdpReceiver = SocketUdpReceiver<>;
using UdpSender = SocketUdpSender;
#endif
#include "timestamp.h"
#include "Statistic.h"

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
    cout << "usage: " << argv[0] << " interface local_ip local_port dest_ip desp_port [pack_per_sec=1]" << endl;
    return 1;
  }

  const char* interface = argv[1];
  const char* local_ip = argv[2];
  int local_port = stoi(argv[3]);
  const char* dest_ip = argv[4];
  int dest_port = stoi(argv[5]);
  int pack_per_sec = 1;
  if (argc >= 7) {
    pack_per_sec = stoi(argv[6]);
  }

  const int64_t send_interval = 1000000000 / pack_per_sec;

  UdpSender sender;

  if (!sender.init(interface, local_ip, local_port + 1, dest_ip, dest_port)) {
    cout << sender.getLastError() << endl;
    return 1;
  }

  uint8_t data[201];
  data[200] = 0;

  int64_t send_time = 0;
  while (running) {
    int64_t now = getns();
    if (now - send_time > send_interval) {
      send_time = now;
      for (int _ = 0; _ < 10; _++) {
        for (int i = 0; i < 10; i++) {
          for (int j = 0; j < 20; j++) {
            memset(data + j * 10, '0' + i, 10);
          }
          // memset(data, '0' + i, 200);
          sender.write(data, sizeof(data));
          auto expire = getns() + 20;
          while (getns() < expire)
            ;
        }
      }
    }
  }

  return 0;
}

