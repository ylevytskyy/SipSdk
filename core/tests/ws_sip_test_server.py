#!/usr/bin/env python3
"""Minimal independent RFC 7118 SIP-over-WebSocket (WS/WSS) test registrar.

Usage:
  ws_sip_test_server.py [--tls --cert PATH --key PATH]

Prints `PORT=<port>` to stdout once listening on 127.0.0.1, then accepts
RFC 6455 WebSocket connections with `Sec-WebSocket-Protocol: sip`, verifies
the incoming SIP REGISTER carries the expected WS/WSS Via transport, and
replies with `SIP/2.0 200 OK`.
"""

import argparse
import base64
import hashlib
import re
import socket
import ssl
import struct
import sys
import threading

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def recv_exact(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def read_ws_frame(conn):
    hdr = recv_exact(conn, 2)
    if hdr is None:
        return None, None
    opcode = hdr[0] & 0x0F
    masked = (hdr[1] & 0x80) != 0
    length = hdr[1] & 0x7F
    if length == 126:
        ext = recv_exact(conn, 2)
        if ext is None:
            return None, None
        length = struct.unpack("!H", ext)[0]
    elif length == 127:
        ext = recv_exact(conn, 8)
        if ext is None:
            return None, None
        length = struct.unpack("!Q", ext)[0]
    mask = recv_exact(conn, 4) if masked else b""
    if masked and mask is None:
        return None, None
    payload = recv_exact(conn, length) if length > 0 else b""
    if payload is None:
        return None, None
    if masked:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return opcode, payload


def send_ws_frame(conn, opcode, payload):
    frame = bytearray([0x80 | (opcode & 0x0F)])
    n = len(payload)
    if n < 126:
        frame.append(n)
    elif n < 65536:
        frame.append(126)
        frame.extend(struct.pack("!H", n))
    else:
        frame.append(127)
        frame.extend(struct.pack("!Q", n))
    frame.extend(payload)
    conn.sendall(bytes(frame))


def extract_header(sip_text, name):
    m = re.search(rf"(?im)^{re.escape(name)}\s*:\s*([^\r\n]+)", sip_text)
    return m.group(1).strip() if m else ""


def build_register_200(sip_text, expected_via_tp):
    via = extract_header(sip_text, "Via")
    frm = extract_header(sip_text, "From")
    to = extract_header(sip_text, "To")
    call_id = extract_header(sip_text, "Call-ID")
    cseq = extract_header(sip_text, "CSeq")
    contact = extract_header(sip_text, "Contact")
    if expected_via_tp not in via.upper():
        return (
            f"SIP/2.0 400 Bad Transport\r\n"
            f"Via: {via}\r\n"
            f"From: {frm}\r\n"
            f"To: {to};tag=wsserver\r\n"
            f"Call-ID: {call_id}\r\n"
            f"CSeq: {cseq}\r\n"
            f"Content-Length: 0\r\n\r\n"
        ).encode("utf-8")
    if ";tag=" not in to.lower():
        to = f"{to};tag=wsserver"
    resp = (
        f"SIP/2.0 200 OK\r\n"
        f"Via: {via}\r\n"
        f"From: {frm}\r\n"
        f"To: {to}\r\n"
        f"Call-ID: {call_id}\r\n"
        f"CSeq: {cseq}\r\n"
        f"Contact: {contact};expires=3600\r\n"
        f"Expires: 3600\r\n"
        f"Content-Length: 0\r\n\r\n"
    )
    return resp.encode("utf-8")


def handle_client(raw_conn, ssl_ctx, expected_via_tp):
    conn = raw_conn
    try:
        conn.settimeout(5.0)
        if ssl_ctx is not None:
            conn = ssl_ctx.wrap_socket(raw_conn, server_side=True)
        req = b""
        while b"\r\n\r\n" not in req and len(req) < 8192:
            chunk = conn.recv(4096)
            if not chunk:
                return
            req += chunk
        req_text = req.decode("utf-8", errors="replace")
        key_m = re.search(r"(?im)^Sec-WebSocket-Key\s*:\s*([^\r\n]+)", req_text)
        proto_m = re.search(r"(?im)^Sec-WebSocket-Protocol\s*:\s*([^\r\n]+)", req_text)
        if not key_m or not proto_m or "sip" not in proto_m.group(1).lower():
            conn.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
            return
        accept = base64.b64encode(
            hashlib.sha1((key_m.group(1).strip() + WS_GUID).encode("utf-8")).digest()
        ).decode("ascii")
        handshake = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n"
            "Sec-WebSocket-Protocol: sip\r\n\r\n"
        )
        conn.sendall(handshake.encode("utf-8"))

        while True:
            opcode, payload = read_ws_frame(conn)
            if opcode is None or opcode == 8:
                break
            if opcode == 9:  # ping
                send_ws_frame(conn, 10, payload)
                continue
            if opcode in (1, 2):  # text or binary SIP message
                sip_text = payload.decode("utf-8", errors="replace")
                if sip_text.startswith("REGISTER "):
                    resp = build_register_200(sip_text, expected_via_tp)
                    send_ws_frame(conn, opcode, resp)
    except Exception:
        pass
    finally:
        try:
            conn.close()
        except Exception:
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tls", action="store_true")
    parser.add_argument("--cert", default="")
    parser.add_argument("--key", default="")
    args = parser.parse_args()

    ssl_ctx = None
    expected_via_tp = "SIP/2.0/WS "
    if args.tls:
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(certfile=args.cert, keyfile=args.key)
        expected_via_tp = "SIP/2.0/WSS "

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(16)
    port = srv.getsockname()[1]
    sys.stdout.write(f"PORT={port}\n")
    sys.stdout.flush()

    try:
        while True:
            conn, _ = srv.accept()
            t = threading.Thread(
                target=handle_client,
                args=(conn, ssl_ctx, expected_via_tp),
                daemon=True,
            )
            t.start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()


if __name__ == "__main__":
    main()
