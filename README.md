## pollnet
Pollnet contains some header only network libs for tcp, udp or ethernet data processing on Linux. Most of its APIs are non-blocking nor event-driven for simplicity and low-latency purposes, which requires the user to poll the lib frequently to receive new data from the network. 

Another important feature of pollnet is that it supports low level apis for solarflare network adapters: Tcpdirect and Efvi, providing the same interface as its Socket counterparts, allowing the user to easily switch between Socket and Tcpdirect/Evfi implementations depending on whether or not solarflare NIC is being used.

## TCP
TcpConnection, TcpClient and TcpServer classes are implemented in Socket, Tcpdirect and Efvi versions.

TcpConnection can't be created directly by the user, but user can access its reference in client/server's callback function parameter as onXXX(TcpClient::Conn& conn) or onXXX(TcpServer::Conn& conn). TcpConnection has below functions:
```C++
// get last error msg for printing/logging
const char* getLastError();

// close this connection with a reason msg that can be fetched later by getLastError();
void close(const char* reason);

// check if this connection is alive
// call getLastError() if not
bool isConnected();

// get remote address
bool getPeername(struct sockaddr_in& addr);

// send data, might block because it retries sending until all data has been accepted or an error occurred.
bool write(const char* data, uint32_t size);

// send data without retry, thus wouldn't block. If data can not be accepted immediately it simply closes the connection.
bool writeNonblock(const char* data, uint32_t size);
```
One thing to note is that pollnet won't print or log any error msg internally, which requires the user to fetch it by `getLastError()` whenever an error occurred.


TcpClient and TcpServer are template classes that take a parameter for configuration, for example:
```c++
struct ClientConf
{
  // receive buffer size for each connection
  static const uint32_t RecvBufSize = 4096;
  // connect retry periods in seconds
  static const uint32_t ConnRetrySec = 5;
  // send timeout in seconds for each connection, set to 0 to disable timeout
  static const uint32_t SendTimeoutSec = 1;
  // receive timeout in seconds for each connection, set to 0 to disable timeout
  static const uint32_t RecvTimeoutSec = 3;
  // user defined data attached to each connection
  struct UserData 
  {
      int foo;
      std::string bar;
  };
};
using TcpClient = SocketTcpClient<ClientConf>;

struct ServerConf
{
  static const uint32_t RecvBufSize = 4096;
  // max number of connections the server can support
  static const uint32_t MaxConns = 10;
  static const uint32_t SendTimeoutSec = 0;
  static const uint32_t RecvTimeoutSec = 10;
  struct UserData
  {
    int foo;
    std::string bar;
  };
};
using TcpServer = SocketTcpServer<ServerConf>;

```
TcpClient and TcpServer also have similar user interfaces: first call `init()` to initialize the it:
```c++
// interface can be empty for Socket version
bool init(const char* interface, const char* server_ip, uint16_t server_port);
```

Then call `poll(Handler& handler)` repetitively to get it running, in which `TcpClient` automatically tries connecting and `TcpServer` tries accepting. `poll()` takes an user defined object for handling network/timer events, all defined callback functions(non-virtual) in both classes are as follows:
```c++
// When a new connection is estasblished
void onTcpConnected(Conn& conn){}

// When user has not sent any data to the connection for some period(defined in Conf::SendTimeoutSec)
void onSendTimeout(Conn& conn){}

// When the connection has not received any data for some period(defined in Conf::RecvTimeoutSec)
void onRecvTimeout(Conn& conn){}

// when new tcp data arrives.
// return the remaining data size that user can't handle currently, which will be returned in the next onTcpData with newly arrived data
uint32_t onTcpData(Conn& conn, const uint8_t* data, uint32_t size){}

// when the connection is closed
void onTcpDisconnect(Conn& conn){}
```
User can save the reference to the conn after it's established for sending data etc..., but for TcpServer user should not use it after it's been closed because the same connection object will be reused for new connections.

TcpClient has one additional callback function for `poll`:
```c++
// when connect failed, it will retry connecting after some period(defined in Conf::ConnRetrySec)
void onTcpConnectFailed(){}
```
While TcpServer has more functions:
```c++
// get total active connection number(for efvi vertion, it also includes SYN-RECEIVED connections)
uint32_t getConnCnt();

// visit all established connections, handler is of signature: void (TcpServer::Conn& conn).
template<typename Handler>
void foreachConn(Handler handler);
```

