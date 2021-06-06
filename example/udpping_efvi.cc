#include <bits/stdc++.h>
#include "../Efvi.h"
#include "timestamp.h"
#include "Statistic.h"
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
    cout << "usage: " << argv[0] << " interface local_ip local_port dest_ip dest_port [pack_per_sec=1]" << endl;
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

  const uint64_t send_interval = 1000000000 / pack_per_sec;

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

  struct Data
  {
    uint64_t seq = 0;
    uint64_t send_time = 0;
  };

  Data send_data;

  Statistic<uint64_t> sta;
  sta.reserve(10000);

  while (running) {
    receiver.read([&sta](const char* data, uint32_t len) {
      uint64_t now = getns();
      Data& d = *(Data*)data;
      uint64_t latency = now - d.send_time;
      sta.add(latency);
      cout << "read " << len << " bytes,  seq=" << d.seq << " latency=" << latency << " ns" << endl;
    });
    uint64_t now = getns();
    if (now - send_data.send_time > send_interval) {
      send_data.send_time = now;
      send_data.seq++;
      sender.write((const char*)&send_data, sizeof(send_data));
    }
  }
  sta.print(cout);

  return 0;
}


