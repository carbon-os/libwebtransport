<h1 align="center">
  <a href="https://libwebtransport.example"><img src="./.github/logo.png" alt="libwebTransport" height="150px"></a>
  <br>
  libwebtransport
  <br>
</h1>
<h4 align="center">A pure C/C++ implementation of the WebTransport API leveraging QUIC and HTTP/3</h4>
<p align="center">
    <a href="https://github.com/carbon-os/libwebtransport"><img src="https://img.shields.io/badge/libwebTransport-C/C++-blue.svg?longCache=true" alt="libwebTransport" /></a>
  <a href="https://datatracker.ietf.org/doc/html/rfc9000"><img src="https://img.shields.io/static/v1?label=RFC&message=9000&color=brightgreen" /></a>
  <a href="https://datatracker.ietf.org/doc/html/rfc9001"><img src="https://img.shields.io/static/v1?label=RFC&message=9001&color=brightgreen" /></a>
  <a href="https://datatracker.ietf.org/doc/html/rfc9002"><img src="https://img.shields.io/static/v1?label=RFC&message=9002&color=brightgreen" /></a>
  <a href="https://datatracker.ietf.org/doc/html/rfc9114"><img src="https://img.shields.io/static/v1?label=RFC&message=9114&color=brightgreen" /></a>
  <br>
    <a href="https://github.com/carbon-os/libwebtransport"><img src="https://img.shields.io/static/v1?label=Build&message=Documentation&color=brightgreen" /></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-5865F2.svg" alt="License: MIT" /></a>
</p>
<br>

### New Release

