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

#define RETRY_MAX 3
#define INTERVAL 3
#define TIMEOUT_SEC 3
#define TIMEOUT_USEC 0

Server servers[MAX_NUM_NODE] /*全ノード情報の構造体が入った配列*/;
unsigned int num_node;
unsigned int self_id;
unsigned int leaderID;

unsigned int currentTerm;
unsigned int votedFor;
Log_Entry log[LOG_INDEX_MAX];
unsigned int logLength;

unsigned int commitIndex;
unsigned int lastApplied;

unsigned int nextIndex[LOG_INDEX_MAX];
unsigned int matchIndex[LOG_INDEX_MAX];

int leader_func(int sock) {
    unsigned int majority_agreed; /*T/F*/
    unsigned int num_agreed;
    unsigned int majority = num_node / 2 + 1;

    struct sockaddr_in *peer_addr /*相手サーバーのアドレス構造体を入れるポインタ*/;
    socklen_t addr_len;
    struct timeval recv_timeout;
    recv_timeout.tv_sec = TIMEOUT_SEC;
    recv_timeout.tv_usec = TIMEOUT_USEC;
    Arg_AppendEntries arg_buffer;
    Res_AppendEntries res_buffer;

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout)) <
        0) {
        perror("setsockopt(RCVTIMEO) failed");
        exit(EXIT_FAILURE);
    }

    while (1) {
        for (int i = 0; i < num_node; i++) {
            servers[i].agreed = undecided;
        }
        num_agreed = 1;
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
        majority_agreed = 0;
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
                    printf("Server[%d] recv timeout\n", i);
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
                    servers[i].agreed = agreed;
                    num_agreed++;
                    printf("Agreed:%d\t", num_agreed);
                }
            }
            printf("\n");
        }

        if (num_agreed > majority) {
            printf("Majority Agreed\n");
            majority_agreed = 1;
        }
        sleep(INTERVAL);
    }
}

Res_AppendEntries AppendEntries(Arg_AppendEntries *arg_appendentries) {
    Res_AppendEntries res_appendentries;
    // リーダーが認識しているタームが遅れている
    if (arg_appendentries->term < currentTerm) {
        res_appendentries.success = FAILURE;
    }
    if (log[arg_appendentries->prevLogIndex].term != arg_appendentries->prevLogTerm) {
        res_appendentries.success = FAILURE;
    }
    if (logLength <= arg_appendentries->prevLogIndex) {
        res_appendentries.success = FAILURE;
    }

    res_appendentries.term = currentTerm;
    return res_appendentries;
}

int follower_func(int sock) {
    unsigned int recv_retry_cnt;
    struct sockaddr_in *peer_addr /*相手サーバーのアドレス構造体を入れるポインタ*/;
    struct timeval election_timeout;
    socklen_t addr_len;

    Arg_AppendEntries arg_buffer;
    Res_AppendEntries res_buffer;

    currentTerm = 0;
    election_timeout.tv_sec = TIMEOUT_SEC;
    election_timeout.tv_usec = TIMEOUT_USEC;

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &election_timeout, sizeof(election_timeout)) <
        0) {
        perror("setsockopt(RCVTIMEO) failed");
        exit(EXIT_FAILURE);
    }
    recv_retry_cnt = 0;
    while (1) {
        /*****************************************************************************/
        peer_addr = &(servers[leaderID].serv_addr);
        addr_len = sizeof(*peer_addr);

        if (recvfrom(sock, &arg_buffer, sizeof(arg_buffer), 0,
                     (struct sockaddr *)peer_addr, &addr_len) < 0) {
            if (errno == EWOULDBLOCK) {
                recv_retry_cnt++;
                /*選挙タイムアウトしたらリーダーの死亡認定*/
                if (recv_retry_cnt == RETRY_MAX) {
                    servers[leaderID].nm = dead;
                    printf("Leader Dead\n");
                    recv_retry_cnt = 0;
                    // 選挙を開始する
                }
            } else {
                /*それ以外は終了*/
                perror("recvfrom() failed");
                exit(EXIT_FAILURE);
            }
        } else {
            /*成功応答*/
            printf("HB receved\n");
            recv_retry_cnt = 0;
            servers[leaderID].status = alive;
            memset(&res_buffer, 0, sizeof(res_buffer));
            // AppendEntriesを開始する

            if (sendto(sock, &res_buffer, sizeof(res_buffer), 0,
                       (struct sockaddr *)peer_addr, addr_len) < 0) {
                perror("sendto() failed");
                exit(EXIT_FAILURE);
            }
        }
        sleep(INTERVAL);
        /*****************************************************************************/
    }
}

int main(int argc, char **argv) {
    int sock;
    struct sockaddr_in *self_addr /*自身のアドレス構造体を入れるポインタ*/;

    if (argc != 3) {
        printf("Usage: %s <total node number> <my node id>\n", argv[0]);
        exit(EXIT_SUCCESS);
    }

    currentTerm = 0;
    num_node = atoi(argv[1]);
    self_id = atoi(argv[2]);

    leaderID = LEADER_ID;

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

    self_addr = &(servers[self_id].serv_addr);

    if (self_id == leaderID) {
        servers[self_id].status = leader;
        self_addr->sin_addr.s_addr = INADDR_ANY;
    }

    if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }

    print_sockaddr_in(self_addr, "self_addr");
    if (bind(sock, (struct sockaddr *)self_addr, sizeof(*self_addr))) {
        perror("bind() failed");
        exit(EXIT_FAILURE);
    }

    printf("Program Start\n");
    sleep(STARTUP_LATANCY_SEC);

    while (1) {
        switch (servers[self_id].status) {
        case leader:
            /* code */
            leader_func(sock);
            break;
        case follower:
            follower_func(sock);
            break;

        default:
            break;
        }
    }
    /*****************************************************************************/
}