Note that Efvi tcp implementation provides more functionalities for finer controls, check [efvitcp](https://github.com/MengRao/pollnet/tree/master/efvitcp) for details.

Check [tcpclient.cc](https://github.com/MengRao/pollnet/blob/master/example/tcpclient.cc) and [tcpserver.cc](https://github.com/MengRao/pollnet/blob/master/example/tcpserver.cc) for code examples.

## UdpReceiver
UdpReceiver provides Socket and Efvi versions, with multicast support:
```c++
// interface can be empty for Socket version.
// specify subscribe_ip (to the ip of the interface) to join multicast group if multicast msgs are to be received
bool init(const char* interface, const char* dest_ip, uint16_t dest_port, const char* subscribe_ip = "");

// receive new data without blocking
// Handler is of signature void (const char* data, uint32_t size).
template<typename Handler>
bool read(Handler handler);

// receive new data and get remote address without blocking
// Handler is of signature void (const char* data, uint32_t size, const struct sockaddr_in& addr), where addr is the remote address.
template<typename Handler>
bool recvfrom(Handler handler);
```

## UdpSender
UdpSender provides Socket and Efvi versions, with multicast support:
```c++
// interface can be empty for Socket version.
bool init(const char* interface, const char* local_ip, uint16_t local_port, const char* dest_ip, uint16_t dest_port);

// send one udp packet with payload
bool write(const char* payload, uint32_t size);
```

## EthReceiver and TcpStream
In some cases we need to capture and process raw packets from the network that are not destined to our host by means of TAP or SPAN. 

EthReceiver can capture/sniff ethernet packets received on a NIC port and requires root permission(similar to tcpdump), it has both Socket and Efvi versions:
```c++
bool init(const char* interface);

// receive ethernet packet without blocking
// Handler is of signature: void (const char* data, uint32_t size) where data is raw ethernet packet with header
template<typename Handler>
bool read(Handler handler);

```

TcpStream is a tool for filtering and reconstructing unidirectional tcp stream from raw ethernet packets, allowing the user to process tcp payload as if it's from a TcpConnection. TcpStream can be used in conjunction with EthReceiver or any pcap readers.
```c++
// init filter based on src and dst addresses
// use "0.0.0.0" for wildcard ip and 0 for wildcard port
void initFilter(const char* src_ip, uint16_t src_port, const char* dst_ip, uint16_t dst_port);

// check if this raw packet matches the filter
bool filterPacket(const char* data, uint32_t size);

// handle a raw packet of data,size
// the signature of Handler is the same as that of TcpConnection.read()
template<typename Handler>
bool handlePacket(const char* data, uint32_t size, Handler handler);
```
Note that TcpStream has a template parameter `WaitForResend = false`, which indicates whether to wait for resend data once data drop is detected. If set to true, use will get intact tcp stream. But sometimes this is impossible because e.g. switch could drop packets on mirror port which would never be resent. Setting WaitForResend to false adds packets drop tolerance and also timeliness for new data handling. Once packet drop is detected, any remaining data is also discarded and new data is feed to user immediately, thus user must be ready for handling stream of data where segments could be missing between different calls of `handlePacket`.

## EFVI Ping
An EFVI implemented ping program intended to provide lower rtt latency and higher precision(in nanoseconds).

Usage: `./efvi_ping dest_ip [pack_per_sec=1]`, note that root permission is required. Example: 

![image](https://user-images.githubusercontent.com/11496526/135202450-65a8435d-70fa-45e6-a2a5-fb4abf0636a3.png)



## How to switch between different implementations
In examples, this is done via macro selections so that tcpdirect/efvi headers will not be included to prevent from compile errors on machines where onload is not installed.
```c++
struct ClientConf
{
  static const uint32_t RecvBufSize = 4096;
  static const uint32_t ConnRetrySec = 5;
  static const uint32_t SendTimeoutSec = 1;
  static const uint32_t RecvTimeoutSec = 3;
  struct UserData
  {
  };
};

#ifdef USE_TCPDIRECT
#include "../Tcpdirect.h"
using TcpClient = TcpdirectTcpClient<ClientConf>;
#else
#ifdef USE_EFVI
#include "../efvitcp/EfviTcp.h"
using TcpClient = EfviTcpClient<ClientConf>;
#else
#include "../Socket.h"
using TcpClient = SocketTcpClient<ClientConf>;
#endif
#endif

```

User can also use template parameter to choose implementation in the run time without performance compromise:
```c++

template<typename TcpClient>
class MyApp: public BaseApp
{
public:
  virtual void run() override;
  ...
  
private:
  struct ClientConf{
    ...
  };
  TcpClient<ClientConf> client;
  ...
};
...

#include "MyApp.h"
#include "pollnet/Tcpdirect.h"
#include "pollnet/Socket.h"
#include "pollnet/efvitcp/EfviTcp.h"

BaseApp* app;
if(Config()::useTcpdirect()){
  app = new MyApp<TcpdirectTcpClient>();
}
else if(Config()::useEfvi()){
  app = new MyApp<EfviClient>();
}
else {
  app = new MyApp<SocketTcpClient>();
}
app->run();
```

## Thread safety
Pollnet libs are not designed to be thread safe, mainly because tcpdirect/efvi are not thread safe. 
