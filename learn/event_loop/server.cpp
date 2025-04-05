#include <csignal>
#include <cstddef>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

const int k_header_size = 4;
const int k_max_msg = 256;

enum class ConType { REQUEST = 0, RESPOND, END };
class Connection {
public:
  int fd = -1;
  size_t buf_size = 0;
  uint8_t buf[k_header_size + k_max_msg];
  ConType type = ConType::END;
};

class ReqCon : public Connection {
public:
  ReqCon() { type = ConType::REQUEST; }
};
class ResCon : public Connection {
public:
  ResCon() { type = ConType::RESPOND; }

public:
  size_t buf_send = 0;
};
using ConnectionPtr = std::shared_ptr<Connection>;

namespace {
namespace Internal {
int set_up_fd();
bool fd_set_nb(const int &fd);
bool connection_io(const ConnectionPtr &conn);
bool accept_new_conn(std::unordered_map<int, ConnectionPtr> &fd2Conn,
                     const int &fd);
} // namespace Internal
} // namespace

int main() {
  int fd = Internal::set_up_fd();
  std::unordered_map<int, ConnectionPtr> fd2Conn;
  std::vector<pollfd> poll_args;
  while (true) {
    poll_args.clear(); // prepare the arg for the pool()
    poll_args.push_back(
        pollfd{fd, POLL_IN, 0}); // put the listening fd on the top
    for (const auto &[conFD, conn] : fd2Conn) {
      pollfd pfd = {};
      pfd.fd = conn->fd;
      pfd.events =
          ((conn->type == ConType::REQUEST) ? POLL_IN : POLL_OUT) | POLL_ERR;
      poll_args.push_back(pfd);
    }

    if (int rc = poll(poll_args.data(), (nfds_t)(poll_args.size()), 1000);
        rc < 0) {
      std::cout << "Poll error, ec: " << rc;
    }

    for (const auto &poll_arg : poll_args) {
      if (!poll_arg.revents) {
        continue;
      }
      ConnectionPtr conn = fd2Conn[poll_arg.fd];
      Internal::connection_io(conn);
      if (conn->type == ConType::END) {
        // Client destroy normally, or something bad happened
        // Destroy this connection
        fd2Conn.erase(conn->fd);
        close(conn->fd);
      }
    }
    // Try to accept new connection if the listening fd is active
    if (poll_args.front().revents) {
      Internal::accept_new_conn(fd2Conn, fd);
    }
  }
}

namespace {
namespace Internal {
int set_up_fd() {
  int fd = socket(AF_INET, SOCK_STREAM,
                  0); // SOCK_STREAM for TCP
  if (fd < 0) {
    std::cout << "Error creating socket" << std::endl;
    return 1;
  }

  sockaddr_in addr;
  addr.sin_family = AF_INET;   // IPv4
  addr.sin_port = ntohs(8080); // Port 8080
  addr.sin_addr.s_addr = ntohl(0);
  int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (rc < 0) {
    std::cout << "Error binding to "
                 "port 8080 "
              << std::endl;
    return 1;
  }

  // Set the server fd to non-blocking mode
  fd_set_nb(fd);
  return fd;
}

bool fd_set_nb(const int &fd) {
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

bool try_one_req(ConnectionPtr conn) {
  if (sizeof(conn->buf) < 4)
    return false;
  size_t len;
  int rv = read(conn->fd, &conn->buf[0], 4);
  if (len > k_max_msg) {
    std::cout << "Request too long\n";
    conn->type = ConType::END;
    return false;
  }
  if (4 + len > conn->buf_size) {
    // There is not enough data in buffer, try to read in next iterator
    return false;
  }
  printf("Client says: %.*s\n", (int)len, &conn->buf[4]);

  ConnectionPtr resCon = std::make_shared<ResCon>();
  resCon->fd = conn->fd;
  resCon.
}

bool try_fill_buffer(ConnectionPtr conn) {
  ssize_t rv = 0;
  do {
    size_t cap = conn->buf_size - sizeof(conn->buf);
    rv = read(conn->fd, &conn->buf[conn->buf_size], cap);
  } while (rv < 0 && errno == EINTR);
  if (rv < 0 && errno == EAGAIN) {
    // Hit EAGAIN, stop
    return false;
  }
  if (rv == 0) {
    if (conn->buf_size > 0) {
      std::cout << "Unexpected EOF\n";
    } else {
      std::cout << "EOF\n";
    }
    conn->type = ConType::END;
    return false;
  }

  conn->buf_size += (size_t)rv;
  // Read explanation of pipelining to understand while there is a loop
  // Because there are many request in a connection (to save latency from
  // client)
  while (try_one_req(conn)) {
  }
  return conn->type == ConType::REQUEST;
}

bool do_request(ConnectionPtr conn) {
  while (try_fill_buffer(conn)) {
  }
  return true;
}

bool do_response(ConnectionPtr conn) {
  // TBD
  return true;
}
bool connection_io(const ConnectionPtr &conn) {
  if (conn->type == ConType::REQUEST) {
    return do_request(conn);
  } else if (conn->type == ConType::RESPOND) {
    return do_response(conn);
  }
  return true;
}

bool accept_new_conn(std::unordered_map<int, ConnectionPtr> &fd2Conn,
                     const int &fd) {
  sockaddr_in client_addr{};
  socklen_t len = sizeof(client_addr);
  int connFD = accept(fd, (sockaddr *)&client_addr, &len);
  if (connFD < 0) {
    std::cout << "Accept fd failed";
    return false;
  }
  if (!fd_set_nb(connFD)) {
    std::cout << "Can not set fd to non-blocking mode";
    return false;
  }

  ConnectionPtr conn = std::make_shared<ReqCon>();
  conn->fd = connFD;
  fd2Conn[conn->fd] = conn;
  return true;
}
} // namespace Internal
} // namespace
