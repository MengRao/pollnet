set -e

SOLARFLARE_OPT="-DUSE_SOLARFLARE -lonload_zf_static -lciul1"
#SOLARFLARE_OPT=""

g++ -O3 tcpclient.cc -o tcpclient $SOLARFLARE_OPT
g++ -O3 tcpserver.cc -o tcpserver $SOLARFLARE_OPT
g++ -O3 udpreceiver.cc -o udpreceiver $SOLARFLARE_OPT
g++ -O3 tcpsniffer.cc -o tcpsniffer $SOLARFLARE_OPT
