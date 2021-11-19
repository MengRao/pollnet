# Efvitcp
Efvitcp is a tcp library using Solarflare ef_vi interface on linux, and also a tcp multiplexing framework for both C++ client and server program.

As ef_vi is a low-level interface passing Ethernet frames between applications and the network, efvitcp implements full tcp/ip stack on its own in user space while also taking advantage of ef_vi's zero-copy buffer delivering feature.

On the user interface, efvitcp is a header-only C++ template for tcp client and server, using reactor model(like select/poll/epoll) and handling events for multiple connections in a single call. 

It opens a fast path directly between the user and the NIC, providing extremely efficient data processing performance. For example, on frame arrives the tcp payload memory could be directly handed over to the user which is written by the NIC on a DMA buffer.

## Why not use Tcpdirect?
While tcpdirect also provides a tcp implementation based on ef_vi, it suffers from the maximum PIO number limitation the solarflare NIC provides: there can be no more than 12 instances on a host(onload is sharing the same PIO resource which makes it worse). And tcpdirect performs poorly in performance when onload is also in use, which I don't know why.
These problems make tcpdirect hard to use in practice, so an alternative approach is needed to make full use of solarflare NIC's low-latency capacity for tcp applications.

## Features
* Fast - it makes good use of solarflare's low-latency features including CTPIO, it's even faster than tcpdirect especially for large segments.
* Headers only and no third-party dependences(except for ef_vi).
* Reactor model with rich network and time events, giving finer control over a tcp connection.
* Highly configurable in compile-time.
* Non-blocking - none of the apis blocks.
* No thread created internally
* Support user timers on each connection.

## Platform
Linux and C++11 is required.

## Usage
efvitcp provides two class templates for tcp client and server: `efvitcp::TcpClient<Conf>` and `efvitcp::TcpServer<Conf>`, both taking a template argument used for configuration in compile time(we'll cover it later). 

Take client for example, an instance can be declared as below:
```c++
struct Conf {
...
};
using TcpClient = efvitcp::TcpClient<Conf>;
using TcpConn = TcpClient::Conn;

TcpClient client;
```
At first we need to call `init(interface)` for initialization:
```c++
const char* err = client.init("p1p1");
if (err) {
  cout << "init failed: " << err << endl;
}
else {
  cout << "init ok" << endl;
}
```
To create a tcp connection we call `connect(server_ip, server_ip[, local_port=0])`:
```c++
if((err = client.connect("192.168.20.15", 1234)) {
  cout << "connect failed: " << err << endl;
}
else {
  cout << "connect ok" << endl;
}
```
Note that `connect()` not returning an error doesn't mean the connection is established: it just indicate that a Syn is sent out without error(as it's non-blocking), we still need to wait for the 3-way handshake to complete. As efvitcp won't create a thread internally, we need to `poll()` it in the user thread, and as `poll()` is also non-blocking, we need to call it **repetitively**. 

`poll()` is the core of muliplexing in the lib, all sorts of events will be triggers by this single call including whether or not the `connect()` is successful. How can user retrieve the events from `poll()`? A class with event handler functions(non-virtual) need to be defined and an instance be provided as the argument of `poll()`, and it'll call those user defined callback functions when events occur. For completing a connection establishment, three events could be triggered:

```c++
struct MyClient {
  void onConnectionRefused() { 
    err = "onConnectionRefused"; 
  }
  void onConnectionTimeout(TcpConn& conn) { 
    err = "onConnectionTimeout"; 
  }
  void onConnectionEstablished(TcpConn& conn) { 
    pc = &conn;
  }
  ...
  
  const char* err = nullptr;
  TcpConn* pc = nullptr;
} my_client;

while(!my_client.err && !my_client.pc){
  client.poll(my_client);
}
if(my_client.err) {
  cout << my_client.err << endl;
}
else {
  cout << "connection established" << endl;
}
```
Once connection is established on the callback `onConnectionEstablished`, we can save the reference to the `TcpConn` parameter, because all operations on an established connection will be called on this object.

For sending data, two functions of `TcpConn` can be used:
```c++
// more is like MSG_MORE flag in socket, indicating there're pending sends
uint32_t send(const void* data, uint32_t size, bool more = false);
uint32_t sendv(const iovec* iov, int iovcnt);
```
Both calls return the number of bytes accepted(like socket send/writev call). User can also know previously the maxmium bytes the next send/sendv can accept by `getSendable()`.
Note that data being accepted doesn't mean it has been sent out, it(or part of it) may be buffered to be sent later due to tcp flow control or congestion control limitations. Actually we can get the immediately sendable bytes in the next send/sendv by `getImmediatelySendable()`. 

