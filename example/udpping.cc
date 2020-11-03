#include <bits/stdc++.h>
#include "../Socket.h"
#include "timestamp.h"
#include "Statistic.h"
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
    cout << "usage: " << argv[0] << " dest_ip desp_port [pack_per_sec=1]" << endl;
    return 1;
  }

  const char* dest_ip = argv[1];
  int dest_port = stoi(argv[2]);

  int pack_per_sec = 1;
  if (argc >= 4) {
    pack_per_sec = stoi(argv[3]);
  }

  const uint64_t send_interval = 1000000000 / pack_per_sec;

  struct sockaddr_in destaddr;
  memset(&destaddr, 0, sizeof(destaddr));
  destaddr.sin_family = AF_INET; // IPv4
  destaddr.sin_port = htons(dest_port);
  inet_pton(AF_INET, dest_ip, &(destaddr.sin_addr));

  UdpReceiver receiver;
  if (!receiver.init("", "0.0.0.0", 0)) {
    cout << receiver.getLastError() << endl;
    return 1;
  }

  struct Data
  {
    uint64_t seq = 0;
    uint64_t send_time = 0;
  };

  Data send_data;

  Statistic<uint64_t> sta;
  sta.reserve(10000);

  while (running) {
    receiver.recvfrom([&sta](const char* data, uint32_t len, const struct sockaddr_in& addr) {
      uint64_t now = getns();
      Data& d = *(Data*)data;
      uint64_t latency = now - d.send_time;
      sta.add(latency);
      cout << len << " bytes from " << inet_ntoa(addr.sin_addr) << ":" << ntohs(addr.sin_port) << " seq=" << d.seq
           << " time=" << latency << " ns" << endl;
    });
    uint64_t now = getns();
    if (now - send_data.send_time > send_interval) {
      send_data.send_time = now;
      send_data.seq++;
      receiver.sendto((const char*)&send_data, sizeof(send_data), destaddr);
    }
  }
  sta.print(cout);

  return 0;
}

