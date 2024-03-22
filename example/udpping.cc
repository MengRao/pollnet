#include <bits/stdc++.h>

#ifdef USE_EFVI
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
    cout << "usage: " << argv[0]
         << " interface local_ip local_port dest_ip desp_port [batch_per_sec=1] [batch_size=1]"
         << endl;
    return 1;
  }

  const char* interface = argv[1];
  const char* local_ip = argv[2];
  int local_port = stoi(argv[3]);
  const char* dest_ip = argv[4];
  int dest_port = stoi(argv[5]);
  int batch_per_sec = 1;
  int batch_size = 1;
  if (argc >= 7) {
    batch_per_sec = stoi(argv[6]);
  }
  if (argc >= 8) {
    batch_size = stoi(argv[7]);
  }

  const uint64_t send_interval = 1000000000 / batch_per_sec;

  UdpReceiver receiver;
  UdpSender sender;
  if (!receiver.init(interface, local_ip, local_port)) {
    cout << receiver.getLastError() << endl;
    return 1;
  }

  if (!sender.init(interface, local_ip, local_port + 1, dest_ip, dest_port)) {
    cout << sender.getLastError() << endl;
    return 1;
  }

  struct Data
  {
    uint64_t seq = 0;
    uint64_t send_time = 0;
    uint64_t arr[8];
  };

  Data send_data;
  /*
  for (int i = 0; i < 10; i++) {
    send_data.arr[i] = i;
  }
  */

  Statistic<uint64_t> sta, write_sta;
  sta.reserve(10000);
  write_sta.reserve(10000);

  uint64_t last_recv_seq = 0;
  uint64_t miss_seq_cnt = 0;
  uint64_t bad_cnt = 0;
  while (running) {
    receiver.read([&](const uint8_t* data, uint32_t len) {
      uint64_t now = getns();
      if (len != sizeof(Data)) {
        bad_cnt++;
        return;
      }
      Data& d = *(Data*)data;
      uint64_t latency = now - d.send_time;
      sta.add(latency);
      if (d.seq != last_recv_seq + 1) {
        cout << "abnormal seq: " << d.seq << ", last_recv_seq: " << last_recv_seq << endl;
        miss_seq_cnt++;
      }

      last_recv_seq = d.seq;
    });
    uint64_t now = getns();
    if (now - send_data.send_time > send_interval) {
      for (int i = 0; i < batch_size; i++) {
        send_data.send_time = now;
        send_data.seq++;
        sender.write((const uint8_t*)&send_data, sizeof(send_data));
        uint64_t after = getns();
        write_sta.add(after - now);
        now = after;
      }
    }
  }
  sta.print(cout);

  cout << "write latency: " << endl;
  write_sta.print(cout);

  cout << "miss_seq_cnt: " << miss_seq_cnt << ", bad_cnt: " << bad_cnt << endl;

  return 0;
}

