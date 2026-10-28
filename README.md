# r11234567/xquic — fork notice

This is the **r11234567/xquic** fork of [alibaba/xquic](https://github.com/alibaba/xquic) (synced with [mp0rta/xquic](https://github.com/mp0rta/xquic)), adding [draft-ietf-quic-multipath-21](https://datatracker.ietf.org/doc/draft-ietf-quic-multipath/21/) wire compliance and CONNECT-IP ([RFC 9484](https://www.rfc-editor.org/rfc/rfc9484.html)) support for the [mqvpn](https://github.com/r11234567/mqvpn) project. Per-PR audit findings against the draft live under [`docs/audit-notes/`](docs/audit-notes/).

> **Fork Notice**
>
> This repository is a fork maintained primarily for deployment, testing, customization, and synchronization with upstream enhancements.
>
> Upstream copyright, licensing, and attribution remain unchanged.

---

## Synchronized fork fixes

- Certificate verification supports `XQC_TLS_CERT_FLAG_APP_VERIFY`. It passes the complete peer-presented chain, leaf first, to the application's `cert_verify_cb` and treats that decision as final. The mode implies peer verification and fails closed when no callback is installed. The normal `XQC_TLS_CERT_FLAG_NEED_VERIFY` path remains library-owned and uses the TLS backend's default trust paths.
- **Two-tiered Stream Reassembly Node Cap**: The cap is configurable per connection, one slot is reserved for a frame that can fill the leftmost gap, repeated cap hits are rate-limited in logs, and FIN repair prevents a rejected frame from leaving the receive state incomplete. The packet remains unacknowledged so normal QUIC retransmission supplies backpressure without closing the connection. The default is two-tiered: 8192 nodes is the sparse-frame threshold, frames above it must average at least 256 bytes per node, and 32768 nodes is the absolute ceiling. This preserves a full 16 MiB receive window for ordinary packet-sized frames without reopening the sparse-node amplification. Index: `include/xquic/xquic.h` documents the public setting; `src/transport/xqc_defs.h` defines both tiers and the density floor; `src/transport/xqc_frame.c` enforces them and reserves the prefix slot; and `tests/unittest/xqc_stream_frame_test.c` covers dense-window admission, sparse rejection, prefix liveness, the hard ceiling, FIN repair, and an explicit per-connection override.

## HTTP/3 proxy backpressure and urgency

An event-driven proxy paces a fast backend against one H3 response using the return value of `xqc_h3_request_send_body()`: a short write or `-XQC_EAGAIN` means the stream cannot take more right now. `xqc_h3_request_set_write_notify()` then keeps `h3_request_write_notify` arriving on every engine pass in which the stream is writable, so the acknowledgement that releases queue space resumes the producer with no application timer. Releasing the callback removes an otherwise idle transport stream from writable scheduling, while preserving H3's own buffered-frame retries.

`xqc_h3_request_get_unsent_queue_bytes()` reports only what the connection has yet to put on the wire and is a memory bound, not a rate control. Packets in flight are excluded on purpose: they are the congestion controller's budget, and a connection at its BDP holds a congestion window of them at all times, so a watermark that counted them latched closed on exactly the fast connections it was meant to pace.

RFC 9218 urgency 0 and 1 use a dedicated urgent packet queue. It is scheduled ahead of normal application data but, unlike private priority hacks, remains subject to congestion control and pacing. This allows a small late response to precede queued bulk resources without bypassing network safety. Retry, 0-RTT discard, connection teardown, and stream-close paths all preserve or clean up the queue.

`xqc_parse_http_priority()` parses the supplied byte span without assuming NUL termination. This removes out-of-bounds reads on HTTP header slices and accepts the RFC 9218 incremental forms `i`, `i=?0`, and `i=?1`.

## Per-path PMTU discovery

Upstream historically discovered one PMTU per *connection*. On a multipath connection whose paths have different MTUs — an ordinary laptop with WiFi at 1500 and a tethered handset at 1400 — that single value cannot be right for both, and a single cursor caused packet black-holing on the narrower link.

- Per-path probing cursor and attempt tracking: `xqc_path_ctx_t` maintains its own PMTU discovery state and packet ceiling (`path_max_pkt_out_size`), allowing the wider link to advance to 1500 without dropping packets on the narrow link.
- Downward MTU update: when a path validates at a smaller MTU or encounters black holes (RFC 8899 §4.3), the connection and path MTUs drop safely to prevent black-holing.
- **Raise Timer (RFC 8899 §5.3)**: A converged PMTU search rearms at `pmtud_raise_interval` (default 600 s) to detect route changes or removed MTU clamping mid-connection. Probing resumes above the confirmed size while the connection continues using the proven size, avoiding throughput dips.
- External path MTU updates via `xqc_conn_set_path_pmtu()` allow external network discovery components to inform xquic of path constraints.

## Multipath WLB (Weighted Load Balancing) Scheduler

WLB distributes DATAGRAM traffic across paths in proportion to what each path is measured to deliver, while pinning inner TCP flows (via `po_flow_hash`) to individual QUIC paths to prevent reordering inside VPN tunnels.

- **Sampled Goodput over Wall Clock**: Sustained goodput is measured over wall-clock windows (`WLB_GOODPUT_SAMPLE_MIN_US` = 200 ms) rather than ACK-burst spans, ensuring zero-delivery paths decay their weight naturally.
- **Faded Estimate Credit & Standing Queue Protection**: Under-fed app-limited paths credit BBR's delivery rate estimate, but credit is smoothly faded out if a standing queue forms (`srtt` rising over its load-learned floor).
- **Decoupled Round Renewal & Weight Refresh**: Round boundary deficits reset cleanly without repeatedly recomputing weights on the hot packet path. Weight sampling occurs once per 200 ms interval, avoiding memory allocations and cache thrashing.
- **Last-Path PTO Deadlock Prevention (`wlb_any_path_responsive`)**: The consecutive-PTO blackhole guard acts as a preference between paths rather than an absolute lockout. When all paths cross the PTO threshold, MinRTT fallback continues scheduling traffic so an incoming ACK can clear `ctl_pto_count` and restore the connection.
- **Demoted Per-Packet Logging**: Hot-path packet scheduling logs (`flow_hit`, `select`, `round_start`, `recovery_grace`) use `XQC_LOG_DEBUG` to avoid CPU exhaustion at high packet rates (>100k pps).

## Known issues

### Datagram MSS is connection-wide

`xqc_datagram_record_mss()` derives the datagram MSS from `conn->pkt_out_size`. A DATAGRAM frame cannot be fragmented across packets, so its size must fit the *smallest* path it might be scheduled onto — which is what the connection-wide minimum gives, holding every path to the narrowest one. A scheduler that inspects frame size can route large datagrams away from narrow paths; future work will decouple this.

### `min_rtt` and `srtt` are on different scales

RFC 9002 §5.3 subtracts the peer's `ack_delay` when smoothing `srtt`, while §5.2 tracks `min_rtt` from the unadjusted `latest_rtt`. `srtt < min_rtt` is therefore a routine outcome, not an anomaly. `xqc_bbr2_compensate_cwnd_for_rttvar()` safely handles this comparison.

### Retry CRYPTO Header Growth Mitigation

When a Retry packet arrives, `xqc_conn_reassemble_packet()` rebuilds an Initial packet with a longer header (including the Retry token and new DCID). When using post-quantum key exchange (such as X25519MLKEM768 with a 1184-byte key share), the first Initial packet is already full, causing header reassembly to overflow typical buffers.

- `XQC_PACKET_OUT_HDR_GROWTH` (277 bytes in `src/transport/xqc_packet_out.h`) reserves the worst-case header expansion space (`XQC_MAX_TOKEN_LEN + 1 + XQC_MAX_CID_LEN`).
- Payload copying is strictly bounds-checked to prevent buffer corruption.

---

# XQUIC Architecture & Features

### Features

- **Standardized Features**: Full conformance with [RFC 9000](https://www.rfc-editor.org/rfc/rfc9000), [RFC 9001](https://www.rfc-editor.org/rfc/rfc9001), [RFC 9002](https://www.rfc-editor.org/rfc/rfc9002), [RFC 9114](https://www.rfc-editor.org/rfc/rfc9114) (HTTP/3), [RFC 9204](https://www.rfc-editor.org/rfc/rfc9204) (QPACK), and [RFC 9221](https://datatracker.ietf.org/doc/html/rfc9221) (Datagram).
- **Multipath Support**: [draft-ietf-quic-multipath-21](https://datatracker.ietf.org/doc/draft-ietf-quic-multipath/21/) wire compliance with multiple packet number spaces, path validation, and WLB / MinRTT / Backup schedulers.
- **Proxy Protocol**: CONNECT-IP ([RFC 9484](https://www.rfc-editor.org/rfc/rfc9484.html)) tunneling support.
- **Congestion Control**: BBR, BBRv2, CUBIC, and NewReno.
- **Forward Error Correction (FEC)**: Reed-Solomon (RSC) and XOR packet recovery.

### Supported TLS 1.3 Cipher Suites

| Cipher Suite | AEAD | Header Protection | Status |
|---|---|---|---|
| TLS_AES_128_GCM_SHA256 | AEAD_AES_128_GCM | AES-ECB (128-bit) | Supported |
| TLS_AES_256_GCM_SHA384 | AEAD_AES_256_GCM | AES-ECB (256-bit) | Supported |
| TLS_CHACHA20_POLY1305_SHA256 | AEAD_CHACHA20_POLY1305 | ChaCha20 | Supported |

### Building and Testing

```bash
# Build BoringSSL submodule
cd third_party/boringssl
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON ..
make -j$(nproc) crypto ssl

# Build XQUIC
cd ../../..
mkdir build && cd build
cmake -DXQC_ENABLE_TESTING=1 -DXQC_SUPPORT_SENDMMSG_BUILD=1 ..
make -j$(nproc)
make test
```

## License

XQUIC is released under the Apache 2.0 License.
