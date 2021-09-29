set -e

#SOLARFLARE_OPT="-DUSE_SOLARFLARE -lonload_zf -lciul1"
SOLARFLARE_OPT=""

g++ -O3 -Wall tcpclient.cc -o tcpclient $SOLARFLARE_OPT
g++ -O3 -Wall tcpserver.cc -o tcpserver $SOLARFLARE_OPT
g++ -O3 -Wall udpreceiver.cc -o udpreceiver $SOLARFLARE_OPT
g++ -O3 -Wall tcpsniffer.cc -o tcpsniffer $SOLARFLARE_OPT
g++ -O3 -Wall udpping.cc -o udpping $SOLARFLARE_OPT
g++ -O3 -Wall udppong.cc -o udppong $SOLARFLARE_OPT
g++ -O3 -Wall udprecv.cc -o udprecv $SOLARFLARE_OPT
g++ -O3 -Wall udpsend.cc -o udpsend $SOLARFLARE_OPT
#g++ -O3 -Wall efvi_ping.cc -o efvi_ping $SOLARFLARE_OPT
