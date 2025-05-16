#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "raft.h"
#include "testparam.h"

#define TIMEOUT_SEC 5
#define TIMEOUT_USEC 0
#define INTERVAL 5

unsigned int currentTerm;
unsigned int votedFor;
char log[LOG_INDEX_MAX];

unsigned int commitIndex;
unsigned int lastApplied;

unsigned int nextIndex[LOG_INDEX_MAX];
unsigned int matcheIndex[LOG_INDEX_MAX];

enum NodeMap nm[NUM_NODE];

int main(int argc, char **argv) {
    int sock;
    Server servers[NUM_NODE] /*全ノード情報の構造体が入った配列*/;
    struct sockaddr_in *self_addr /*自身のアドレス構造体を入れるポインタ*/;
    struct sockaddr_in *peer_addr /*相手サーバーのアドレス構造体を入れるポインタ*/;
    Arg_AppendEntries arg_buffer;
    Res_AppendEntries res_buffer;
    unsigned int self_id;
    unsigned int leaderID = LEADER_ID;
    unsigned int addr_len;
    unsigned int num_node = NUM_NODE;
    unsigned int majority = num_node / 2 + 1;
    struct timeval election_timeout;

    if (argc != 2) {
        printf("Usage: %s node number\n", argv[0]);
        exit(EXIT_SUCCESS);
    }

    currentTerm = 0;
    self_id = atoi(argv[1]);
    election_timeout.tv_sec = TIMEOUT_SEC;
    election_timeout.tv_usec = TIMEOUT_USEC;

    if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &election_timeout, sizeof(election_timeout)) <
        0) {
        perror("setsockopt(RCVTIMEO) failed");
        exit(EXIT_FAILURE);
    }
    /*サーバー情報の初期化*/
    for (int i = 0; i < num_node; i++) {
        memset(&(servers[i]), 0, sizeof(servers[i]));
        servers[i].id = i;
        servers[i].status = follower;
        servers[i].nm = alive;
        servers[i].serv_addr.sin_family = AF_INET;
        servers[i].serv_addr.sin_addr.s_addr = inet_addr(IP);
        servers[i].serv_addr.sin_port = htons(PORT + i);
    }

    self_addr = &(servers[self_id].serv_addr);
    self_addr->sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *)self_addr, sizeof(*self_addr))) {
        perror("bind() failed");
        exit(EXIT_FAILURE);
    }
    while (1) {
        /*****************************************************************************/
        peer_addr = &(servers[leaderID].serv_addr);
        addr_len = sizeof(*peer_addr);
        if (recvfrom(sock, &arg_buffer, sizeof(arg_buffer), 0,
                     (struct sockaddr *)peer_addr, &addr_len) < 0) {
            if (errno == EWOULDBLOCK) {
                /*選挙タイムアウトしたらリーダーの死亡認定*/
                nm[leaderID] = dead;
                printf("Leader Dead\n");
            } else {
                /*それ以外は終了*/
                perror("recvfrom() failed");
                exit(EXIT_FAILURE);
            }
        } else {
            /*成功応答*/
            memset(&res_buffer, 0, sizeof(res_buffer));
            if (sendto(sock, &res_buffer, sizeof(res_buffer), 0,
                       (struct sockaddr *)peer_addr, addr_len) < 0) {
                perror("sendto() failed");
                exit(EXIT_FAILURE);
            }
        }
        /*****************************************************************************/
    }
}
