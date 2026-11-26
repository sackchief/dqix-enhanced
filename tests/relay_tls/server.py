#!/usr/bin/env python3
"""A stand-in relay over TLS for tests/relay_tls/run.sh: accepts one WebSocket,
sends RM_OK, answers every RM_DATA with RM_DATA "pong:" + what it got.
usage: server.py cert key port"""
import base64, hashlib, socket, ssl, struct, sys

cert, key, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port)); srv.listen(4)
print("ready", flush=True)

def frame(payload):
    n = len(payload)
    h = b"\x82" + (bytes([n]) if n < 126 else b"\x7e" + struct.pack(">H", n))
    return h + payload

def readn(c, n):
    b = b""
    while len(b) < n:
        d = c.recv(n - len(b))
        if not d: raise EOFError
        b += d
    return b

while True:
    raw, _ = srv.accept()
    try:
        c = ctx.wrap_socket(raw, server_side=True)
    except ssl.SSLError as e:                    # the client refused our certificate
        print("handshake refused:", e.reason, flush=True); raw.close(); continue
    head = b""
    while not head.endswith(b"\r\n\r\n"): head += c.recv(1)
    lines = head.decode().split("\r\n")
    print("request:", lines[0], "| Host:", [l for l in lines if l.lower().startswith("host:")][0], flush=True)
    k = [l.split(": ")[1] for l in lines if l.lower().startswith("sec-websocket-key")][0]
    acc = base64.b64encode(hashlib.sha1((k + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    c.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
    c.sendall(frame(bytes([3, 0])))              # RM_OK, connection id 0
    try:
        while True:
            h = readn(c, 2); op = h[0] & 15; n = h[1] & 127
            if n == 126: n = struct.unpack(">H", readn(c, 2))[0]
            mask = readn(c, 4) if h[1] & 128 else b"\0" * 4
            p = bytes(b ^ mask[i & 3] for i, b in enumerate(readn(c, n)))
            if op == 8: break
            if op == 2 and p[0] == 1:            # RM_DATA from the client
                print("data from the client:", p[2:], flush=True)
                c.sendall(frame(bytes([1, 0]) + b"pong:" + p[2:]))
    except (EOFError, ssl.SSLError, ConnectionError):
        pass
    c.close()
