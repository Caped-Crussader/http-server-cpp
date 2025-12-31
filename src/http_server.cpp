#include "http_server.h"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

#include "http_message.h"
#include "uri.h"

namespace http_server {

HttpServer::HttpServer(const std::string &host, std::uint16_t port)
    : host_(host),
      port_(port),
      running_(false),
      worker_sock_fd_(),
      worker_epoll_fd_() {
}

void HttpServer::Start() {
  // Initialize memory pools for each worker
  for (int i = 0; i < kThreadPoolSize; i++) {
    worker_pools_[i].reset(new MemoryPool<EventData>(kPoolSizePerWorker));
  }

  // Create sockets for each worker (SO_REUSEPORT enables kernel load balancing)
  for (int i = 0; i < kThreadPoolSize; i++) {
    CreateSocket(i);
  }

  SetUpEpoll();
  running_ = true;  // Set before starting threads to avoid race condition
  
  // Each worker thread handles its own listener socket and connections
  for (int i = 0; i < kThreadPoolSize; i++) {
    worker_threads_[i] = std::thread(&HttpServer::WorkerThread, this, i);
  }
}

void HttpServer::Stop() {
  running_ = false;
  // Close epoll and socket file descriptors to wake up blocked worker threads
  for (int i = 0; i < kThreadPoolSize; i++) {
    close(worker_epoll_fd_[i]);
    close(worker_sock_fd_[i]);
  }
  for (int i = 0; i < kThreadPoolSize; i++) {
    worker_threads_[i].join();
  }
}

void HttpServer::CreateSocket(int worker_id) {
  int sock_fd;
  int opt = 1;
  sockaddr_in server_address;

  if ((sock_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0) {
    throw std::runtime_error("Failed to create a TCP socket");
  }

  // Enable SO_REUSEADDR and SO_REUSEPORT for kernel load balancing
  if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    close(sock_fd);
    throw std::runtime_error("Failed to set SO_REUSEADDR");
  }
  if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
    close(sock_fd);
    throw std::runtime_error("Failed to set SO_REUSEPORT");
  }

  // Enable TCP_NODELAY for lower latency
  if (setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
    close(sock_fd);
    throw std::runtime_error("Failed to set TCP_NODELAY");
  }

  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  inet_pton(AF_INET, host_.c_str(), &(server_address.sin_addr.s_addr));
  server_address.sin_port = htons(port_);

  if (bind(sock_fd, (sockaddr *)&server_address, sizeof(server_address)) < 0) {
    close(sock_fd);
    throw std::runtime_error("Failed to bind to socket");
  }

  if (listen(sock_fd, kBacklogSize) < 0) {
    close(sock_fd);
    std::ostringstream msg;
    msg << "Failed to listen on port " << port_;
    throw std::runtime_error(msg.str());
  }

  worker_sock_fd_[worker_id] = sock_fd;
}

void HttpServer::SetUpEpoll() {
  for (int i = 0; i < kThreadPoolSize; i++) {
    if ((worker_epoll_fd_[i] = epoll_create1(0)) < 0) {
      throw std::runtime_error(
          "Failed to create epoll file descriptor for worker");
    }
  }
}

