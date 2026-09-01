# mp0rta/xquic — fork notice

This is the **mp0rta/xquic** fork of [alibaba/xquic](https://github.com/alibaba/xquic), adding [draft-ietf-quic-multipath-21](https://datatracker.ietf.org/doc/draft-ietf-quic-multipath/21/) wire compliance and CONNECT-IP ([RFC 9484](https://www.rfc-editor.org/rfc/rfc9484.html)) support for the [mqvpn](https://github.com/mp0rta/mqvpn) project. Per-PR audit findings against the draft live under [`docs/audit-notes/`](docs/audit-notes/). Upstream alibaba/xquic README follows.

## Per-path PMTU discovery

Upstream discovers one PMTU per *connection*. On a multipath connection whose
paths have different MTUs — an ordinary laptop with WiFi at 1500 and a tethered
handset at 1400 — that single value cannot be right for both, and the way it was
maintained meant it converged on neither.

Three properties combined into a packet black hole:

- `xqc_conn_try_to_update_mss()` computed the minimum `curr_pkt_out_size` across
  live paths and then applied it only `if (min_pkt_out_size > conn->pkt_out_size)`.
  The connection's packet size could therefore only ever **rise**. A path with a
  smaller usable MTU could never lower it.
- A new path was seeded from `conn->pkt_out_size` rather than from the size QUIC
  guarantees every path carries, so it was immediately counted as supporting a
  size no probe had confirmed on it — and since the connection size is the
  minimum over paths, that assumption could not be falsified.
- The downward half of the binary search floored at `conn->pkt_out_size`, so the
  search could narrow the range above the current size but never discover that
  the current size was itself too large.

With nothing able to reduce it, a connection configured for 1400-byte payloads
(1428 bytes on the wire once IPv4 and UDP headers are on) sent packets no
1400-MTU link could forward, indefinitely, and fed every drop to congestion
control as congestion.

What changed:

- **The search is per path.** `xqc_path_ctx_t` carries its own
  `path_probing_pkt_out_size` and `path_probing_cnt`, and
  `xqc_conn_ptmud_probing()` advances each path's cursor independently. A probe
  acked on the wide path no longer resets the range for the narrow one.
- **A new path starts at `XQC_PACKET_OUT_SIZE`** (1200, the size RFC 9000 §14.1
  requires any usable path to carry) and probes up. The first probe aims at the
  configured ceiling, so a path that does support it converges in one round trip
  rather than a binary descent.
- **`conn->pkt_out_size` is recomputed in both directions** as the minimum over
  paths whose limit is actually known, clamped to `[XQC_PACKET_OUT_SIZE,
  pkt_out_size_limit]`. Removing the constraining path — a failover — gives the
  size back.
- **`path_pmtu_bounded` separates "measured smaller" from "not measured yet".**
  Only a path whose search has excluded larger sizes constrains the connection.
  Without this distinction a connection would drop to 1200 and stay there
  whenever the peer does not negotiate PMTUD, since no path would ever be
  probed.
- **Black-hole detection.** Persistent congestion on a path resets it to the base
  size and reopens the search (RFC 8899 §5.2), so an MTU that shrinks
  mid-connection recovers instead of discarding every oversized packet.
- **`xqc_send_ctl_on_pmtud_ping_acked()` no longer dereferences NULL** when a
  probe is acked after its path closed — the `path == NULL` branch logged a
  warning and then fell through into `path->curr_pkt_out_size`.
- **The datagram MSS callback fires on a decrease, not only an increase.**
  `xqc_datagram_record_mss()` raised `XQC_CONN_FLAG_DGRAM_MSS_NOTIFY` only when
  the MSS grew, which was very nearly complete while the packet size could only
  rise. An application that sizes its writes to the MSS — a tunnel setting its
  interface MTU, say — would otherwise keep a stale, too-large value and have
  every oversized datagram rejected.
- The probe ceiling is now a maximum over paths. It was captured inside the
  branch tracking the minimum, so it came from whichever path held the
  *smallest* packet size.

Regression coverage is in `tests/unittest/xqc_pmtud_mp_test.c`.

**Cost of the fix.** Adding a path briefly lowers the connection to whatever the
new path has confirmed, since one buffer size serves every path. That is a
transient throughput dip on path addition, in exchange for not black-holing.
Sizing packets per path at send time would avoid it, but packets are allocated
before the scheduler picks a path, so that is a larger change.

**Not measured here.** These are static changes validated by unit tests and
review; no throughput measurement backs them yet. Read a weekly netsim run
before treating the aggregation numbers in mqvpn's
`docs/network_emulation_matrix.md` as changed.

## Known issues

### The PMTU search never reopens after it converges

Once a path's search converges, `path_pmtu_bounded` stays set and nothing probes
that path again. A PMTU that *increases* mid-connection — roaming onto a better
link, or a middlebox that stops clamping — is therefore never discovered, and a
long-lived connection keeps whatever size it settled on. RFC 8899 §5.3 handles
this with a PMTU_RAISE_TIMER (600 s by default) that drops the path back into
search; that timer is not implemented here.

This is a narrowing of an old behaviour rather than a new failure: before, the
size could only rise, so an increase was "handled" by raising it without ever
validating that anything could carry it — which is the black hole this change
exists to close. Converging low is the safe direction to be wrong in, but it is
still wrong.

### Datagram MSS is connection-wide

`xqc_datagram_record_mss()` derives the datagram MSS from `conn->pkt_out_size`.
A DATAGRAM frame cannot be fragmented across packets, so its size must fit the
*smallest* path it might be scheduled onto — which is what the connection-wide
minimum now gives, at the cost of holding every path to the narrowest one. A
scheduler that knew the frame size could route large datagrams away from narrow
paths instead; nothing does that today.

### `min_rtt` and `srtt` are on different scales

RFC 9002 §5.3 subtracts the peer's `ack_delay` when smoothing `srtt`, while §5.2
tracks `min_rtt` from the unadjusted `latest_rtt`. `srtt < min_rtt` is therefore
a routine outcome, not an anomaly, and consumers that assume otherwise get
nothing useful. `xqc_bbr2_compensate_cwnd_for_rttvar()` already returns zero
compensation in that case; its log line was downgraded from `WARN` to `DEBUG`
rather than the comparison being changed, since correcting the scales touches
congestion control and is not something to do without measurement.

### TODO: re-fragment CRYPTO data when a Retry lengthens the Initial header

`xqc_conn_reassemble_packet()` (`src/transport/xqc_conn.c`) rebuilds an Initial packet
with a new, longer header once a Retry arrives — it gains the Retry token and possibly a
longer DCID — and then copies the original payload in verbatim. The payload was sized
against the *old*, shorter header, so the packet ends up larger than the
`max_pkt_out_size` it was built for. With a post-quantum key share such as
X25519MLKEM768 (1184-byte key share) the first Initial is already full, which made this
routine rather than a corner case: the re-sent packet overflowed the AEAD output buffer
and encryption failed with `XQC_TLS_ENCRYPT_DATA_ERROR` (-736), so the connection closed
locally and the handshake never completed.

Currently mitigated, not fixed:

- `XQC_PACKET_OUT_HDR_GROWTH` (`src/transport/xqc_packet_out.h`) reserves the provable
  worst-case header growth (`XQC_MAX_TOKEN_LEN + 1 + XQC_MAX_CID_LEN` = 277 bytes), so
  the rebuilt packet fits the buffer. This is capacity only — it does not change what is
  put on the wire.
- The payload copy is now bounds-checked, so any future overflow fails at the copy with
  `XQC_ENOBUF` and a log line instead of surfacing as an opaque encryption error.

**Still to fix:** the re-sent Initial can exceed the locally configured
`max_pkt_out_size`, so it may be dropped on a path whose real MTU is that small. The
proper fix is to re-fragment the CRYPTO data across packets under the new header, which
is the refactor the `TODO` in `xqc_conn_resend_packets()` already calls for (generate the
header before writing frames, so reassembly is unnecessary).

---

# XQUIC
<img src="docs/images/xquic_logo.png" alt="xquic logo" width=585.9 height=309.1/>

![GitHub](https://img.shields.io/github/license/alibaba/xquic)
[![Build](https://github.com/alibaba/xquic/actions/workflows/build.yml/badge.svg)](https://github.com/alibaba/xquic/actions/workflows/build.yml)
[![CodeQL](https://github.com/alibaba/xquic/actions/workflows/codeql-analysis.yml/badge.svg)](https://github.com/alibaba/xquic/actions/workflows/codeql-analysis.yml)
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/485e758edd98409bb7a51cbb803838c4)](https://www.codacy.com/gh/alibaba/xquic/dashboard?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=alibaba/xquic&amp;utm_campaign=Badge_Grade)
[![Codacy Badge](https://app.codacy.com/project/badge/Coverage/485e758edd98409bb7a51cbb803838c4)](https://www.codacy.com/gh/alibaba/xquic/dashboard?utm_source=github.com&utm_medium=referral&utm_content=alibaba/xquic&utm_campaign=Badge_Coverage)
![Platforms](https://img.shields.io/badge/platform-Android%20%7C%20iOS%20%7C%20Linux%20%7C%20macOS-blue)
[![DeepWiki](https://img.shields.io/badge/DeepWiki-alibaba%2Fxquic-blue.svg?logo=data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACwAAAAyCAYAAAAnWDnqAAAAAXNSR0IArs4c6QAAA05JREFUaEPtmUtyEzEQhtWTQyQLHNak2AB7ZnyXZMEjXMGeK/AIi+QuHrMnbChYY7MIh8g01fJoopFb0uhhEqqcbWTp06/uv1saEDv4O3n3dV60RfP947Mm9/SQc0ICFQgzfc4CYZoTPAswgSJCCUJUnAAoRHOAUOcATwbmVLWdGoH//PB8mnKqScAhsD0kYP3j/Yt5LPQe2KvcXmGvRHcDnpxfL2zOYJ1mFwrryWTz0advv1Ut4CJgf5uhDuDj5eUcAUoahrdY/56ebRWeraTjMt/00Sh3UDtjgHtQNHwcRGOC98BJEAEymycmYcWwOprTgcB6VZ5JK5TAJ+fXGLBm3FDAmn6oPPjR4rKCAoJCal2eAiQp2x0vxTPB3ALO2CRkwmDy5WohzBDwSEFKRwPbknEggCPB/imwrycgxX2NzoMCHhPkDwqYMr9tRcP5qNrMZHkVnOjRMWwLCcr8ohBVb1OMjxLwGCvjTikrsBOiA6fNyCrm8V1rP93iVPpwaE+gO0SsWmPiXB+jikdf6SizrT5qKasx5j8ABbHpFTx+vFXp9EnYQmLx02h1QTTrl6eDqxLnGjporxl3NL3agEvXdT0WmEost648sQOYAeJS9Q7bfUVoMGnjo4AZdUMQku50McDcMWcBPvr0SzbTAFDfvJqwLzgxwATnCgnp4wDl6Aa+Ax283gghmj+vj7feE2KBBRMW3FzOpLOADl0Isb5587h/U4gGvkt5v60Z1VLG8BhYjbzRwyQZemwAd6cCR5/XFWLYZRIMpX39AR0tjaGGiGzLVyhse5C9RKC6ai42ppWPKiBagOvaYk8lO7DajerabOZP46Lby5wKjw1HCRx7p9sVMOWGzb/vA1hwiWc6jm3MvQDTogQkiqIhJV0nBQBTU+3okKCFDy9WwferkHjtxib7t3xIUQtHxnIwtx4mpg26/HfwVNVDb4oI9RHmx5WGelRVlrtiw43zboCLaxv46AZeB3IlTkwouebTr1y2NjSpHz68WNFjHvupy3q8TFn3Hos2IAk4Ju5dCo8B3wP7VPr/FGaKiG+T+v+TQqIrOqMTL1VdWV1DdmcbO8KXBz6esmYWYKPwDL5b5FA1a0hwapHiom0r/cKaoqr+27/XcrS5UwSMbQAAAABJRU5ErkJggg==)](https://deepwiki.com/alibaba/xquic)
<!-- DeepWiki badge generated by https://deepwiki.ryoppippi.com/ -->
+ [官方文档](https://doc.xquic.org.cn)
+ [AI文档](https://deepwiki.com/alibaba/xquic/)
+ [简体中文文档 README-zh-CN](https://github.com/alibaba/xquic/blob/main/docs/docs-zh/README-zh.md)

## Introduction

XQUIC Library released by Alibaba is …

… **a client and server implementation of QUIC and HTTP/3 as specified by the IETF.** Currently supported QUIC versions are v1 and draft-29.

… **OS and platform agnostic.** It currently supports Android, iOS, HarmonyOS, Linux, macOS and Windows(v1.2.0). Most of the code is used in our own products, and has been tested at scale on android, iOS apps, as well as servers.

… **still in active development.** [Interoperability](https://interop.seemann.io/) is regularly tested with other QUIC implementations.

### Features

[![](https://img.shields.io/static/v1?label=RFC&message=9000&color=brightgreen)](https://tools.ietf.org/html/rfc9000)
[![](https://img.shields.io/static/v1?label=RFC&message=9001&color=brightgreen)](https://tools.ietf.org/html/rfc9001)
[![](https://img.shields.io/static/v1?label=RFC&message=9002&color=brightgreen)](https://tools.ietf.org/html/rfc9002)
[![](https://img.shields.io/static/v1?label=RFC&message=9114&color=brightgreen)](https://tools.ietf.org/html/rfc9114)
[![](https://img.shields.io/static/v1?label=RFC&message=9204&color=brightgreen)](https://tools.ietf.org/html/rfc9204)
[![](https://img.shields.io/static/v1?label=RFC&message=9221&color=brightgreen)](https://datatracker.ietf.org/doc/html/rfc9221)


[![](https://img.shields.io/static/v1?label=draft-13&message=QUIC-LB&color=9cf)](https://tools.ietf.org/html/draft-ietf-quic-load-balancers-13)
[![](https://img.shields.io/static/v1?label=draft-05&message=Multipath-QUIC&color=9cf)](https://tools.ietf.org/html/draft-ietf-quic-multipath-05)
[![](https://img.shields.io/static/v1?label=draft-06&message=Multipath-QUIC&color=9cf)](https://tools.ietf.org/html/draft-ietf-quic-multipath-06)
[![](https://img.shields.io/static/v1?label=draft-07&message=QUIC-Qlog&color=9cf)](https://datatracker.ietf.org/doc/html/draft-ietf-quic-qlog-main-schema-07)

#### Standardized Features

* All big features conforming with [RFC 9000](https://www.rfc-editor.org/rfc/rfc9000), [RFC9001](https://www.rfc-editor.org/rfc/rfc9001), [RFC9002](https://www.rfc-editor.org/rfc/rfc9002), [RFC9114](https://www.rfc-editor.org/rfc/rfc9114) and [RFC9204](https://www.rfc-editor.org/rfc/rfc9204), including the interface between QUIC and TLS, 0-RTT connection establishment, HTTP/3 and QPACK.
* ALPN Extension conforming with [RFC7301](https://www.rfc-editor.org/rfc/rfc7301)

#### Supported TLS 1.3 Cipher Suites

XQUIC supports the following TLS 1.3 cipher suites for QUIC packet protection (as defined in [RFC 9001 Section 5](https://www.rfc-editor.org/rfc/rfc9001#section-5)):

| Cipher Suite | AEAD | Header Protection | Status |
|---|---|---|---|
| TLS_AES_128_GCM_SHA256 | AEAD_AES_128_GCM | AES-ECB (128-bit) | Supported |
| TLS_AES_256_GCM_SHA384 | AEAD_AES_256_GCM | AES-ECB (256-bit) | Supported |
| TLS_CHACHA20_POLY1305_SHA256 | AEAD_CHACHA20_POLY1305 | ChaCha20 | Supported |
| TLS_AES_128_CCM_SHA256 | AEAD_AES_128_CCM | — | Not Supported |
| TLS_AES_128_CCM_8_SHA256 | — | — | Not Supported |

> **Note:** `TLS_AES_128_CCM_SHA256` and `TLS_AES_128_CCM_8_SHA256` are not supported. CCM-based cipher suites are optional per [RFC 9001](https://www.rfc-editor.org/rfc/rfc9001) and have significantly lower confidentiality and integrity limits (2^21.5) compared to GCM (2^23 / 2^52) and ChaCha20-Poly1305. `TLS_AES_128_CCM_8_SHA256` is further excluded from QUIC usage by RFC 9001 as no header protection scheme is defined for it.

#### Not Yet Standardized Features

* [Multipath QUIC](https://tools.ietf.org/html/draft-ietf-quic-multipath-04)
* [QUIC-LB](https://tools.ietf.org/html/draft-ietf-quic-load-balancers-13)

#### Library Features

* Pluggable congestion control: NewReno, Cubic, BBR and BBRv2, ...
* Pluggable cryptography, integration with BoringSSL and BabaSSL
* Cross-platform implementation, support Android, iOS, HarmonyOS, Linux, macOS and Windows(v1.2.0)

## Requirements

To build XQUIC, you need 
* CMake
* BoringSSL or BabaSSL

To run test cases, you need
* libevent
* CUnit

## QuickStart Guide

XQUIC can be built with BabaSSL(Tongsuo) or BoringSSL.

### Build with BoringSSL

```bash
sudo apt-get install -y build-essential libevent-dev

# get XQUIC source code
git clone https://github.com/alibaba/xquic.git; cd xquic

# get and build BoringSSL
git clone https://github.com/google/boringssl.git ./third_party/boringssl; cd ./third_party/boringssl
mkdir -p build && cd build
cmake -DBUILD_SHARED_LIBS=0 -DCMAKE_C_FLAGS="-fPIC" -DCMAKE_CXX_FLAGS="-fPIC" ..
make -j ssl crypto
cd ..
SSL_TYPE_STR="boringssl"
SSL_PATH_STR="${PWD}"
cd ../..

# build XQUIC with BoringSSL
# When build XQUIC with boringssl, by default XQUIC will use boringssl
# in third_party. If boringssl is deployed in other directories, SSL_PATH could be 
# used to specify the search path of boringssl
git submodule update --init --recursive
mkdir -p build; cd build
cmake -DGCOV=on -DCMAKE_BUILD_TYPE=Debug -DXQC_ENABLE_TESTING=1 -DXQC_SUPPORT_SENDMMSG_BUILD=1 -DXQC_ENABLE_EVENT_LOG=1 -DXQC_ENABLE_BBR2=1 -DXQC_ENABLE_RENO=1 -DSSL_TYPE=${SSL_TYPE_STR} -DSSL_PATH=${SSL_PATH_STR} ..

# exit if cmake error
if [ $? -ne 0 ]; then
    echo "cmake failed"
    exit 1
fi

make -j
```

### Build with BabaSSL(Tongsuo)

```bash
sudo apt-get install -y build-essential libevent-dev

# get XQUIC source code
git clone https://github.com/alibaba/xquic.git; cd xquic

# get and build BabaSSL(Tongsuo)
git clone -b 8.3-stable https://github.com/Tongsuo-Project/Tongsuo.git ./third_party/babassl; cd ./third_party/babassl/
./config --prefix=/usr/local/babassl
make -j
SSL_TYPE_STR="babassl"
SSL_PATH_STR="${PWD}"
cd -

# build XQUIC with BabaSSL
# When build XQUIC with babassl, /usr/local/babassl directory will be searched
# as default. If babassl is deployed in other directories, SSL_PATH could be 
# used to specify the search path of babassl
git submodule update --init --recursive
mkdir -p build; cd build
cmake -DGCOV=on -DCMAKE_BUILD_TYPE=Debug -DXQC_ENABLE_TESTING=1 -DXQC_SUPPORT_SENDMMSG_BUILD=1 -DXQC_ENABLE_EVENT_LOG=1 -DXQC_ENABLE_BBR2=1 -DXQC_ENABLE_RENO=1 -DSSL_TYPE=${SSL_TYPE_STR} -DSSL_PATH=${SSL_PATH_STR} ..

# exit if cmake error
if [ $? -ne 0 ]; then
    echo "cmake failed"
    exit 1
fi

make -j
```

### Run testcases

```bash
sh ../scripts/xquic_test.sh
```

## Documentation

* For using the API, see the [API docs](./docs/API.md).
* For platform support details, see the [Platforms docs](./docs/Platforms.md).
* For Chinese Simplified (zh-CN) translation of the IETF QUIC Protocol, see the Translation docs.
    - [RFC8999-invariants-zh](./docs/translation/rfc8999-invariants-zh.md)
    - [RFC9000-transport-zh](./docs/translation/rfc9000-transport-zh.md)
    - [RFC9001-tls-zh](./docs/translation/rfc9001-tls-zh.md)
    - [RFC9002-recovery-zh](./docs/translation/rfc9002-recovery-zh.md)
    - [draft-ietf-quic-http-34-zh](./docs/translation/draft-ietf-quic-http-34-zh.md)
    - [draft-ietf-quic-qpack-21-zh](./docs/translation/draft-ietf-quic-qpack-21-zh.md)
    - [RFC9221-datagram-zh](./docs/translation/rfc9221-datagram-zh.md)

* For using quic-qlog, see the [Features: qlog](./docs/Features.md)
* For testing the library, see the [Testing docs](./docs/docs-zh/Testing-zh.md).
* For other frequently asked questions, see the [FAQs](./docs/docs-zh/FAQ-zh.md) and [Trouble Shooting Guide](./docs/docs-zh/Troubleshooting-zh.md).

## Contributing

We would love for you to contribute to XQUIC and help make it even better than it is today! All types of contributions are encouraged and valued. Thanks to [all contributors](https://github.com/alibaba/xquic/blob/main/CONTRIBUTING.md#all-contributors). See our [Contributing Guidelines](./CONTRIBUTING.md) for more information.

If you have any questions, please feel free to open a new Discussion topic in our [discussion forums](https://github.com/alibaba/xquic/discussions).

## License

XQUIC is released under the Apache 2.0 License.

## Contact Us

Feel free to contact us in the following ways:

* e-mail: xquic@alibaba-inc.com
* Dingtalk group: 34059705
* slack channel: #xquic in quicdev group

  <img src="docs/images/dingtalk_group.JPG" width=200 alt="dingtalk group"/>