When connection send buffer is full of unacked data, no more data will be sendable, user has to wait for new data to be acked for more sendable, there is a specific event assiciated with this need:
```c++
void onMoreSendable(TcpConn& conn) {
  cout << "senable: "<< conn.getSendable() << endl;
}
```

On the receiver side, when new data is received below event will occur:
```c++
uint32_t onData(TcpConn& conn, uint8_t* data, uint32_t size) {
  while (size >= sizeof(Packet)) {
    // handle Packet...
    
    data += sizeof(Packet);
    size -= sizeof(Packet);
  }
  return size;
}
```
`onData()` callback returns the remaining size not consumed by the user, probably because only a part of application packet has been recevied and more data is needed to proceed. In this case user don't need to buffer the partial data himself but just return the remaining size. When new data is avaialble, it will be appended to the old data and handed together to the user in the next `onData()`, this allows for elegant data processing code just as above.

If user is done with sending and want to start or continue with the 4-way handshake closure, `sendFin()` can be called, this is analogous to `shutdown(fd, SHUT_WR)`. On the other hand, when a connection received a Fin(the remote side is done sending), below event will trigger:
```c++
void onFin(TcpConn& conn, uint8_t* data, uint32_t size) {}
```
where `data` and `size` are the last unconsumed data and `size` could be 0.

When the 4-way handshake is complete and the connection enters into either CLOSED or TIME_WAIT state, below event will trigger:
```c++
void onConnectionClosed(TcpConn& conn){}
```

If user doesn't want to go through the elegant tcp closure procedure and wish to close the connection immediately, he can call `close()` on the `TcpConn` and a Rst will be sent out and the connection goes into CLOSED status. On receiving a Rst, below event will trigger:
```c++
void onConnectionReset(TcpConn& conn) {}
```
and the connection is closed. In addition, the `onConnectionTimeout(TcpConn& conn)` event can also occur on an established connection, indicating data retransmission time has exceeded some limit and the connection is automatically closed.

Note that after a connection is closed(either by active `close()` or on events `onConnectionClosed`/`onConnectionReset`/`onConnectionTimeout`) the connection reference should not be used because it could be reused for a new connection.

Lastly, efvitcp allows for user timers on a connection basis, and each connection can have multiple user timers concurrently. This is done by `void setUserTimer(uint32_t timer_id, uint32_t duration)` function on `TcpConn`, where `timer_id` is the timer type starting from 0, with different timers having different `timer_id`. `duration` is the timeout in milliseconds, currently the maximum possible duration is 65000(65 seconds). Setting a duration of 0 will delete a specific timer. When the connection is closed all users timers are deleted as well. When user timer expires below event will trigger:
```c++
void onUserTimeout(TcpConn& conn, uint32_t timer_id) {}
```

Some other information user can get from a TcpConn what may be helpful:
* A variable `user_data` of user defined type. It can be any user data attached to a connection.
* `uint32_t getConnId()`: Get connection ID starting from 0, the ID is auto assigned by the lib like fd in linux, it can be reused after the connection is closed.
* `void getPeername(struct sockaddr_in& addr)`: get network address of the remote peer.
* `bool isEstablished()`: Check if the connection is established, it may be useful in `onConnectionTimeout()` callback because it can ocurr in both unestablished and established connection.
* `bool isClosed()`: Check if the connection is closed, if so the connection should not be used.

The interfaces of TcpServer template is very similar to that of TcpClient, with below differeces:
* The `connect()` function is replaced by `const char* listen(uint16_t server_port)`
* The `onConnectionRefused()` event is replaced by `bool allowNewConnection(uint32_t ip, uint16_t port_be)`, this new event occurs when a new connection is being established(in Syn-Received state) and the user can decide whether to accept it or not according to its remote ip and port.

