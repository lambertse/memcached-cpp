#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <iostream>

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
  int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (rc < 0) {
    std::cout << "Error connecting to server" << std::endl;
    return 1;
  }

  while (true) {
    char message[1024];
    std::string inputString;
    std::cout << "Enter a message: ";
    std::getline(std::cin, inputString);
    memcpy(message, inputString.c_str(), inputString.length());
    message[inputString.length()] = '\0';
    if (message == "exit") {
      break;
    }
    std::cout << "Sending: " << message << std::endl;
    write(fd, message, sizeof(message));
    char rbuf[1024];
    ssize_t n = read(fd, rbuf, sizeof(rbuf) - 1);
    if (n < 0) {
      std::cout << "Error reading from socket" << std::endl;
      return 1;
    }
    rbuf[n] = '\0';
    std::cout << "Received: " << rbuf << std::endl;
  }
}
