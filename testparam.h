#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>

#define MAX_NUM_NODE 5
#define CLIENT_IP "127.0.0.1"
#define CLIENT_PORT 1000
#define CLIENT_ID 100

void print_sockaddr_in(const struct sockaddr_in *addr, const char *label) {
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(addr->sin_addr), ip_str, sizeof(ip_str));
    printf("[%s] IP: %s, Port: %d, Family: %d\n",
           label,
           ip_str,
           ntohs(addr->sin_port),
           addr->sin_family);
}
