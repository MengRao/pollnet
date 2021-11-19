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

  if (argc < 4) {
    cout << "usage: " << argv[0] << " interface local_ip local_port [sub_ip]" << endl;
    return 1;
  }

  const char* interface = argv[1];
  const char* local_ip = argv[2];
  int local_port = stoi(argv[3]);
  const char* sub_ip = "";
  if (argc > 4) {
    sub_ip = argv[4];
  }

  UdpReceiver receiver;
  if (!receiver.init(interface, local_ip, local_port, sub_ip)) {
    cout << receiver.getLastError() << endl;
    return 1;
  }

  int cur = 0;
  int bad_cnt = 0;
  int miss_cnt = 0;
  int cnt = 0;
  while (running) {
    receiver.read([&](const uint8_t* data, uint32_t len) {
      cnt++;
      // cout << "data: " << data << endl;
      if (len != 201) {
        cout << "error len: " << len << endl;
        bad_cnt++;
        return;
      }
      while (data[0] != cur + '0') {
        miss_cnt++;
        cout << "miss: " << (char)(cur + '0') << endl;
        cur++;
        if (cur == 10) cur = 0;
      }
      for (int i = 0; i < 200; i++) {
        if (data[i] != data[0]) {
          cout << "invalid data: " << data << endl;
          bad_cnt++;
          break;
        }
      }
      cur++;
      if (cur == 10) cur = 0;
    });
  }
  cout << "cnt: " << cnt << ", miss_cnt: " << miss_cnt << ", bad_cnt: " << bad_cnt << endl;

  return 0;
}


