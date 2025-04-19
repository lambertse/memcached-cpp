#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <ostream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

constexpr int k_header_size = 4;
constexpr int k_max_msg = 256;
constexpr int k_port = 9001;
constexpr int k_max_events = 10;

enum class ConnectionType { REQUEST = 0, RESPOND, END };
class Connection {
public:
  int fd = -1;
  ConnectionType type = ConnectionType::END;
  // For request connection only
  size_t rbuf_size = 0;
  char rbuf[k_header_size + k_max_msg];

  // For response connection only
  size_t wbuf_size = 0;
  size_t wbuf_sent = 0;
  char wbuf[k_header_size + k_max_msg];
};

using ConnectionPtr = std::shared_ptr<Connection>;

class ServerImpl;
class Server {
public:
  Server(const int port = 8080);
  ~Server();

  bool init();
  bool start();
  bool stop();
  bool deinit();

private:
  std::unique_ptr<ServerImpl> impl_;
};

int main() {
  Server server(k_port);
  server.init();
  server.start();
  std::this_thread::sleep_for(std::chrono::seconds(100000000));
}

namespace {
namespace Internal {} // namespace Internal
} // namespace
//

// Server private implementation
class ServerImpl {
public:
  ServerImpl(const int &port);
  ~ServerImpl();

  bool init();
  bool start();
  bool stop();
  bool deinit();

private:
  int setUpFD();
  bool setFDNonBlocking(const int &fd);
  bool acceptNewConn(std::unordered_map<int, ConnectionPtr> &fd2Conn,
                     const int &fd);

  bool connectionIO(ConnectionPtr conn);
  bool stateRequest(ConnectionPtr conn);
  bool stateResponse(ConnectionPtr conn);
  bool tryFillBuffer(ConnectionPtr conn);
  bool tryFlushBuffer(ConnectionPtr conn);

  bool doRequest(ConnectionPtr conn);

private:
  int _port;
  int _fd;
  int _ePollFD;
  std::unordered_map<int, ConnectionPtr> _fd2Conn;
  epoll_event _events[k_max_events];

  bool _stopped = false;
  std::thread _executor;
};

ServerImpl::~ServerImpl() {
  if (_fd > 0) {
    close(_fd);
  }
  if (_ePollFD > 0) {
    close(_ePollFD);
  }
  if (_executor.joinable()) {
    _executor.join();
  }
}

Server::Server(const int port) { impl_ = std::make_unique<ServerImpl>(port); }

Server::~Server() {
  if (impl_) {
    impl_->deinit();
  }
}

bool Server::init() { return impl_->init(); }
bool Server::start() { return impl_->start(); }
bool Server::stop() { return impl_->stop(); }
bool Server::deinit() { return impl_->deinit(); }

ServerImpl::ServerImpl(const int &port) : _port(port), _fd(-1) {}

bool ServerImpl::init() {
  _fd = setUpFD();
  if (_fd < 0) {
    std::cout << "Error setting up fd" << std::endl;
    return false;
  }
  _ePollFD = epoll_create1(0);
  if (_ePollFD < 0) {
    std::cout << "Error creating epoll fd" << std::endl;
    return false;
  }
  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = _fd;
  if (epoll_ctl(_ePollFD, EPOLL_CTL_ADD, _fd, &ev) < 0) {
    std::cerr << "Failed to add file descriptor to epoll: " << strerror(errno)
              << std::endl;
    close(_fd);
    close(_ePollFD);
    return 1;
  }
  return true;
}

bool ServerImpl::start() {
  _executor = std::thread([&]() {
    while (!_stopped) {
      // std::cout << "Waiting for events..." << std::endl;
      int nfds = epoll_wait(_ePollFD, _events, k_max_events, 500);

      if (nfds < 0) {
        std::cerr << "epoll_wait failed: " << strerror(errno) << std::endl;
        break;
      }

      if (nfds == 0) {
        // std::cout << "epoll_wait timeout..." << std::endl;
        continue;
      }
      for (int i = 0; i < nfds; ++i) {
        if (_events[i].data.fd == _fd) {
          acceptNewConn(_fd2Conn, _fd);
        } else if (_events[i].events & EPOLLIN) {
          // Handle existing connection
          auto conn = _fd2Conn[_events[i].data.fd];
          if (conn) {
            connectionIO(conn);
            if (conn->type == ConnectionType::END) {
              close(conn->fd);
              _fd2Conn.erase(conn->fd);
            }
          }
        }
      }
    }
  });
  return true;
}

bool ServerImpl::stop() {
  _stopped = true;
  return true;
}

bool ServerImpl::deinit() {
  if (_fd > 0) {
    close(_fd);
  }
  if (_ePollFD > 0) {
    close(_ePollFD);
  }
  return true;
}

int ServerImpl::setUpFD() {
  int fd = socket(AF_INET, SOCK_STREAM,
                  0); // SOCK_STREAM for TCP
  if (fd < 0) {
    std::cout << "Error creating socket" << std::endl;
    return 1;
  }

  sockaddr_in addr;
  addr.sin_family = AF_INET; // IPv4
  addr.sin_port = ntohs(k_port);
  addr.sin_addr.s_addr = ntohl(0);
  int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (rc < 0) {
    std::cout << "Error binding to port " << k_port << std::endl;
    return 1;
  }

  // Set the server fd to non-blocking mode
  setFDNonBlocking(fd);
  std::cout << "Binding server on port " << k_port << ", fd " << fd
            << std::endl;
  if (listen(fd, SOMAXCONN) < 0) {
    // SOMAXCONN is the maximum number of pending connections
    std::cout << "Error listening on socket" << std::endl;
    return 1;
  }
  return fd;
}

