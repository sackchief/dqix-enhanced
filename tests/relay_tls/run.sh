#!/bin/sh
# Headless test of relay.c's TLS (Linux): a local TLS server plays the relay.
#   1. its certificate is trusted (SSL_CERT_FILE)      -> joins, data both ways
#   2. not trusted                                      -> refused, says why
#   3. trusted but for another name than we connect to  -> refused
set -e
cd "$(dirname "$0")"; R=../../runtime
W=${TMPDIR:-/tmp}/relay_tls.$$; mkdir -p $W; trap 'kill $SRV 2>/dev/null || true; rm -rf $W' EXIT
gcc -O1 -Wall -DDQIX_TLS -I$R harness.c $R/relay.c $R/relay_tls.c -o $W/h $(pkg-config --cflags --libs openssl) -lpthread
mkcert() {  # name file
    openssl req -x509 -newkey rsa:2048 -nodes -keyout $W/$2.key -out $W/$2.pem -days 2 -subj "/CN=$1" -addext "subjectAltName=DNS:$1" 2>/dev/null
}
mkcert localhost good; mkcert wrong.example bad
PORT=$((20000 + $$ % 20000)); fail=0
try() {  # label cert-for-server trust-file expect-exit
    python3 server.py $W/$2.pem $W/$2.key $PORT > $W/srv.log & SRV=$!
    while ! grep -q ready $W/srv.log 2>/dev/null; do sleep 0.1; done
    set +e; out=$(SSL_CERT_FILE=$W/$3.pem DQIX_RELAY=https://localhost:$PORT timeout 20 $W/h 2>&1); rc=$?; set -e
    kill $SRV 2>/dev/null; wait $SRV 2>/dev/null || true
    echo "--- $1: exit $rc"; echo "$out" | tail -3
    [ $rc = $4 ] || { echo "FAIL: expected exit $4"; fail=1; }
}
try "trusted certificate" good good 0
try "certificate not trusted" good bad 2
try "certificate for another name" bad bad 2
[ $fail = 0 ] && echo "ALL OK" || { echo "FAILED"; exit 1; }