libwebTransport v1.0.0 has been released! See the [release notes](https://github.com/carbon-os/libwebtransport/) to learn about new features, enhancements, and breaking changes.

If you aren't ready to upgrade yet, check the [tags](https://github.com/carbon-os/libwebtransport/tags) for previous stable releases.

We appreciate your feedback! Feel free to open GitHub issues or submit changes to stay updated in development and connect with the maintainers.

---

## Usage

libwebtransport is distributed as a vcpkg registry package. Follow the steps below to integrate it into your project.

### 1. Bootstrap vcpkg

In your project root, clone vcpkg if you haven't already:

```bash
git clone https://github.com/microsoft/vcpkg.git

# Linux/macOS
./vcpkg/bootstrap-vcpkg.sh

# Windows
.\vcpkg\bootstrap-vcpkg.bat
```

### 2. Configure the Registry

Add a `vcpkg-configuration.json` to your project root pointing to both the libquiche and libwebtransport registries:

```json
{
  "default-registry": {
    "kind": "git",
    "repository": "https://github.com/microsoft/vcpkg",
    "baseline": "3508985146f1b1d248c67ead13f8f54be5b4f5da"
  },
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/carbon-os/libquiche",
      "baseline": "032f24a83fa22d5a7cd9b51bb7cdbea2e21f496f",
      "packages": ["quiche", "abseil-cpp", "boringssl", "protobuf", "zlib"]
    },
    {
      "kind": "git",
      "repository": "https://github.com/carbon-os/libwebtransport",
      "baseline": "INSERT_LATEST_BASELINE_COMMIT",
      "packages": ["webtransport"]
    }
  ]
}
```

### 3. Declare the Dependency

Add a `vcpkg.json` manifest to your project root:

```json
{
  "name": "your-app",
  "version": "1.0.0",
  "dependencies": [
    "webtransport"
  ]
}
```

### 4. Install Dependencies

```bash
# Linux/macOS
./vcpkg/vcpkg install

# Windows
.\vcpkg\vcpkg.exe install
```

### 5. Configure Your Project

**Linux / macOS**
```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake
```

**Windows**
```bat
cmake -B build ^
  -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=.\vcpkg\scripts\buildsystems\vcpkg.cmake
```

### 6. Link in CMake

```cmake
find_package(webtransport CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE webtransport::webtransport)
```

---

## Generating TLS Certificates

WebTransport runs over QUIC, which requires TLS — plain `openssl` self-signed certs won't work here. QUIC and BoringSSL require certificates with **Subject Alternative Names (SANs)** and keys in **PKCS#8 format**. A Go script is included in the repo to generate a correctly formatted cert/key pair for local development.

> **Requires Go** — install from [golang.org/dl](https://golang.org/dl/) if you don't have it.

Run this from the **root of the repository**:

```bash
go run scripts/gen-keys.go
```

This generates two files in your current working directory:

| File | Description |
|---|---|
| `cert.pem` | Self-signed ECDSA P-256 certificate, valid for 365 days |
| `key.pem` | PKCS#8 private key (`0600` permissions) |

The certificate is issued for `localhost`, `127.0.0.1`, and `::1` — covering the default local development address used by the example server.

> **Note:** Do not use these files in production. For deployment, use a certificate issued by a trusted CA (e.g. Let's Encrypt).

---

## Simple API

<table>
<tr>
<th> Server </th>
<th> Client </th>
</tr>
<tr>
<td>

```cpp
#include <webtransport.h>
#include <cstdio>

int main() {
    web_transport::Server svr;

    svr.WebTransport("/chat", [](web_transport::Session& sess) {
        printf("[server] new session from %s\n",
               sess.peer_address().c_str());

        sess.on_bidi_stream([&sess](web_transport::BidirectionalStream& s) {
            s.on_data([&s, &sess](const uint8_t* data, size_t len) {
                // Echo back on the stream and as a datagram
                s.write(data, len);
                s.close_write();
                sess.send_datagram(data, len);
            });
        });

        sess.on_datagram([&sess](const uint8_t* data, size_t len) {
            sess.send_datagram(data, len); // echo
        });

        sess.on_close([](uint32_t code, const std::string& reason) {
            printf("[server] closed: code=%u reason=%s\n",
                   code, reason.c_str());
        });
    });

    svr.listen("0.0.0.0", 4433, "cert.pem", "key.pem");
    return 0;
}
```

</td>
<td>

```cpp
#include <webtransport.h>
#include <atomic>
#include <chrono>
#include <cstdio>

int main() {
    web_transport::ClientConfig cfg;
    cfg.verify_certificate = false;

    web_transport::WebTransport client("127.0.0.1:4433/chat", cfg);

    web_transport::Session* sess = client.connect(/*timeout_ms=*/5000);
    if (!sess) return 1;

    std::atomic<bool> done{false};

    sess->on_datagram([&](const uint8_t* data, size_t len) {
        printf("[client] datagram echo: %.*s\n", (int)len, data);
        done = true;
    });

    auto* s = sess->open_bidi_stream();
    s->on_data([&](const uint8_t* data, size_t len) {
        printf("[client] stream echo: %.*s\n", (int)len, data);
    });

    s->write("Hello, Server!");
    s->close_write();
    sess->send_datagram("ping!");

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(5);
    while (!done && std::chrono::steady_clock::now() < deadline)
        client.run_once(10);

    sess->close();
    client.shutdown();
    return 0;
}
```

</td>
</tr>
</table>

**[Example Applications](examples/)** contain ready-to-build server and client samples — see [examples/README.md](examples/README.md) for build instructions.

**[API Documentation](https://libwebtransport.example/docs)** provides a comprehensive reference of the public API.

Now go build something amazing! Here are some ideas to spark your creativity:
* Transfer large files in real-time with QUIC's low latency and stream multiplexing.
* Develop a real-time multiplayer game server with ultra-responsive data channels.
* Create interactive live-streaming applications featuring dynamic data exchange.
* Implement low-latency remote control and telemetry for embedded and IoT devices.
* Integrate server push and bidirectional streams for cutting-edge web applications.

---

## Prerequisites

### All Platforms
- CMake ≥ 3.20
- Git
- C++20 capable compiler
- Go (for certificate generation via `scripts/gen-keys.go`)

### Windows
- Visual Studio 2022 (MSVC v143+)
- Windows SDK

### macOS
- Xcode Command Line Tools
```bash
xcode-select --install
```

### Linux
- GCC 11+ or Clang 13+
- ICU development libraries
```bash
sudo apt install libicu-dev   # Debian/Ubuntu
sudo dnf install libicu-devel # Fedora/RHEL
```

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `vcpkg install` fails on `quiche` | Ensure internet access — the custom registry is fetched from `https://github.com/carbon-os/libquiche` |
| `Could not find webtransport` | Confirm the registry baseline in `vcpkg-configuration.json` matches a valid commit on the libwebtransport repo |
| Registry resolution failure | Ensure `vcpkg-configuration.json` and `vcpkg.json` are in the same directory |
| `ICU not found` on Linux | Run `sudo apt install libicu-dev` |
| macOS arch mismatch (M1/Intel) | Do not set `CMAKE_OSX_ARCHITECTURES` manually — the protobuf port detects the arch via `uname -m` |
| MSVC iterator debug mismatch | All deps must build with `/D_ITERATOR_DEBUG_LEVEL=0` — wipe `./vcpkg/buildtrees` and reinstall |
| TLS handshake failure at runtime | Certificates must have SANs and use PKCS#8 key format — use `go run scripts/gen-keys.go`, not plain `openssl req` |

---

### Features

#### WebTransport API
* Pure C/C++ implementation of the [WebTransport](https://www.w3.org/TR/webtransport/) API for bidirectional data streams.
* Supports both reliable (streams) and unreliable (datagrams) data delivery modes.
* Enables server push, stream multiplexing, and efficient session management.

#### QUIC & HTTP/3 Powered Connectivity
* Built on QUIC — harnessing 0-RTT connection establishment and connection migration.
* Leverages HTTP/3 for reduced latency, improved congestion control, and robust performance.
* Multiplexed streams allow concurrent data transfers without head-of-line blocking.

#### Data Streams
* Bidirectional and unidirectional streams for flexible data transfer.
* Ordered and unordered delivery options to suit various application needs.
* Customizable stream priorities and flow control mechanisms.

#### Security
* TLS 1.3 integrated within QUIC for state-of-the-art encryption.
* End-to-end secure data channels with advanced protection against network threats.

#### Pure C/C++
* Written entirely in C/C++ with no external dependencies beyond standard libraries.
* Wide platform support: Windows, macOS, Linux, FreeBSD, and more.
* Optimized for high performance with fast builds and a comprehensive test suite.
* Easily integrated into existing projects via vcpkg.

---

### Contributing

Check out the [contributing guide](https://github.com/carbon-os/libwebtransport/wiki/Contributing) to join the team of dedicated contributors making this project possible.

### License

MIT License — see [LICENSE](LICENSE) for full text