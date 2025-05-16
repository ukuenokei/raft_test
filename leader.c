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

#define INTERVAL 3
#define TIMEOUT_SEC 1
#define TIMEOUT_USEC 0

unsigned int currentTerm;
unsigned int votedFor;
char log[LOG_INDEX_MAX];

unsigned int commitIndex;
unsigned int lastApplied;

unsigned int nextIndex[LOG_INDEX_MAX];
unsigned int matcheIndex[LOG_INDEX_MAX];

int main() {
    int sock;
    Server servers[NUM_NODE] /*全ノード情報の構造体が入った配列*/;
    struct sockaddr_in *self_addr /*自身のアドレス構造体を入れるポインタ*/;
    struct sockaddr_in *peer_addr /*相手サーバーのアドレス構造体を入れるポインタ*/;
    Arg_AppendEntries arg_buffer;
    Res_AppendEntries res_buffer;
    socklen_t addr_len;
    unsigned int num_agreed;
    unsigned int num_node = NUM_NODE;
    unsigned int majority = num_node / 2 + 1;
    unsigned int leaderID = LEADER_ID;
    unsigned int self_id = leaderID;
    struct timeval timeout;

    currentTerm = 0;
    timeout.tv_sec = TIMEOUT_SEC;
    timeout.tv_usec = TIMEOUT_USEC;
    if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) <
        0) {
        perror("setsockopt(RCVTIMEO) failed");
        exit(EXIT_FAILURE);
    }
    /*サーバー情報の初期化*/
    memset(servers, 0, sizeof(servers));
    for (int i = 0; i < num_node; i++) {
        servers[i].id = i;
        servers[i].status = follower;
        servers[i].nm = alive;
        servers[i].serv_addr.sin_family = AF_INET;
        servers[i].serv_addr.sin_addr.s_addr = inet_addr(IP);
        servers[i].serv_addr.sin_port = htons(PORT + i);
    }
    servers[self_id].status = leader;
    self_addr = &(servers[self_id].serv_addr);
    self_addr->sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *)self_addr, sizeof(*self_addr))) {
        perror("bind() failed");
        exit(EXIT_FAILURE);
    }
    printf("Program Start\n");
    while (1) {
        num_agreed = 0;
        /*ハートビートとしてsendをばらまく*/
        for (int i = 0; i < num_node; i++) {
            if (i == self_id)
                continue;
            peer_addr = &(servers[i].serv_addr);
            addr_len = sizeof(*peer_addr);
            print_sockaddr_in(peer_addr, "peer_addr");
            if (sendto(sock, &arg_buffer, sizeof(arg_buffer), 0,
                       (struct sockaddr *)peer_addr, addr_len) < 0) {
                perror("sendto() failed");
                exit(EXIT_FAILURE);
            }
        }
        sleep(INTERVAL);
        /*****************************************************************************/
        /*過半数からレシーブできるまで待つ*/
        num_agreed++; /*自身の分*/
        while (1) {
            for (int i = 0; i < num_node; i++) {
                if (i == self_id)
                    continue;
                peer_addr = &(servers[i].serv_addr);
                addr_len = sizeof(peer_addr);
                if (recvfrom(sock, &res_buffer, sizeof(res_buffer), 0,
                             (struct sockaddr *)peer_addr, &addr_len) < 0) {
                    if (errno == EWOULDBLOCK) {
                        /*フォロワーから時間内に応答がなければスキップ*/
                        servers[i].nm = dead;
                        printf("Server[%d] recv timeout\n", i);
                        continue;
                    } else {
                        /*それ以外は終了*/
                        perror("recvfrom() failed");
                        exit(EXIT_FAILURE);
                    }
                } else {
                    /*recv成功時*/
                    servers[i].nm = alive;
                    if (res_buffer.sucess != 0) {
                        num_agreed++;
                        printf("Agreed:%d\n", num_agreed);
                    }
                    if (num_agreed >= majority) {
                        printf("Majority alive\n");
                    }
                }
            }
        }
    }
    /*****************************************************************************/
}