// Combined listener and event processor for each worker thread
// With SO_REUSEPORT, each worker has its own listener socket and the kernel
// load-balances incoming connections across all workers
void HttpServer::WorkerThread(int worker_id) {
  int sock_fd = worker_sock_fd_[worker_id];
  int epoll_fd = worker_epoll_fd_[worker_id];
  sockaddr_in client_address;
  socklen_t client_len = sizeof(client_address);

  // Add the listener socket to this worker's epoll
  epoll_event listen_event;
  listen_event.events = EPOLLIN;
  listen_event.data.fd = sock_fd;  // Use fd directly for listener
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &listen_event) < 0) {
    return;  // Failed to add listener to epoll
  }

  while (running_) {
    int nfds = epoll_wait(epoll_fd, worker_events_[worker_id], kMaxEvents, 100);
    if (nfds < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (nfds == 0) continue;

    auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < nfds; i++) {
      const epoll_event& current_event = worker_events_[worker_id][i];

      // Check if this is the listener socket
      if (current_event.data.fd == sock_fd) {
        // Accept new connections
        while (true) {
          int client_fd = accept4(sock_fd, (sockaddr*)&client_address, 
                                   &client_len, SOCK_NONBLOCK);
          if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
              break;  // No more pending connections
            }
            break;  // Error
          }

          // Enable TCP_NODELAY on client socket for lower latency
          int opt = 1;
          setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

          EventData* client_data = AcquireEventData(worker_id);
          client_data->fd = client_fd;
          try {
            control_epoll_event(epoll_fd, EPOLL_CTL_ADD, client_fd, EPOLLIN, client_data);
          } catch (...) {
            ReleaseEventData(worker_id, client_data);
            close(client_fd);
          }
        }
      } else {
        // Handle client connection events
        EventData* data = reinterpret_cast<EventData*>(current_event.data.ptr);
        
        // Check for keep-alive timeout
        auto idle_duration = std::chrono::duration_cast<std::chrono::seconds>(
            now - data->last_activity).count();
        if (idle_duration > kKeepAliveTimeout) {
          // Connection has been idle too long, close it
          control_epoll_event(epoll_fd, EPOLL_CTL_DEL, data->fd);
          close(data->fd);
          ReleaseEventData(worker_id, data);
          continue;
        }
        
        if ((current_event.events & EPOLLHUP) || (current_event.events & EPOLLERR)) {
          control_epoll_event(epoll_fd, EPOLL_CTL_DEL, data->fd);
          close(data->fd);
          ReleaseEventData(worker_id, data);
        } else if ((current_event.events & EPOLLIN) || (current_event.events & EPOLLOUT)) {
          HandleEpollEvent(worker_id, epoll_fd, data, current_event.events);
        } else {
          control_epoll_event(epoll_fd, EPOLL_CTL_DEL, data->fd);
          close(data->fd);
          ReleaseEventData(worker_id, data);
        }
      }
    }
  }
}

// Memory pool helper functions
EventData* HttpServer::AcquireEventData(int worker_id) {
  return worker_pools_[worker_id]->Acquire();
}

void HttpServer::ReleaseEventData(int worker_id, EventData* data) {
  worker_pools_[worker_id]->Release(data);
}

void HttpServer::HandleEpollEvent(int worker_id, int epoll_fd, EventData *data,
                                  std::uint32_t events) {
  int fd = data->fd;
  EventData *request, *response;

  if (events == EPOLLIN) {
    request = data;
    ssize_t byte_count = recv(fd, request->buffer, kMaxBufferSize, 0);
    if (byte_count > 0) {  // we have fully received the message
      request->length = byte_count;
      request->last_activity = std::chrono::steady_clock::now();
      response = AcquireEventData(worker_id);
      response->fd = fd;
      HandleHttpData(worker_id, *request, response);
      control_epoll_event(epoll_fd, EPOLL_CTL_MOD, fd, EPOLLOUT, response);
      ReleaseEventData(worker_id, request);
    } else if (byte_count == 0) {  // client has closed connection
      control_epoll_event(epoll_fd, EPOLL_CTL_DEL, fd);
      close(fd);
      ReleaseEventData(worker_id, request);
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {  // retry
        request->fd = fd;
        control_epoll_event(epoll_fd, EPOLL_CTL_MOD, fd, EPOLLIN, request);
      } else {  // other error
        control_epoll_event(epoll_fd, EPOLL_CTL_DEL, fd);
        close(fd);
        ReleaseEventData(worker_id, request);
      }
    }
  } else {
    response = data;
    ssize_t byte_count =
        send(fd, response->buffer + response->cursor, response->length, 0);
    if (byte_count >= 0) {
      if (byte_count < static_cast<ssize_t>(response->length)) {  // there are still bytes to write
        response->cursor += byte_count;
        response->length -= byte_count;
        control_epoll_event(epoll_fd, EPOLL_CTL_MOD, fd, EPOLLOUT, response);
      } else {  // we have written the complete message
        if (response->keep_alive) {
          // Keep connection open for next request
          request = AcquireEventData(worker_id);
          request->fd = fd;
          request->last_activity = std::chrono::steady_clock::now();
          control_epoll_event(epoll_fd, EPOLL_CTL_MOD, fd, EPOLLIN, request);
        } else {
          // Close connection
          control_epoll_event(epoll_fd, EPOLL_CTL_DEL, fd);
          close(fd);
        }
        ReleaseEventData(worker_id, response);
      }
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {  // retry
        control_epoll_event(epoll_fd, EPOLL_CTL_MOD, fd, EPOLLOUT, response);
      } else {  // other error
        control_epoll_event(epoll_fd, EPOLL_CTL_DEL, fd);
        close(fd);
        ReleaseEventData(worker_id, response);
      }
    }
  }
}

