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
unsigned int matchIndex[LOG_INDEX_MAX];

int main(int argc, char **argv) {
    int sock;
    Server servers[MAX_NUM_NODE] /*全ノード情報の構造体が入った配列*/;
    struct sockaddr_in *self_addr /*自身のアドレス構造体を入れるポインタ*/;
    struct sockaddr_in *peer_addr /*相手サーバーのアドレス構造体を入れるポインタ*/;
    Arg_AppendEntries arg_buffer;
    Res_AppendEntries res_buffer;
    socklen_t addr_len;
    unsigned int agreed; /*T/F*/
    unsigned int num_agreed;
    unsigned int num_node;
    unsigned int majority;
    unsigned int leaderID;
    unsigned int self_id;
    struct timeval timeout;

    if (argc != 2) {
        printf("Usage: %s <total node number>\n", argv[0]);
        exit(EXIT_SUCCESS);
    }
    currentTerm = 0;
    num_node = atoi(argv[1]);
    majority = num_node / 2 + 1;
    leaderID = LEADER_ID;
    self_id = leaderID;
    timeout.tv_sec = TIMEOUT_SEC;
    timeout.tv_usec = TIMEOUT_USEC;

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

    if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) <
        0) {
        perror("setsockopt(RCVTIMEO) failed");
        exit(EXIT_FAILURE);
    }
    if (bind(sock, (struct sockaddr *)self_addr, sizeof(*self_addr))) {
        perror("bind() failed");
        exit(EXIT_FAILURE);
    }

    printf("Program Start\n");
    sleep(STARTUP_LATANCY_SEC);

    while (1) {
        num_agreed = 1; /*自身の分*/
        /*ハートビートとしてsendをばらまく*/
        for (int i = 0; i < num_node; i++) {
            if (i == self_id)
                continue;
            peer_addr = &(servers[i].serv_addr);
            addr_len = sizeof(*peer_addr);
            memset(&arg_buffer, 0, sizeof(arg_buffer));
            // print_sockaddr_in(peer_addr, "peer_addr");
            if (sendto(sock, &arg_buffer, sizeof(arg_buffer), 0,
                       (struct sockaddr *)peer_addr, addr_len) < 0) {
                perror("sendto() failed");
                exit(EXIT_FAILURE);
            }
        }
        sleep(INTERVAL);

        /*****************************************************************************/
        /*過半数からレシーブできるまで待つ*/
        agreed = 0;
        // while (num_agreed < majority) {
        for (int i = 0; i < num_node; i++) {
            if (i == self_id)
                continue;
            peer_addr = &(servers[i].serv_addr);
            addr_len = sizeof(*peer_addr);
            // print_sockaddr_in(peer_addr, "peer_addr");
            if (recvfrom(sock, &res_buffer, sizeof(res_buffer), 0,
                         (struct sockaddr *)peer_addr, &addr_len) < 0) {
                if (errno == EWOULDBLOCK) {
                    /*フォロワーから時間内に応答がなければスキップ*/
                    servers[i].nm = dead;
                    // printf("Server[%d] recv timeout\n", i);
                    continue;
                } else {
                    /*それ以外は終了*/
                    perror("recvfrom() failed");
                    exit(EXIT_FAILURE);
                }
            } else {
                /*recv成功時*/
                printf("HB receved [%d]\t", i);
                servers[i].nm = alive;
                if (res_buffer.success == 0) {
                    num_agreed++;
                    printf("Agreed:%d\t", num_agreed);
                }
            }
            printf("\n");
        }
        // }
        if (num_agreed > majority) {
            printf("Majority Agreed\n");
            agreed = 1;
        }
        sleep(INTERVAL);
    }
    /*****************************************************************************/
}