#include <cstring>
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
  addr.sin_port = ntohs(9001); // Port 8080
  addr.sin_addr.s_addr = ntohl(0);
  int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (rc < 0) {
    std::cout << "Error connecting to server " << rc << std::endl;
    return 1;
  }

  while (true) {
    char message[260];
    std::string inputString;
    std::cout << "Enter a message: ";
    std::getline(std::cin, inputString);
    int size = inputString.length();
    memcpy(&message[0], &size, 4);
    memcpy(&message[4], inputString.c_str(), inputString.length());

    write(fd, message, sizeof(message));
    char rbuf[260];
    ssize_t n = read(fd, rbuf, sizeof(rbuf) - 1);
    ssize_t len = 0;
    memcpy(&len, &rbuf[0], 4);
    printf("Server says: %.*s\n", (int)len, &rbuf[4]);
  }
}
