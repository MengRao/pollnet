#include <bits/stdc++.h>
using namespace std;
#include "../efvitcp/TcpClient.h"

struct Conf
{
  static const uint32_t ConnSendBufCnt = 128;
  static const bool SendBuf1K = true;
  static const uint32_t ConnRecvBufSize = 40960;
  static const uint32_t MaxConnCnt = 2;
  static const uint32_t MaxTimeWaitConnCnt = 2;
  static const uint32_t RecvBufCnt = 128;
  static const uint32_t SynRetries = 3;
  static const uint32_t TcpRetries = 10;
  static const uint32_t DelayedAckMS = 10;
  static const uint32_t MinRtoMS = 100;
  static const uint32_t MaxRtoMS = 30 * 1000;
  static const bool WindowScaleOption = false;
  static const bool TimestampOption = false;
  static const int CongestionControlAlgo = 0; // 0: no cwnd, 1: new reno, 2: cubic
  static const uint32_t UserTimerCnt = 2;
  struct UserData
  {
  };
};
using TcpClient = efvitcp::TcpClient<Conf>;
using TcpConn = TcpClient::Conn;

inline int64_t getns() {
  timespec ts;
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_sec * 1000000000 + ts.tv_nsec;
}

#pragma pack(push, 1)
struct Packet
{
  int64_t ts = 0;
  int64_t val = 0;
};
#pragma pack(pop)

volatile bool running = true;

void my_handler(int s) {
  running = false;
}

class Client
{
public:
  bool init(const char* interface, const char* server_ip, uint16_t server_port) {
    const char* err = client.init(interface);
    if (err) {
      cout << err << endl;
      return false;
    }
    err = client.connect(server_ip, server_port);
    if (err) {
      cout << err << endl;
      return false;
    }
    return true;
  }

  void poll() { client.poll(*this); }

  bool bye() {
    client.poll(*this);
    if (!conn_ || conn_->isClosed()) return true;
    conn_->sendFin();
    return false;
  }

  void onConnectionRefused() { cout << "onConnectionRefused" << endl; }
  void onConnectionEstablished(TcpConn& conn) {
    conn_ = &conn;
    cout << "onConnectionEstablished" << endl;
    cout << "sendable: " << conn.getSendable() << endl;
    conn.setUserTimer(0, 10 * 1000);
    conn.setUserTimer(1, 1); // send data almost immediately with 1 ms delay
  }

  uint32_t onData(TcpConn& conn, uint8_t* data, uint32_t size) {
    auto now = getns();
    while (size >= sizeof(Packet)) {
      const Packet& recv_pack = *(const Packet*)data;
      auto lat = now - recv_pack.ts;
      cout << "recv val: " << recv_pack.val << " latency: " << lat << endl;
      if (recv_pack.val != ++last_recv_val) {
        cout << "invalid recv val: " << recv_pack.val << ", last_recv_val: " << last_recv_val << endl;
        exit(1);
      }
      data += sizeof(Packet);
      size -= sizeof(Packet);
    }
    conn.setUserTimer(0, 10 * 1000); // reset 10 secs recv timeout
    return size;
  }

  void onConnectionReset(TcpConn& conn) { cout << "onConnectionReset" << endl; }

  void onConnectionClosed(TcpConn& conn) { cout << "onConnectionClosed" << endl; }

  void onFin(TcpConn& conn, uint8_t* data, uint32_t size) {
    cout << "onFin, remaining data size:" << size << endl;
    conn.sendFin();
  }
  void onConnectionTimeout(TcpConn& conn) {
    cout << "onConnectionTimeout, established: " << conn.isEstablished() << endl;
  }

  void onMoreSendable(TcpConn& conn) {}

  void onUserTimeout(TcpConn& conn, uint32_t timer_id) {
    if (timer_id == 0) { // recv timeout
      cout << "on Recv timeout: conn_id: " << conn.getConnId() << endl;
      conn.close();
    }
    else if (timer_id == 1) { // send timeout
      Packet packs[1];
      auto now = getns();

      if (conn.getSendable() >= sizeof(packs)) {
        for (auto& pack : packs) {
          pack.val = ++last_send_val;
          pack.ts = now;
        }

        if (conn.send(packs, sizeof(packs)) != sizeof(packs)) {
          cout << "error sending, sendable: " << conn.getSendable() << endl;
          exit(1);
        }
      }
      conn.setUserTimer(1, 1000);
    }
    else {
      cout << "invalid timer_id: " << timer_id << ", conn_id: " << conn.getConnId() << endl;
      exit(1);
    }
  }

  TcpClient client;
  TcpConn* conn_ = nullptr;
  int64_t last_recv_val = 0;
  int64_t last_send_val = 0;
};

const int NCli = 100;
Client clis[NCli];

int main(int argc, const char** argv) {
  struct sigaction sigIntHandler;
  sigIntHandler.sa_handler = my_handler;
  sigemptyset(&sigIntHandler.sa_mask);
  sigIntHandler.sa_flags = 0;
  sigaction(SIGINT, &sigIntHandler, NULL);
  sigaction(SIGTERM, &sigIntHandler, NULL);
  sigaction(SIGPIPE, &sigIntHandler, NULL);

  if (argc < 4) {
    cout << "usage: " << argv[0] << " interface server_ip server_port" << endl;
    return 1;
  }
  const char* interface = argv[1];
  const char* server_ip = argv[2];
  uint16_t server_port = atoi(argv[3]);
  for (Client& cli : clis) {
    if (!cli.init(interface, server_ip, server_port)) return 1;
  }

  while (running) {
    for (int i = 0; i < NCli; i++) {
      Client& cli = clis[i];
      cli.poll();
    }
  }
  cout << "ending..." << endl;
  bool all_ended = false;
  while (!all_ended) {
    all_ended = true;
    for (Client& cli : clis)
      if (!cli.bye()) all_ended = false;
  }
  return 0;
}
