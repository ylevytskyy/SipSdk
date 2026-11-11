//
//  LoopbackPeer.swift
//  SipSdkTests
//

import Foundation
import Network

/// Lightweight loopback SIP peer for automated local signaling & media tests.
final class LoopbackPeer: @unchecked Sendable {
    let port: UInt16
    private var listener: NWListener?
    private var connection: NWConnection?
    private let queue = DispatchQueue(label: "com.sipsdk.loopbackpeer")
    private let lock = NSLock()

    var isBound: Bool = false
    var established: Bool = false
    var reinviteAnswered: Bool = false
    var acknowledged: Bool = false
    var lastRequest: String = ""
    var lastRegisterRequest: String = ""

    init(port: UInt16) {
        self.port = port
    }

    func start() {
        do {
            let params = NWParameters.udp
            params.allowLocalEndpointReuse = true
            let listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: port)!)
            self.listener = listener

            listener.stateUpdateHandler = { [weak self] state in
                if case .ready = state {
                    self?.lock.lock()
                    self?.isBound = true
                    self?.lock.unlock()
                }
            }

            listener.newConnectionHandler = { [weak self] conn in
                self?.handleNewConnection(conn)
            }

            listener.start(queue: queue)
        } catch {
            NSLog("[LoopbackPeer] Failed to bind UDP port %d: %@", port, error.localizedDescription)
        }
    }

    func stop() {
        listener?.cancel()
        listener = nil
        connection?.cancel()
        connection = nil
        isBound = false
    }

    private func handleNewConnection(_ conn: NWConnection) {
        self.connection = conn
        conn.start(queue: queue)
        receiveNext(on: conn)
    }

    private func receiveNext(on conn: NWConnection) {
        conn.receiveMessage { [weak self] content, context, isComplete, error in
            guard let self = self, let data = content, let msg = String(data: data, encoding: .utf8) else { return }
            self.lock.lock()
            self.lastRequest = msg
            self.lock.unlock()

            if msg.hasPrefix("REGISTER") {
                self.lock.lock()
                self.lastRegisterRequest = msg
                self.lock.unlock()
                self.sendOkResponse(on: conn, req: msg)
            } else if msg.hasPrefix("INVITE") {
                self.sendInviteResponse(on: conn, invite: msg)
            } else if msg.hasPrefix("ACK") {
                self.lock.lock()
                self.acknowledged = true
                self.established = true
                self.lock.unlock()
            } else if msg.hasPrefix("BYE") {
                self.sendOkResponse(on: conn, req: msg)
            }

            self.receiveNext(on: conn)
        }
    }

    private func sendInviteResponse(on conn: NWConnection, invite: String) {
        let callId = extractHeader(msg: invite, name: "Call-ID") ?? "unknown"
        let from = extractHeader(msg: invite, name: "From") ?? ""
        let to = extractHeader(msg: invite, name: "To") ?? ""
        let cseq = extractHeader(msg: invite, name: "CSeq") ?? "1 INVITE"
        let via = extractHeader(msg: invite, name: "Via") ?? ""

        let sdp = "v=0\r\n" +
            "o=- 123456 123456 IN IP4 127.0.0.1\r\n" +
            "s=-\r\n" +
            "c=IN IP4 127.0.0.1\r\n" +
            "t=0 0\r\n" +
            "m=audio 16000 RTP/AVP 0\r\n" +
            "a=rtpmap:0 PCMU/8000\r\n" +
            "a=sendrecv\r\n" +
            "m=video 16002 RTP/AVP 96\r\n" +
            "a=rtpmap:96 H264/90000\r\n" +
            "a=fmtp:96 packetization-mode=1\r\n" +
            "a=sendrecv\r\n"

        let resp = "SIP/2.0 200 OK\r\n" +
            "Via: \(via)\r\n" +
            "From: \(from)\r\n" +
            "To: \(to);tag=peer-tag-\(port)\r\n" +
            "Call-ID: \(callId)\r\n" +
            "CSeq: \(cseq)\r\n" +
            "Contact: <sip:peer@127.0.0.1:\(port)>\r\n" +
            "Content-Type: application/sdp\r\n" +
            "Content-Length: \(sdp.utf8.count)\r\n\r\n" +
            sdp

        if let data = resp.data(using: .utf8) {
            conn.send(content: data, completion: .idempotent)
        }
    }

    private func sendOkResponse(on conn: NWConnection, req: String) {
        let callId = extractHeader(msg: req, name: "Call-ID") ?? "unknown"
        let from = extractHeader(msg: req, name: "From") ?? ""
        let to = extractHeader(msg: req, name: "To") ?? ""
        let cseq = extractHeader(msg: req, name: "CSeq") ?? "2 BYE"
        let via = extractHeader(msg: req, name: "Via") ?? ""

        let resp = "SIP/2.0 200 OK\r\n" +
            "Via: \(via)\r\n" +
            "From: \(from)\r\n" +
            "To: \(to)\r\n" +
            "Call-ID: \(callId)\r\n" +
            "CSeq: \(cseq)\r\n" +
            "Content-Length: 0\r\n\r\n"

        if let data = resp.data(using: .utf8) {
            conn.send(content: data, completion: .idempotent)
        }
    }

    private func extractHeader(msg: String, name: String) -> String? {
        let lines = msg.components(separatedBy: "\r\n")
        for line in lines {
            if line.lowercased().hasPrefix(name.lowercased() + ":") {
                let parts = line.split(separator: ":", maxSplits: 1, omittingEmptySubsequences: true)
                if parts.count == 2 {
                    return parts[1].trimmingCharacters(in: .whitespaces)
                }
            }
        }
        return nil
    }
}
