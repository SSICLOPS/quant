#! /usr/bin/env bash

rm -f /cores/*.core

set -e

# by default, test quant
c=${1:-quant}
s=${2:-quant}

# port to run servers on
addr=127.0.0.1
port=4433 # mozquic server can only run on 4433 at the moment
path=/index.html
dir=/Users/lars/Sites/lars/output
cert=/etc/letsencrypt/live/slate.eggert.org/fullchain.pem
key=/etc/letsencrypt/live/slate.eggert.org/privkey.pem

# (re-)build the client (and possibly server) to test
if [ "$c" == wcquant ] || [ "$s" == wcquant ] ||
   [ "$c" == wsquant ] || [ "$s" == wsquant ]; then
        delay=0.1
        vagrant ssh -c "\
                mkdir -p /vagrant/Linux; \
                cd /vagrant/Linux; \
                cmake -GNinja .. && ninja"
else
        delay=0.1
        ninja "$c"
        [ "$c" != "$s" ] && ninja "$s"
fi

export ASAN_OPTIONS=strict_string_checks=1:strict_init_order=1:detect_stack_use_after_return=1:detect_leaks=1:check_initialization_order=1:sleep_before_dying=30:alloc_dealloc_mismatch=1:detect_invalid_pointer_pairs=1
# export UBSAN_OPTIONS=suppressions=../misc/ubsan.supp:print_stacktrace=1

# commands to run the different clients against $addr:$port
case $c in
        quant)
                cc="bin/client -v5 https://$addr:$port$path"
                ;;
        wsquant)
                cc="bin/client -i vboxnet3 -v5 https://172.28.128.3:$port$path"
                ;;
        wcquant)
                cc="vagrant ssh -c \"\
                        /vagrant/Linux/bin/client -i enp0s8 -v5 \
                                https://172.28.128.1:$port$path\""
                ;;
        quicly)
                cc="external/usr/local/bin/cli -l /tmp/quicly-c.log -v \
                        -p $path $addr $port"
                ;;
        minq)
                cc="env MINQ_LOG=aead,connection,ack,handshake,tls,server,udp \
                        GOPATH=$(pwd)/external/go go run \
                        external/go/src/github.com/ekr/minq/bin/client/main.go \
                        -addr $addr:$port -http $path"
                ;;
        ngtcp2)
                cc="echo GET / | \
                        external/ngtcp2-prefix/src/ngtcp2/examples/client \
                        -i $addr $port"
                ;;
        mozquic)
                cc="env MOZQUIC_LOG=all:9 \
                        MOZQUIC_NSS_CONFIG=external/mozquic-prefix/src/mozquic/sample/nss-config \
                        DYLD_LIBRARY_PATH=external/mozquic-prefix/src/dist/$(cat external/mozquic-prefix/src/dist/latest)/lib \
                        external/mozquic-prefix/src/mozquic/client \
                                -peer $addr:$port -get $path -send-close"
                ;;
        picoquic)
                cc="external/picoquic-prefix/src/picoquic/picoquicdemo \
                        $addr $port -r"
                ;;
esac

# commands to run the different servers on  $addr:$port
case $s in
        quant)
                sc="bin/server -v5 -p $port -d $dir"
                ;;
        wsquant)
                sc="vagrant ssh -c \"\
                        /vagrant/Linux/bin/server -i enp0s8 -v5 -p $port \
                                -c ~/slate.eggert.org/fullchain.pem \
                                -k ~/slate.eggert.org/privkey.pem \
                                -d /usr/share/apache2/default-site\""
                ;;
        wcquant)
                sc="bin/server -v5 -i vboxnet3 -p $port -d $dir"
                ;;
        quicly)
                sc="external/usr/local/bin/cli \
                        -l /tmp/quicly-s.log -v \
                        -k $key -c $cert $addr $port"
                ;;
        minq)
                sc="env MINQ_LOG=aead,connection,ack,handshake,tls,server,udp \
                        GOPATH=$(pwd)/external/go go run \
                        external/go/src/github.com/ekr/minq/bin/server/main.go \
                        -addr $addr:$port -http -key $key -stateless-reset \
                        -cert $cert -server-name $addr"
                ;;
        ngtcp2)
                sc="external/ngtcp2-prefix/src/ngtcp2/examples/server \
                        -d $dir $addr $port $key $cert"
                ;;
        mozquic)
                sc="env MOZQUIC_LOG=all:9 \
                        MOZQUIC_NSS_CONFIG=external/mozquic-prefix/src/mozquic/sample/nss-config \
                        DYLD_LIBRARY_PATH=external/mozquic-prefix/src/dist/$(cat external/mozquic-prefix/src/dist/latest)/lib \
                        external/mozquic-prefix/src/mozquic/server -send-close"
                ;;
        picoquic)
                sc="external/picoquic-prefix/src/picoquic/picoquicdemo \
                        -p $port -k $key -c $cert"
                ;;
        ats)
                sed -i"" -e "s/.*proxy.config.http.server_ports.*/CONFIG proxy.config.http.server_ports STRING $port:quic/g" external/etc/trafficserver/records.config
                echo "dest_ip=* ssl_cert_name=$cert ssl_key_name=$key" > external/etc/trafficserver/ssl_multicert.config
                echo "map / http://127.0.0.1:8000/" > external/etc/trafficserver/remap.config
                sc="external/bin/traffic_server"
                ;;
esac

# # if we are on MacOS X, configure the firewall to add delay and loss
# if [ -x /usr/sbin/dnctl ]; then
#         # create pipes to limit bandwidth and add loss
#         sudo dnctl pipe 1 config bw 64Kbit/s delay 250 queue 10Kbytes #plr 0.5
#         sudo dnctl pipe 2 config bw 64Kbit/s delay 250 queue 10Kbytes #plr 0.25
#         sudo pfctl -f - <<EOF
#                 dummynet out proto udp from any to $addr port $port pipe 1
#                 dummynet out proto udp from $addr port $port to any pipe 2
# EOF
#         sudo pfctl -e || true
# fi

tmux -CC \
        new-session "sleep $delay; $cc" \; \
        split-window -h "$sc" \; \
        set remain-on-exit on

# ats doesn't exit cleanly
pkill traffic_server

# if we are on MacOS X, unconfigure the firewall
if [ -x /usr/sbin/dnctl ]; then
        sudo pfctl -d
        sudo dnctl -f flush
fi
