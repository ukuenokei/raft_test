#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
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

/*全ノード情報の構造体が入った配列*/
Node_Info *node, *node_tail, *node_self, *node_leader;
unsigned int num_node;
unsigned int self_id;
unsigned int leaderID;

// Persistent state on all services: (RPC に応答する前に安定記憶装置を更新する)
unsigned int currentTerm;
unsigned int votedFor;
Log_Entry entries[LOG_INDEX_MAX];
unsigned int lastLogIndex;

// Volatile state on all servers:
unsigned int commitIndex;
unsigned int lastApplied;

// Volatile state on leader: (選挙後に再初期化)
// 各サーバに対して、そのサーバに送信する次のログエントリのインデックス (リーダーの最後のログインデックス + 1 に初期化)。
// Index *nextIndex;
// 各サーバに対して、そのサーバで複製されていることが分かっている最も大きいログエントリのインデックス (0に初期化され単調増加)。
// Index *matchIndex;

int init_nodeinfo() {
    Node_Info *tmp_node;
    // Index *tmp_nextIndex, *tmp_matchIndex;
    for (int i = 0; i < num_node; i++) {
        tmp_node = (Node_Info *)malloc(sizeof(Node_Info));
        if (tmp_node == NULL) {
            perror("Failed to allocate memory for node");
            return -1;
        }
        tmp_node->id = i;
        tmp_node->status = follower;
        tmp_node->nm = alive;
        tmp_node->serv_addr.sin_family = AF_INET;
        tmp_node->serv_addr.sin_addr.s_addr = inet_addr(IP);
        tmp_node->serv_addr.sin_port = htons(PORT + i);
        tmp_node->nextIndex = 1;
        tmp_node->matchIndex = 0;
        tmp_node->next = NULL;

        if (node == NULL) {
            node = tmp_node;
            node_tail = node;
        } else {
            node_tail->next = tmp_node;
            node_tail = tmp_node;
        }
        if (i == self_id) {
            node_self = tmp_node;
        }
    }
}

void cleanup_nodeinfo(void) {
    Node_Info *current_node = node;
    Node_Info *next_node;

    while (current_node != NULL) {
        next_node = current_node->next;
        free(current_node);
        current_node = next_node;
    }

    node = NULL;
    node_tail = NULL;
    node_self = NULL;
    node_leader = NULL;
}

int init_log_info() {
    Node_Info *pt_node;
    pt_node = node;
    while (pt_node) {
        pt_node->nextIndex = lastLogIndex + 1;
        pt_node->matchIndex = 0;
        pt_node = pt_node->next;
    }
}

