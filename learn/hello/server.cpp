#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

void doSomething(const int &fd) {
  // Do something with the connection
  char rbuf[1024];
  ssize_t n = read(fd, rbuf, sizeof(rbuf) - 1);
  if (n < 0) {
    std::cout << "Error reading from socket" << std::endl;
    return;
  }

  std::cout << "Received: " << rbuf << std::endl;
  char wbuf[] = "Hello from server!";
  write(fd, wbuf, sizeof(wbuf));
}

int main() {
  int fd = socket(AF_INET, SOCK_STREAM, 0); // SOCK_STREAM for TCP
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
    std::cout << "Error binding to port 8080" << std::endl;
    return 1;
  }

  if (listen(fd, SOMAXCONN) <
      0) { // SOMAXCONN is the maximum number of pending connections
    std::cout << "Error listening on socket" << std::endl;
    return 1;
  }

  while (true) {
    sockaddr_in client_addr = {};
    socklen_t client_addr_len = sizeof(client_addr);
    std::cout << "Waiting for connection..." << std::endl;
    int connectionFD =
        accept(fd, (struct sockaddr *)&client_addr, &client_addr_len);
    if (connectionFD < 0) {
      std::cout << "Error accepting connection, fd: " << connectionFD
                << std::endl;
      return 1;
    }
    doSomething(connectionFD);
    close(connectionFD);
  }
}
