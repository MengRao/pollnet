#include <bits/stdc++.h>
#include "EfviUdpClient.h"
#include "timestamp.h"

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

  if (argc < 7) {
    cout << "usage: " << argv[0] << " interface local_ip local_port dest_ip dest_port dest_mac" << endl;
    exit(1);
  }
  const char* interface = argv[1];
  const char* local_ip = argv[2];
  int local_port = stoi(argv[3]);
  const char* dest_ip = argv[4];
  int dest_port = stoi(argv[5]);
  const char* dest_mac = argv[6];

  EfviUdpClient client;
  if (!client.init(dest_ip, dest_port, interface, local_ip, dest_mac)) {
    cout << client.getLastError() << endl;
    return 1;
  }

  static const int BUF_LEN = 1400;
  char buf[BUF_LEN];
  int send_seq = 0;
  uint64_t last_sendtime = 0;

  while (running) {
    // client.read();
    uint64_t now = getns();
    if (now - last_sendtime > 10000000) {
      last_sendtime = now;
      send_seq++;
      if (send_seq > BUF_LEN) send_seq = BUF_LEN;

      client.write(buf, send_seq);
    }
  }

  return 0;
}



