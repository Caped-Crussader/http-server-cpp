// Defines the HTTP server object with some constants and structs
// useful for request handling and improving performance

#ifndef HTTP_SERVER_H_
#define HTTP_SERVER_H_

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "http_message.h"
#include "memory_pool.h"
#include "uri.h"

namespace http_server {

// Maximum size of an HTTP message is limited by how much bytes
// we can read or send via socket each time
// 16KB is optimal for modern networks and aligns with typical TCP window sizes
constexpr size_t kMaxBufferSize = 16384;

// Size of the memory pool for EventData objects per worker thread
constexpr size_t kPoolSizePerWorker = 2000;

// Keep-alive timeout in seconds
constexpr int kKeepAliveTimeout = 30;

struct EventData {
  EventData() : fd(0), length(0), cursor(0), keep_alive(true), 
                last_activity(std::chrono::steady_clock::now()), buffer() {}
  int fd;
  size_t length;
  size_t cursor;
  bool keep_alive;
  std::chrono::steady_clock::time_point last_activity;
  char buffer[kMaxBufferSize];
};

// A request handler should expect a request as argument and returns a response
using HttpRequestHandler_t = std::function<HttpResponse(const HttpRequest&)>;

// The server consists of:
// - 1 main thread
// - Multiple worker threads that each have their own listener socket (SO_REUSEPORT)
//   and process HTTP messages using epoll. The kernel load-balances connections.
//   The number of workers is defined by a constant
class HttpServer {
 public:
  explicit HttpServer(const std::string& host, std::uint16_t port);
  ~HttpServer() = default;

  HttpServer() = default;
  HttpServer(HttpServer&&) = default;
  HttpServer& operator=(HttpServer&&) = default;

  void Start();
  void Stop();
  void RegisterHttpRequestHandler(const std::string& path, HttpMethod method,
                                  const HttpRequestHandler_t callback) {
    Uri uri(path);
    request_handlers_[uri].insert(std::make_pair(method, std::move(callback)));
  }
  void RegisterHttpRequestHandler(const Uri& uri, HttpMethod method,
                                  const HttpRequestHandler_t callback) {
    request_handlers_[uri].insert(std::make_pair(method, std::move(callback)));
  }

  std::string host() const { return host_; }
  std::uint16_t port() const { return port_; }
  bool running() const { return running_; }

 private:
  static constexpr int kBacklogSize = 1000;
  static constexpr int kMaxConnections = 10000;
  static constexpr int kMaxEvents = 10000;
  static constexpr int kThreadPoolSize = 5;

  std::string host_;
  std::uint16_t port_;
  std::atomic<bool> running_;
  std::thread worker_threads_[kThreadPoolSize];
  int worker_sock_fd_[kThreadPoolSize];      // Each worker has its own listener socket (SO_REUSEPORT)
  int worker_epoll_fd_[kThreadPoolSize];
  epoll_event worker_events_[kThreadPoolSize][kMaxEvents];
  std::unique_ptr<MemoryPool<EventData>> worker_pools_[kThreadPoolSize];  // Memory pools for EventData
  std::map<Uri, std::map<HttpMethod, HttpRequestHandler_t>> request_handlers_;

  void CreateSocket(int worker_id);
  void SetUpEpoll();
  void WorkerThread(int worker_id);  // Combined listen + process for each worker
  void HandleEpollEvent(int worker_id, int epoll_fd, EventData* event, std::uint32_t events);
  void HandleHttpData(int worker_id, const EventData& request, EventData* response);
  HttpResponse HandleHttpRequest(const HttpRequest& request);

  // Memory pool helpers
  EventData* AcquireEventData(int worker_id);
  void ReleaseEventData(int worker_id, EventData* data);

  void control_epoll_event(int epoll_fd, int op, int fd,
                           std::uint32_t events = 0, void* data = nullptr);
};

}  // namespace http_server

#endif  // HTTP_SERVER_H_
