# Building libwebtransport

## Prerequisites

- CMake ≥ 3.20
- Git
- C++20 compatible compiler (Clang or GCC)
- Go (for certificate generation — install from [golang.org/dl](https://golang.org/dl/))
- BoringSSL (pulled in automatically via vcpkg)

> **macOS only:** Ensure Xcode Command Line Tools are installed:
> ```bash
> xcode-select --install
> ```

---

## Build

### 1. Clone the Repository

```bash
git clone https://github.com/carbon-os/libwebtransport.git
cd libwebtransport
```

### 2. Navigate to the Examples Directory

```bash
cd examples
```

### 3. Bootstrap vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
```

### 4. Install Dependencies

This project uses a custom registry for `quiche` and its dependencies (`abseil-cpp`, `boringssl`, `protobuf`, `zlib`).

```bash
rm -rf ~/.cache/vcpkg/registries
./vcpkg/vcpkg install
```

### 5. Configure and Build

```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

---

## Running the Examples

WebTransport runs over QUIC, which requires TLS. You'll need to generate a self-signed certificate before running the server locally.

### 1. Generate a Self-Signed Certificate

Run the following from the **examples directory**:

```bash
export DYLD_LIBRARY_PATH=$(pwd)/vcpkg_installed/arm64-osx/lib:$DYLD_LIBRARY_PATH
go run scripts/gen-keys.go
```

This generates `cert.pem` and `key.pem` in your current working directory.

> If debugging with lldb, you can set the library path in the debugger instead:
> ```
> (lldb) env DYLD_LIBRARY_PATH=/path/to/libwebtransport/examples/vcpkg_installed/arm64-osx/lib
> ```

### 2. Start the Server

Binaries are output to `build/`. The server looks for `cert.pem` and `key.pem` in your current working directory by default.

```bash
./build/wt_server
```

To explicitly specify cert, key, host, and port:

```bash
./build/wt_server cert.pem key.pem 0.0.0.0 4433
```

### 3. Run the Client

In a separate terminal, start the client. It disables certificate verification for local testing and connects to `127.0.0.1:4433/chat` by default.

```bash
./build/wt_client
```

To connect to a different URL:

```bash
./build/wt_client 127.0.0.1:4433/chat
```

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `vcpkg install` fails on `quiche` | Ensure internet access — the custom registry is fetched from `https://github.com/carbon-os/libquiche` |
| TLS handshake failure at runtime | Certificates must have SANs and use PKCS#8 key format — use `go run scripts/gen-keys.go`, not plain `openssl req` |
| macOS arch mismatch (M1/Intel) | Do not set `CMAKE_OSX_ARCHITECTURES` manually — the protobuf port detects the arch via `uname -m` |