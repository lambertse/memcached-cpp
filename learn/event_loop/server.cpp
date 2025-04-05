#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <ostream>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

const int k_header_size = 4;
const int k_max_msg = 256;
const int k_port = 9001;

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

namespace {
namespace Internal {
int set_up_fd();
bool fd_set_nb(const int &fd);
bool accept_new_conn(std::unordered_map<int, ConnectionPtr> &fd2Conn,
                     const int &fd);

bool connection_io(ConnectionPtr conn);
bool state_request(ConnectionPtr conn);
bool state_response(ConnectionPtr conn);
bool try_fill_buffer(ConnectionPtr conn);
bool try_flush_buffer(ConnectionPtr conn);

bool try_one_req(ConnectionPtr conn);
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
          ((conn->type == ConnectionType::REQUEST) ? POLL_IN : POLL_OUT) |
          POLL_ERR;
      poll_args.push_back(pfd);
    }

    if (int rc = poll(poll_args.data(), (nfds_t)(poll_args.size()), 1000);
        rc < 0) {
      std::cout << "Poll error, ec: " << rc;
    }

    // std::cout << poll_args.size() << " fd in poll\n";
    for (int i = 1; i < poll_args.size(); i++) {

      if (!poll_args[i].revents) {
        std::cout << "No event on fd " << poll_args[i].fd << std::endl;
        continue;
      }
      ConnectionPtr conn = fd2Conn[poll_args[i].fd];
      Internal::connection_io(conn);

      if (conn->type == ConnectionType::END) {
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
  addr.sin_family = AF_INET; // IPv4
  addr.sin_port = ntohs(k_port);
  addr.sin_addr.s_addr = ntohl(0);
  int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (rc < 0) {
    std::cout << "Error binding to port " << k_port << std::endl;
    return 1;
  }

  // Set the server fd to non-blocking mode
  fd_set_nb(fd);
  std::cout << "Binding server on port " << k_port << ", fd " << fd
            << std::endl;
  if (listen(fd, SOMAXCONN) < 0) {
    // SOMAXCONN is the maximum number of pending connections
    std::cout << "Error listening on socket" << std::endl;
    return 1;
  }
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

bool accept_new_conn(std::unordered_map<int, ConnectionPtr> &fd2Conn,
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
  if (!fd_set_nb(connFD)) {
    std::cout << "Can not set fd to non-blocking mode\n";
    return false;
  }

  ConnectionPtr conn = std::make_shared<Connection>();
  conn->fd = connFD;
  conn->type = ConnectionType::REQUEST;
  fd2Conn[conn->fd] = conn;

  return true;
}

bool connection_io(ConnectionPtr conn) {
  if (conn->type == ConnectionType::REQUEST) {
    return state_request(conn);
  } else if (conn->type == ConnectionType::RESPOND) {
    return state_response(conn);
  }
  return true;
}

bool state_request(ConnectionPtr conn) {
  // std::cout << "Request state from fd " << conn->fd << std::endl;
  while (try_fill_buffer(conn)) {
  }
  return true;
}

bool state_response(ConnectionPtr conn) {
  // std::cout << "Response state from fd " << conn->fd << std::endl;
  while (try_flush_buffer(conn)) {
  }
  return true;
}

bool try_fill_buffer(ConnectionPtr conn) {
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
  // Because there are many request in a connection (to save latency from
  // client)
  // while (try_one_req(conn)) {}
  try_one_req(conn);
  return conn->type == ConnectionType::REQUEST;
}

bool try_one_req(ConnectionPtr conn) {
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

  state_response(conn);

  return conn->type == ConnectionType::REQUEST;
}

bool try_flush_buffer(ConnectionPtr conn) {
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

} // namespace Internal
} // namespace
