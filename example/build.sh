set -e

#SOLARFLARE_OPT="-DUSE_SOLARFLARE -lonload_zf -lciul1"
SOLARFLARE_OPT=""

g++ -O3 tcpclient.cc -o tcpclient $SOLARFLARE_OPT
g++ -O3 tcpserver.cc -o tcpserver $SOLARFLARE_OPT
g++ -O3 udpreceiver.cc -o udpreceiver $SOLARFLARE_OPT
g++ -O3 tcpsniffer.cc -o tcpsniffer $SOLARFLARE_OPT
g++ -O3 udpping.cc -o udpping $SOLARFLARE_OPT
g++ -O3 udppong.cc -o udppong $SOLARFLARE_OPT
g++ -O3 udprecv.cc -o udprecv $SOLARFLARE_OPT
g++ -O3 udpsend.cc -o udpsend $SOLARFLARE_OPT
