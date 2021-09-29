## pollnet
Pollnet contains some header only network libs for tcp, udp or ethernet data processing on Linux. Most of its APIs are non-blocking nor event-driven for simplicity and low-latency purposes, which requires the user to poll the lib frequently to receive new data from the network. 

Another important feature of pollnet is that it supports low level apis for solarflare network adapters: Tcpdirect and Efvi, providing the same interface as its Socket counterparts, allowing the user to easily switch between Socket and Tcpdirect/Evfi implementations depending on whether or not solarflare NIC is being used.

## TCP
TcpConnection, TcpClient and TcpServer classes are implemented in both Socket and Tcpdirect versions.


TcpConnection:
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

// receive new data without blocking
// Handler is of signature uint32_t (const char* data, uint32_t size), where the return value is remaining size which will be prepended to new received data in the next read handler
// return true if new data is received and handler has been called
template<typename Handler>
bool read(Handler handler);
```

The most interesting part here is the `read` function, it takes a template parameter handler which can be a lambda function. Below is a simple example showing how to read and handle fix-sized application packets:
```c++
struct Packet{
...
};

connection.read([](const char* data, uint32_t size) {
      while (size >= sizeof(Packet)) {
        const Packet& pack = *(const Packet*)data;
        // handle pack...
        
        data += sizeof(Packet);
        size -= sizeof(Packet);
      }
      return size;
    });
```

Another thing to note is that pollnet won't print or log any error msg internally, which requires the user to fetch it by `getLastError()` whenever an error occurred.


TcpClient is a derived class from TcpConnection:
```c++

// connect to server, may block.
// interface can be empty for Socket version
// return true if isConnected()
// else call getLastError()
bool connect(const char* interface, const char* server_ip, uint16_t server_port);
```

TcpServer creates new TcpConnections instead:
```c++

// interface_name can be empty for Socket version
// call getLastError() if return false
bool init(const char* interface_name, const char* server_ip, uint16_t server_port);

using TcpConnectionPtr = std::unique_ptr<TcpConnection>;

// try accepting new connections, non-blocking.
// if no new connection it returns TcpConnectionPtr().
TcpConnectionPtr accept();

// similar to accept, but new connection is set to the parameter: a closed connection
// return true if new connection is accepted
bool accept2(TcpConnection& conn);
```

Note that TcpClient and TcpServer has a template parameter `uint32_t RecvBufSize = 4096`, RecvBufSize must be as least twice the largest application packet its connection is going to receive.

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
// Handler is of signature void (const char* data, uint32_t size) where data is raw ethernet packet with header
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
#ifdef USE_SOLARFLARE
#include "../Tcpdirect.h"
using TcpClient = TcpdirectTcpClient<>;
#else
#include "../Socket.h"
using TcpClient = SocketTcpClient<>;
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
  TcpClient<4096> client;
  ...
};
...

#include "MyApp.h"
#include "pollnet/Tcpdirect.h"
#include "pollnet/Socket.h"

BaseApp* app;
if(Config()::useSolarFlare()){
  app = new MyApp<TcpdirectTcpClient>();
}
else {
  app = new MyApp<SocketTcpClient>();
}
app->run();
```

## Thread safety
Pollnet libs are not designed to be thread safe, mainly because tcpdirect/efvi are not thread safe. 

Additional note for `TcpdirectTcpServer`: different TcpConnections accepted from the same server can't be accessed in multiple threads without locking, because they're sharing the same stack managed by the server, for the same reason, all connections must be closed before the server itself is closed.
