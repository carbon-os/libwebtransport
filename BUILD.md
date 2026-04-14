# Building libwebtransport

## Prerequisites

- [cite_start]**CMake** ≥ 3.20 [cite: 1]
- **Git**
- [cite_start]**C++20** compatible compiler (Clang or GCC) [cite: 1]
- **OpenSSL** (for generating local development certificates)

## Build

1. **Clone the repository**

```bash
git clone https://github.com/carbon-os/libwebtransport.git
cd libwebtransport
```

2. **Bootstrap vcpkg**

```bash
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
```

3. **Install dependencies**

This project uses a custom registry for `quiche` and its dependencies (`abseil-cpp`, `boringssl`, `protobuf`, `zlib`).

```bash
rm -rf ~/.cache/vcpkg/registries
./vcpkg/vcpkg install
```

4. **Configure and build**

```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

## Running the Examples

WebTransport relies on QUIC, which requires TLS. For local development and testing, you will need to generate a self-signed certificate before running the server.

### 1. Generate a Self-Signed Certificate
Run this command from the root of the repository to generate a key and certificate. 

```bash
export DYLD_LIBRARY_PATH=$(pwd)/vcpkg_installed/arm64-osx/lib:$DYLD_LIBRARY_PATH


(lldb) env DYLD_LIBRARY_PATH=/Users/galaxy/Desktop/libwebtransport/vcpkg_installed/arm64-osx/lib

go run scripts/main.go
```

### 2. Start the Server
[cite_start]The build process outputs the example binaries into the `build/examples` directory[cite: 6]. The server defaults to looking for `cert.pem` and `key.pem` in your current working directory.

```bash
./build/examples/wt_server
```
*(Optional)* You can also explicitly specify the host, port, and certificate files:
```bash
./build/examples/wt_server 0.0.0.0 4433 cert.pem key.pem
```

### 3. Run the Client
[cite_start]In a separate terminal window, start the client [cite: 6] to connect to the server. The client is pre-configured to disable certificate verification for local testing and will connect to `127.0.0.1:4433/echo` by default.

```bash
./build/examples/wt_client
```
*(Optional)* You can pass custom arguments to test different routing:
```bash
./build/examples/wt_client 127.0.0.1 4433 /echo "Custom WebTransport Message!"
```

## Troubleshooting

- If `vcpkg install` fails on the `quiche` package, ensure you have an active internet connection — the custom registry is fetched from `https://github.com/carbon-os/libquiche`.
- On macOS, make sure Xcode Command Line Tools are installed: `xcode-select --install`