void HttpServer::HandleHttpData(int worker_id, const EventData &raw_request,
                                EventData *raw_response) {
  std::string request_string(raw_request.buffer, raw_request.length), response_string;
  HttpRequest http_request;
  HttpResponse http_response;
  bool keep_alive = true;

  try {
    http_request = string_to_request(request_string);
    
    // Determine keep-alive based on HTTP version and Connection header
    std::string conn_header = http_request.header("Connection");
    std::transform(conn_header.begin(), conn_header.end(), conn_header.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    
    if (http_request.version() == HttpVersion::HTTP_1_0) {
      // HTTP/1.0 defaults to close, unless Connection: keep-alive
      keep_alive = (conn_header == "keep-alive");
    } else {
      // HTTP/1.1 defaults to keep-alive, unless Connection: close
      keep_alive = (conn_header != "close");
    }
    
    http_response = HandleHttpRequest(http_request);
  } catch (const std::invalid_argument &e) {
    http_response = HttpResponse(HttpStatusCode::BadRequest);
    http_response.SetHeader("Content-Type", "text/plain");
    http_response.SetContent(e.what());
    keep_alive = false;
  } catch (const std::logic_error &e) {
    http_response = HttpResponse(HttpStatusCode::HttpVersionNotSupported);
    http_response.SetHeader("Content-Type", "text/plain");
    http_response.SetContent(e.what());
    keep_alive = false;
  } catch (const std::exception &e) {
    http_response = HttpResponse(HttpStatusCode::InternalServerError);
    http_response.SetHeader("Content-Type", "text/plain");
    http_response.SetContent(e.what());
    keep_alive = false;
  }

  // Set Connection header in response
  if (keep_alive) {
    http_response.SetHeader("Connection", "keep-alive");
  } else {
    http_response.SetHeader("Connection", "close");
  }

  // Set response to write to client
  response_string =
      to_string(http_response, http_request.method() != HttpMethod::HEAD);
  size_t copy_length = std::min(response_string.length(), kMaxBufferSize);
  memcpy(raw_response->buffer, response_string.c_str(), copy_length);
  raw_response->length = copy_length;
  raw_response->keep_alive = keep_alive;
}

HttpResponse HttpServer::HandleHttpRequest(const HttpRequest &request) {
  auto it = request_handlers_.find(request.uri());
  if (it == request_handlers_.end()) {  // this uri is not registered
    return HttpResponse(HttpStatusCode::NotFound);
  }
  auto callback_it = it->second.find(request.method());
  if (callback_it == it->second.end()) {  // no handler for this method
    return HttpResponse(HttpStatusCode::MethodNotAllowed);
  }
  return callback_it->second(request);  // call handler to process the request
}

void HttpServer::control_epoll_event(int epoll_fd, int op, int fd,
                                     std::uint32_t events, void *data) {
  if (op == EPOLL_CTL_DEL) {
    if (epoll_ctl(epoll_fd, op, fd, nullptr) < 0) {
      throw std::runtime_error("Failed to remove file descriptor");
    }
  } else {
    epoll_event ev;
    ev.events = events;
    ev.data.ptr = data;
    if (epoll_ctl(epoll_fd, op, fd, &ev) < 0) {
      throw std::runtime_error("Failed to add file descriptor");
    }
  }
}

}  // namespace http_server
