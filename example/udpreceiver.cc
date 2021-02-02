#include <bits/stdc++.h>

#ifdef USE_SOLARFLARE
#include "../Efvi.h"
using UdpReceiver = EfviUdpReceiver;
#else
#include "../Socket.h"
using UdpReceiver = SocketUdpReceiver<>;
#endif

#include "timestamp.h"

using namespace std;

volatile bool running = true;

void my_handler(int s) {
  running = false;
}

uint64_t midnight_ns = 0;

std::string convertTime(uint64_t ts) {
  ts -= midnight_ns;
  int ns = ts % 1000000000;
  ts /= 1000000000;
  int sec = ts % 60;
  ts /= 60;
  int min = ts % 60;
  ts /= 60;
  int hour = ts;
  std::string ret(18, 0);
  sprintf((char*)ret.data(), "%02d:%02d:%02d.%09d", hour, min, sec, ns);
  return ret;
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

  {
    time_t rawtime;
    struct tm* timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    timeinfo->tm_sec = timeinfo->tm_min = timeinfo->tm_hour = 0;
    midnight_ns = mktime(timeinfo);
    midnight_ns *= 1000000000;
  }

  UdpReceiver receiver;
  if (!receiver.init(interface, dest_ip, dest_port, sub_ip)) {
    cout << receiver.getLastError() << endl;
    return 1;
  }

  while (running) {
    receiver.recvfrom([](const char* data, uint32_t len, const struct sockaddr_in& addr) {
      auto now = getns();

      cout << "now: " << convertTime(now) << ", got data size: " << len << " from " << inet_ntoa(addr.sin_addr) << ":"
           << ntohs(addr.sin_port) << endl;
    });
  }

  return 0;
}