bool ServerImpl::setFDNonBlocking(const int &fd) {
  int flags;

  // Get the current file descriptor flags
  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    perror("fcntl");
    return false;
  }

  // Set the file descriptor to non-blocking mode
  flags |= O_NONBLOCK;
  if (fcntl(fd, F_SETFL, flags) == -1) {
    perror("fcntl");
    return false;
  }

  return true;
}

bool ServerImpl::acceptNewConn(std::unordered_map<int, ConnectionPtr> &fd2Conn,
                               const int &fd) {
  sockaddr_in client_addr{};
  socklen_t len = sizeof(client_addr);
  std::cout << "Waiting for new connection\n";
  int connFD = accept(fd, (sockaddr *)&client_addr, &len);
  if (connFD < 0) {
    // std::cout << "Accept fd failed\n";
    return false;
  }

  std::cout << "Accepted new connection from fd: " << connFD << std::endl;
  if (!setFDNonBlocking(connFD)) {
    std::cout << "Can not set fd to non-blocking mode\n";
    return false;
  }

  // Add connFD to epoll
  struct epoll_event ev;
  ev.events = EPOLLIN | EPOLLET; // Edge-triggered
  ev.data.fd = connFD;
  if (epoll_ctl(_ePollFD, EPOLL_CTL_ADD, connFD, &ev) < 0) {
    std::cerr << "Failed to add file descriptor to epoll: " << strerror(errno)
              << std::endl;
    close(connFD);
    return false;
  }

  ConnectionPtr conn = std::make_shared<Connection>();
  conn->fd = connFD;
  conn->type = ConnectionType::REQUEST;
  fd2Conn[conn->fd] = conn;

  return true;
}

bool ServerImpl::connectionIO(ConnectionPtr conn) {
  if (conn->type == ConnectionType::REQUEST) {
    return stateRequest(conn);
  } else if (conn->type == ConnectionType::RESPOND) {
    return stateResponse(conn);
  }
  return true;
}

bool ServerImpl::stateRequest(ConnectionPtr conn) {
  // std::cout << "Request state from fd " << conn->fd << std::endl;
  while (tryFillBuffer(conn)) {
  }
  return true;
}

bool ServerImpl::stateResponse(ConnectionPtr conn) {
  // std::cout << "Response state from fd " << conn->fd << std::endl;
  while (tryFlushBuffer(conn)) {
  }
  return true;
}

bool ServerImpl::tryFillBuffer(ConnectionPtr conn) {
  ssize_t rv = 0;
  do {
    size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
    rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
  } while (rv < 0 && errno == EINTR);
  if (rv < 0 && errno == EAGAIN) {
    // Hit EAGAIN, stop
    return false;
  }
  if (rv == 0) {
    if (conn->rbuf_size > 0) {
      std::cout << "Unexpected EOF\n";
    } else {
      std::cout << "EOF\n";
    }
    conn->type = ConnectionType::END;
    return false;
  }

  conn->rbuf_size += (size_t)rv;

  // Read explanation of pipelining to understand while there is a loop
  // Because there are many request in a connection (to save latency
  // from client) while (try_one_req(conn)) {}
  doRequest(conn);
  return conn->type == ConnectionType::REQUEST;
}

bool ServerImpl::doRequest(ConnectionPtr conn) {
  if (sizeof(conn->rbuf) < 4)
    return false;
  size_t len = 0;
  memcpy(&len, &conn->rbuf[0], 4);
  if (len > k_max_msg) {
    std::cout << "Request too long. Length: " << len << std::endl;
    conn->type = ConnectionType::END;
    return false;
  }
  if (4 + len > conn->rbuf_size) {
    // There is not enough data in buffer,
    //   try to read in next iterator
    return false;
  }
  printf("Client says: %.*s\n", (int)len, &conn->rbuf[4]);

  conn->rbuf_size = 0;
  memset(conn->rbuf, 0, sizeof(conn->rbuf));

  std::string message = "Server response kaka";
  size_t size = message.length();

  memcpy(&conn->wbuf[0], &size, 4);
  memcpy(&conn->wbuf[4], message.c_str(), message.length());
  conn->type = ConnectionType::RESPOND;
  conn->wbuf_size = size + 4;
  conn->wbuf_sent = 0;

  stateResponse(conn);

  return conn->type == ConnectionType::REQUEST;
}

bool ServerImpl::tryFlushBuffer(ConnectionPtr conn) {
  ssize_t rv = 0;
  auto remain = conn->wbuf_size - conn->wbuf_sent;
  rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
  if (rv < 0 && errno == EAGAIN) {
    std::cout << "Flush got EAGAIN\n";
    // Got EAGAIN, stop
    return false;
  }
  if (rv < 0) {
    std::cout << "Flush error, ec: " << rv << std::endl;
    conn->type = ConnectionType::END;
    return false;
  }

  conn->wbuf_sent += (ssize_t)rv;
  if (conn->wbuf_sent == conn->wbuf_size) {
    // Send done
    std::cout << "Send done, size " << conn->wbuf_size << std::endl;
    conn->type = ConnectionType::REQUEST;
    return false;
  }
  return true;
}
