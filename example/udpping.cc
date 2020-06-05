#include <bits/stdc++.h>
#include "../Socket.h"
#include "timestamp.h"
using UdpReceiver = SocketUdpReceiver;

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

  if (argc < 5) {
    cout << "usage: " << argv[0] << " local_ip local_port dest_ip desp_port" << endl;
    return 1;
  }

  const char* local_ip = argv[1];
  int local_port = stoi(argv[2]);
  const char* dest_ip = argv[3];
  int dest_port = stoi(argv[4]);

  struct sockaddr_in destaddr;
  memset(&destaddr, 0, sizeof(destaddr));
  destaddr.sin_family = AF_INET; // IPv4
  destaddr.sin_port = htons(dest_port);
  inet_pton(AF_INET, dest_ip, &(destaddr.sin_addr));

  UdpReceiver receiver;
  if (!receiver.init("", local_ip, local_port)) {
    cout << receiver.getLastError() << endl;
    return 1;
  }

  struct Data
  {
    uint64_t seq = 0;
    uint64_t send_time = 0;
  };

  Data send_data;

  while (running) {
    receiver.recvfrom([](const char* data, uint32_t len, const struct sockaddr_in& addr) {
      uint64_t now = getns();
      Data& d = *(Data*)data;
      uint64_t latency = now - d.send_time;
      cout << len << " bytes from " << inet_ntoa(addr.sin_addr) << ":" << ntohs(addr.sin_port) << " seq=" << d.seq
           << " time=" << latency << " ns" << endl;
    });
    uint64_t now = getns();
    if (now - send_data.send_time > 1000000000) {
      send_data.send_time = now;
      send_data.seq++;
      receiver.sendto((const char*)&send_data, sizeof(send_data), destaddr);
    }
  }

  return 0;
}

