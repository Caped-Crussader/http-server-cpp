# http-server-cpp

A high-performance HTTP/1.1 server implementation in C++ using epoll-based event-driven architecture with multi-threading for concurrent request handling.

## Features

- **HTTP/1.1 compliant** server with support for GET and HEAD methods
- **Multi-threaded architecture** with configurable thread pool
- **Event-driven I/O** using Linux epoll for efficient connection handling
- **Non-blocking sockets** for high concurrency
- **Customizable request handlers** with support for multiple endpoints

## Building the Server

### Prerequisites

- CMake 3.10 or higher
- C++11 compatible compiler (g++, clang++)
- POSIX-compliant system (Linux/Unix)
- pthread library

### Build Instructions

```bash
# Create build directory
mkdir -p build
cd build

# Generate build files and compile
cmake ..
make

# The executables will be generated in the build directory:
# - HttpServer (main server executable)
# - test_HttpServer (test executable)
```

## Running the Server

### Start the Server

```bash
cd build
./HttpServer
```

The server will start on `0.0.0.0:8080` by default.

### Stop the Server

Press **Enter** or type `quit` and press Enter to gracefully stop the server.

### Default Endpoints

- `/` - Returns "Hello, world" (plain text)
- `/hello.html` - Returns an HTML page with greeting

## Configuration Parameters

### Configurable in `src/main.cpp`

| Parameter | Default Value | Description |
|-----------|--------------|-------------|
| `host` | `"0.0.0.0"` | Server bind address |
| `port` | `8080` | Server listening port |

### Server Constants in `src/http_server.h`

| Constant | Default Value | Description |
|----------|--------------|-------------|
| `kMaxBufferSize` | `4096` | Maximum size of HTTP message buffer (bytes) |
| `kBacklogSize` | `1000` | Maximum length of pending connections queue |
| `kMaxConnections` | `10000` | Maximum number of simultaneous connections |
| `kMaxEvents` | `10000` | Maximum number of events per epoll_wait call |
| `kThreadPoolSize` | `5` | Number of worker threads for request processing |

To modify these parameters:
1. Edit the values in `src/main.cpp` (for host/port) or `src/http_server.h` (for server constants)
2. Rebuild the project using `make` in the build directory

## Benchmarking with wrk

### Install wrk

#### Ubuntu/Debian
```bash
sudo apt-get install wrk
```

#### Build from source
```bash
git clone https://github.com/wg/wrk.git
cd wrk
make
sudo cp wrk /usr/local/bin/
```

### Running Benchmarks

#### Basic test
```bash
wrk -t4 -c100 -d30s http://localhost:8080/
```

#### High load test
```bash
wrk -t12 -c400 -d30s http://localhost:8080/
```

#### Test HTML endpoint
```bash
wrk -t4 -c100 -d30s http://localhost:8080/hello.html
```

### wrk Parameters

- `-t` : Number of threads to use
- `-c` : Number of connections to keep open
- `-d` : Duration of test (e.g., 30s, 1m)
- `-H` : Add custom header (e.g., `-H "Accept: text/html"`)

### Example Output
```
Running 1m test @ http://localhost:8080/
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    12.95ms   85.59ms 942.25ms   97.53%
    Req/Sec    51.67k     6.82k   69.66k    87.65%
  12377486 requests in 1.03m, 0.90GB read
Requests/sec: 200297.32
Transfer/sec:     14.90MB
```

## Architecture

- **Main Thread**: Initializes server and handles shutdown
- **Listener Thread**: Accepts new connections and distributes them to workers
- **Worker Threads** (5 by default): Process HTTP requests using epoll for event handling

Each worker thread maintains its own epoll instance for efficient event multiplexing.

## Testing

Run the included test suite:

```bash
cd build
./test_HttpServer
```

## License

This project is open sourced. 