int leader_func(int sock) {
    Node_Info *pt_node = NULL;
    unsigned int majority_agreed; /*T/F*/
    unsigned int num_agreed;
    unsigned int majority = num_node / 2 + 1;
    /*相手サーバーのアドレス構造体を入れるポインタ*/
    struct sockaddr_in *peer_addr;
    socklen_t addr_len;
    struct timeval recv_timeout;
    Arg_AppendEntries arg_buffer;
    Res_AppendEntries res_buffer;

    recv_timeout.tv_sec = TIMEOUT_SEC;
    recv_timeout.tv_usec = TIMEOUT_USEC;

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   &recv_timeout, sizeof(recv_timeout)) < 0) {
        perror("setsockopt(RCVTIMEO) failed");
        exit(EXIT_FAILURE);
    }
    init_log_info();
    while (1) {
        pt_node = node;
        while (pt_node) {
            pt_node->agreed = undecided;
            pt_node = pt_node->next;
        }

        num_agreed = 1;
        /*ハートビートとしてsendをばらまく*/
        pt_node = node;
        while (pt_node) {
            if (pt_node->id == self_id) {
                pt_node = pt_node->next;
                continue;
            }
            peer_addr = &(pt_node->serv_addr);
            addr_len = sizeof(*peer_addr);

            /*arg_bufferの初期化*/
            memset(&arg_buffer, 0, sizeof(arg_buffer));
            // print_sockaddr_in(peer_addr, "peer_addr");
            arg_buffer.term = currentTerm;
            arg_buffer.leaderId = leaderID;
            arg_buffer.prevLogIndex = pt_node->matchIndex;
            arg_buffer.prevLogTerm = entries[arg_buffer.prevLogIndex].term;
            arg_buffer.entries_len = 1; // とりあえず1件
            arg_buffer.entries = malloc(sizeof(arg_buffer.entries) * arg_buffer.entries_len);
            strcpy(arg_buffer.entries[0].log_command, LOG_MESSAGE);
            arg_buffer.leaderCommit = commitIndex;

            if (sendto(sock, &arg_buffer, sizeof(arg_buffer), 0,
                       (struct sockaddr *)peer_addr, addr_len) < 0) {
                perror("sendto() failed");
                exit(EXIT_FAILURE);
            }
            pt_node = pt_node->next;
        }
        sleep(INTERVAL);

        /*****************************************************************************/
        /*過半数からレシーブできるまで待つ*/
        majority_agreed = 0;
        pt_node = node;
        while (pt_node) {
            if (pt_node->id == self_id) {
                pt_node = pt_node->next;
                continue;
            }
            peer_addr = &(pt_node->serv_addr);
            addr_len = sizeof(*peer_addr);
            // print_sockaddr_in(peer_addr, "peer_addr");

            if (recvfrom(sock, &res_buffer, sizeof(res_buffer), 0,
                         (struct sockaddr *)peer_addr, &addr_len) < 0) {
                if (errno == EWOULDBLOCK) {
                    /*フォロワーから時間内に応答がなければスキップ*/
                    pt_node->nm = dead;
                    printf("Server[%d] recv timeout\n", pt_node->id);
                    pt_node = pt_node->next;
                    continue;
                } else {
                    /*それ以外は終了*/
                    perror("recvfrom() failed");
                    exit(EXIT_FAILURE);
                }
            } else {
                /*recv成功時*/
                printf("HB receved [%d]\t[%d]\t", pt_node->id, res_buffer.success);
                pt_node->nm = alive;
                if (res_buffer.success == SUCCESS) {
                    pt_node->agreed = agreed;
                    num_agreed++;
                    printf("Agreed:%d", num_agreed);

                    pt_node->nextIndex++;
                    pt_node->matchIndex++;
                }
            }
            printf("\n");
            pt_node = pt_node->next;
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
    res_appendentries.success = SUCCESS;
    // リーダーが認識しているタームが遅れている場合はfalse
    if (arg_appendentries->term < currentTerm) {
        res_appendentries.success = FAILURE;
    }
    // 直前のログのインデックスとタームが同一でない場合は、ログが一貫していないのでfalse
    if (entries[arg_appendentries->prevLogIndex].term != arg_appendentries->prevLogTerm ||
        lastLogIndex < arg_appendentries->prevLogIndex) {
        res_appendentries.success = FAILURE;
    }

    if (res_appendentries.success == SUCCESS) {
        // ログを自身に適用
        for (int i = 0; i < arg_appendentries->entries_len; i++) {
            entries[arg_appendentries->prevLogIndex + i].term = arg_appendentries->term;
            strcpy(entries[arg_appendentries->prevLogIndex + i].log_command, arg_appendentries->entries[i].log_command);
        }
    }

    if (arg_appendentries->leaderCommit > commitIndex) {
        commitIndex = min(arg_appendentries->leaderCommit,
                          arg_appendentries->prevLogIndex + arg_appendentries->entries_len);
    }
    currentTerm = arg_appendentries->term;
    res_appendentries.term = currentTerm;
    return res_appendentries;
}

int follower_func(int sock) {
    Node_Info *pt_node = NULL;
    unsigned int recv_retry_cnt;
    struct sockaddr_in *peer_addr /*相手サーバーのアドレス構造体を入れるポインタ*/;
    struct timeval election_timeout;
    socklen_t addr_len;

    Arg_AppendEntries arg_buffer;
    Res_AppendEntries res_buffer;

    currentTerm = 0;
    election_timeout.tv_sec = TIMEOUT_SEC;
    election_timeout.tv_usec = TIMEOUT_USEC;

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   &election_timeout, sizeof(election_timeout)) < 0) {
        perror("setsockopt(RCVTIMEO) failed");
        exit(EXIT_FAILURE);
    }
    recv_retry_cnt = 0;
    while (1) {
        /*****************************************************************************/
        peer_addr = &(node_leader->serv_addr);
        addr_len = sizeof(*peer_addr);

        if (recvfrom(sock, &arg_buffer, sizeof(arg_buffer), 0,
                     (struct sockaddr *)peer_addr, &addr_len) < 0) {
            if (errno == EWOULDBLOCK) {
                recv_retry_cnt++;
                /*選挙タイムアウトしたらリーダーの死亡認定*/
                if (recv_retry_cnt == RETRY_MAX) {
                    node_leader->nm = dead;
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
            node_leader->status = alive;
            memset(&res_buffer, 0, sizeof(res_buffer));
            // AppendEntriesを開始する
            res_buffer = AppendEntries(&arg_buffer);

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
    int res;
    Raft_Packet *packet_buf;
    struct sockaddr_in *tmp_addr, *self_addr;
    socklen_t tmp_addrlen;
    Node_Info *pt_node = NULL;

    if (argc != 3) {
        printf("Usage: %s <total node number> <my node id>\n", argv[0]);
        exit(EXIT_SUCCESS);
    }

    currentTerm = 0;
    num_node = atoi(argv[1]);
    self_id = atoi(argv[2]);

    leaderID = LEADER_ID;
    node = NULL;
    node_tail = NULL;
    node_self = NULL;

    lastLogIndex = 0;

    init_nodeinfo();

    if (self_id == leaderID) {
        node_self->serv_addr.sin_addr.s_addr = INADDR_ANY;
    }
    pt_node = node;
    while (pt_node) {
        if (pt_node->id == leaderID) {
            node_leader = pt_node;
            node_leader->status = leader;
        }

        pt_node = pt_node->next;
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
        tmp_addrlen = sizeof(struct sockaddr_in);
        res = recvfrom(sock, &packet_buf, sizeof(packet_buf), 0,
                       (struct sockaddr *)tmp_addr, &tmp_addrlen);
        if (res < 0) {
            perror("recvfrom() failed");
            goto exit;
        }
        switch (packet_buf->RPC_type) {

        case RPC_AppendEntries:
            Arg_AppendEntries arg_buffer;
            while (pt_node) {
                if (pt_node->id == self_id) {
                    pt_node = pt_node->next;
                    continue;
                }
                tmp_addr = &(pt_node->serv_addr);
                tmp_addrlen = sizeof(struct sockaddr);

                /*arg_bufferの初期化*/
                memset(&arg_buffer, 0, sizeof(arg_buffer));
                // print_sockaddr_in(peer_addr, "peer_addr");
                arg_buffer.term = currentTerm;
                arg_buffer.leaderId = leaderID;
                arg_buffer.prevLogIndex = pt_node->matchIndex;
                arg_buffer.prevLogTerm = entries[arg_buffer.prevLogIndex].term;
                arg_buffer.entries_len = 1; // とりあえず1件
                arg_buffer.entries = malloc(sizeof(arg_buffer.entries) * arg_buffer.entries_len);
                strcpy(arg_buffer.entries[0].log_command, LOG_MESSAGE);
                arg_buffer.leaderCommit = commitIndex;

                if (sendto(sock, &arg_buffer, sizeof(arg_buffer), 0,
                           (struct sockaddr *)tmp_addr, tmp_addrlen) < 0) {
                    perror("sendto() failed");
                    exit(EXIT_FAILURE);
                }
                pt_node = pt_node->next;
            }

            break;
        case RES_AppendEntries:
            pt_node->nm = alive;
            if (res_buffer.success == SUCCESS) {
                pt_node->agreed = agreed;
                num_agreed++;
                printf("Agreed:%d", num_agreed);

                pt_node->nextIndex++;
                pt_node->matchIndex++;
            }
            break;
        case RPC_RequestVote:
            follower_func(sock);
            break;

        default:
            break;
        }

        switch (node_self->status) {
        case leader:
            /* code */
            break;
        case follower:
            break;
        default:
            break;
        }
    }
/*****************************************************************************/
exit:
    cleanup_nodeinfo();
}