## Configurations
Below is an example of configuration struct of TcpServer, and TcpClient uses almost the same config, we'll go through them one by one.
```c++
struct Conf
{
  static const uint32_t ConnSendBufCnt = 512;
  static const bool SendBuf1K = true;
  static const uint32_t ConnRecvBufSize = 40960;
  static const uint32_t MaxConnCnt = 200;
  static const uint32_t MaxTimeWaitConnCnt = 100;
  static const uint32_t RecvBufCnt = 512;
  static const uint32_t SynRetries = 3;
  static const uint32_t TcpRetries = 10;
  static const uint32_t DelayedAckMS = 10;
  static const uint32_t MinRtoMS = 100;
  static const uint32_t MaxRtoMS = 30 * 1000;
  static const bool WindowScaleOption = false;
  static const bool TimestampOption = false;
  static const int CongestionControlAlgo = 0; // 0: no cwnd, 1: new reno, 2: cubic
  static const uint32_t UserTimerCnt = 1;
  using UserData = char;
};
```
* `uint32_t ConnSendBufCnt`: The number of DMA send buffers in one connection. Send buffer is used to hold unacked data in case it need to be resent, if the buffer is full no more data can be sendable.
* `bool SendBuf1K`: Whether to use 1024 bytes for a DMA send buffer. If set to false, 2048 bytes is used. ef_vi requires that a DMA buffer be in a 4096 aligned block, and as typical MTU is 1500 bytes, using 2048 sized buffer will have 25% memeory wasted. So setting `SendBuf1K` to true will maximize memory unilization but reducing the max SMSS to 960 bytes.
* `uint32_t ConnRecvBufSize`: Receving buffer size in bytes for each connection. It determines the maximum receive window advertised to the remote peer, and also used to hold unconsumed data(by remaining size returned by onData) and out of sequence data received.
* `uint32_t MaxConnCnt`: Max number of connections supported. This is used only by TcpServer.
* `uint32_t MaxTimeWaitConnCnt`: Max number of TIME_WAIT connections supported. Note that TIME_WAIT connection is not included in `MaxConnCnt`. A TIME_WAIT connection uses much less memory than a normal connection. Also note that efvitcp doesn't distinguish between active and passive closing, and all connections completing the 4-way handshake will enter into TIME_WAIT.
* `uint32_t RecvBufCnt`: The number of DMA receive buffers(one is 2048 bytes) shared by all connections. These DMA buffers are used to transfer network frames received by the NIC to the application, once efvitcp got a filled buffer it can be reused again immediately, so this config don't need to be large, typically 512 is a good default value, the maxmium number ef_vi can support is 4095.
* `uint32_t SynRetries`: Max number of SYN resents for an unestablished connection. When breached, onConnectionTimeout will be triggered.
* `uint32_t TcpRetries`: Max number of data segment resents for an established connection. When breached, onConnectionTimeout will be triggered.
* `uint32_t DelayedAckMS`: Timeout of delayed ack. Setting to 0 will disable delayed ack.
* `uint32_t MinRtoMS`: Mininum retransmission timeout.
* `uint32_t MaxRtoMS`: Maximum retransmission timeout.
* `bool WindowScaleOption`: Whether or not to enable tcp window scale option. This option is useful if window size could be larger than 65535 in either peer.
* `bool TimestampOption`: Whether or not to enable tcp timestamp option. This option can be used to update rtt more precisely, recognize old duplicate packets more accurately and PAWS(Protection Against Wrapped Sequences). if `WindowScaleOption` is used, `TimestampOption` should also be enabled.
* `int CongestionControlAlgo`: The congestion control algorithm to use. There're three options available: "0": no cwnd; "1": new reno; "2": cubic. The "on cwnd" option is almost equal to no congestion control, but fast retransmission is still used: the first unacked segment will be resent immediately on 3 duplicate acks or a partial ack in recover.
* `uint32_t UserTimerCnt`: The number of user timers per connection. The timer_id must be less than this value.
* `using UserData`: User defined type attached to each connection, which can be accessed by conn.user_data.

## Memory Overhead
Efvitcp won't dynamically allocate memory, all memoery it uses is in the object user defines, so it's pretty easy to check the memory overhead of efvitcp:
```c++
// using the Conf defined in the above example
using TcpServer = efvitcp::TcpServer<Conf>;

cout << sizeof(TcpServer) << endl;

// output: 114192400
```

## Thread Safety
Efvitcp is not thread safe, user should have the same thread polling TcpClient/TcpServer and operating on TcpConns. Multi-threading communication techniques can be used to pass data among the network thread and data processing threads.

## Pollnet Interface
Efvitcp also provides a wrapper class `EfviTcpClient` using the [pollnet](https://github.com/MengRao/pollnet) interface, so users can easily switch tcp client underlying implemenation among Socket/Tcpdirect/Efvi with same application code. Currently EfviTcpServer pollnet api is not implemented because of different multiplexing mechanism.

## Examples
Check [test](https://github.com/MengRao/efvitcp/tree/main/test) for a tcp echo client/server code example.
