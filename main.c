#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "raft.h"
#include "testparam.h"

#define RETRY_MAX 3
#define INTERVAL 3
#define RECVTIMEOUT_SEC 1
#define RECVTIMEOUT_USEC 0

#define MAX_FILENAME_LEN 64

/*全ノード情報の構造体が入った配列*/
Node_Info *node_head, *node_tail, *node_self, *node_leader;
unsigned int num_node;
unsigned int self_id;
unsigned int leaderId;

// Persistent state on all services: (RPC に応答する前に安定記憶装置を更新する)
unsigned int currentTerm;
unsigned int votedFor;
Log_Entry log_entries[LOG_INDEX_MAX]; // 一旦配列にしておく、あとで連結リスト化しないと、、、

// Volatile state on all servers:
unsigned int commitIndex;
unsigned int lastApplied;

// Volatile state on leader: (選挙後に再初期化)
// 各サーバに対して、そのサーバに送信する次のログエントリのインデックス
// (リーダーの最後のログインデックス + 1 に初期化)。 Index *nextIndex;
// 各サーバに対して、そのサーバで複製されていることが分かっている最も大きいログエントリのインデックス
// (0に初期化され単調増加)。 Index *matchIndex;

int init_nodeinfo() {
    Node_Info *tmp_node;
    for (int i = 0; i < num_node; i++) {
        tmp_node = (Node_Info *)malloc(sizeof(Node_Info));
        if (tmp_node == NULL) {
            perror("Failed to allocate memory for node");
            return -1;
        }
        tmp_node->id = i;
        tmp_node->status = FOLLOWER;
        tmp_node->nm = ALIVE;
        tmp_node->serv_addr.sin_family = AF_INET;
        tmp_node->serv_addr.sin_addr.s_addr = inet_addr(IP);
        tmp_node->serv_addr.sin_port = htons(PORT + i);
        tmp_node->nextIndex = 1;
        tmp_node->matchIndex = 0;
        tmp_node->next = NULL;

        if (node_head == NULL) {
            node_head = tmp_node;
            node_tail = node_head;
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
    Node_Info *current_node = node_head;
    Node_Info *next_node;

    while (current_node != NULL) {
        next_node = current_node->next;
        free(current_node);
        current_node = next_node;
    }

    node_head = NULL;
    node_tail = NULL;
    node_self = NULL;
    node_leader = NULL;
}

int init_log_info() {
    Node_Info *pt_node;
    pt_node = node_head;
    while (pt_node) {
        pt_node->nextIndex = lastApplied + 1;
        pt_node->matchIndex = 0;
        pt_node = pt_node->next;
    }
}

Node_Info *get_node(unsigned int id) {
    Node_Info *current = node_head;
    while (current != NULL) {
        if (current->id == id) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

int check_timeout(struct timespec *timer, double timeout_sec) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double elapsed = (now.tv_sec - timer->tv_sec) + (now.tv_nsec - timer->tv_nsec) / 1e9;

    return (elapsed >= timeout_sec) ? 1 : 0;
}

void reset_timer(struct timespec *timer) {
    clock_gettime(CLOCK_MONOTONIC, timer);
}

void mklog_message(const char *log_message, unsigned int index) {
    char buffer[MAX_COMMAND_LEN];
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%s (Index : [%u] Term : [%u])", log_message, index, currentTerm);

    log_entries[index].term = currentTerm;

    strncpy(log_entries[index].log_command, buffer, MAX_COMMAND_LEN - 1);
    // log_entries[index].log_command[MAX_COMMAND_LEN - 1] = '\0';
}

int commit(unsigned int index, unsigned int id) {
    FILE *fp;
    char filename[MAX_FILENAME_LEN];
    char buf[MAX_COMMAND_LEN];
    snprintf(filename, sizeof(filename), "Log%d.dat", id);
    // snprintf(buf, sizeof(buf), "%d\t%d\t%s", log_entries[index].term, log_entries[index].log_command);
    if (NULL == (fp = fopen(filename, "a"))) {
        perror("Cannot open Log file");
        exit(EXIT_FAILURE);
    }
    fprintf(fp, "%d\t%d\t%s\n", index, log_entries[index].term, log_entries[index].log_command);
    fclose(fp);
    commitIndex++;
    return 0;
}

Res_AppendEntries AppendEntries(Arg_AppendEntries *arg_appendentries) {
    Res_AppendEntries res_appendentries;
    res_appendentries.success = SUCCESS;
    // リーダーが認識しているタームが遅れている場合はfalse
    if (arg_appendentries->term < currentTerm) {
        res_appendentries.success = FAILURE;
    }
    // 直前のログのインデックスとタームが同一でない場合は、ログが一貫していないのでfalse
    if (log_entries[arg_appendentries->prevLogIndex].term !=
            arg_appendentries->prevLogTerm ||
        lastApplied < arg_appendentries->prevLogIndex) {
        res_appendentries.success = FAILURE;
    }

    if (res_appendentries.success == SUCCESS) {
        log_entries[arg_appendentries->prevLogIndex].term = arg_appendentries->term;
        strcpy(log_entries[arg_appendentries->prevLogIndex].log_command,
               arg_appendentries->entries.log_command);
    }

    if (arg_appendentries->leaderCommit > commitIndex) {
        commitIndex =
            min(arg_appendentries->leaderCommit,
                arg_appendentries->prevLogIndex + arg_appendentries->entries_len);
    }
    currentTerm = arg_appendentries->term;
    res_appendentries.term = currentTerm;
    return res_appendentries;
}

int main(int argc, char **argv) {
    int sock;
    int res;
    Raft_Packet recv_buf, send_buf;
    struct sockaddr_in *tmp_addr;
    socklen_t tmp_addrlen;
    Node_Info *pt_node;
    Leader_Info *leader_info;
    struct timespec ts, to_election, to_hb;
    struct timeval timeout;
    unsigned int tmpindex;

    if (argc != 3) {
        printf("Usage: %s <total node number> <my node id>\n", argv[0]);
        exit(EXIT_SUCCESS);
    }

    currentTerm = 0;
    num_node = atoi(argv[1]);
    self_id = atoi(argv[2]);

    leaderId = LEADER_ID;
    node_head = NULL;
    node_tail = NULL;
    node_self = NULL;
    pt_node = NULL;
    commitIndex = 0;
    lastApplied = 0;

    timeout.tv_sec = RECVTIMEOUT_SEC;
    timeout.tv_usec = RECVTIMEOUT_USEC;
    memset(log_entries, 0, sizeof(log_entries));

    init_nodeinfo();

    if (self_id == leaderId) {
        leader_info = (Leader_Info *)malloc(sizeof(Leader_Info));
        if (leader_info == NULL) {
            perror("Failed to allocate memory for leader_info");
            exit(EXIT_FAILURE);
        }
        node_self->serv_addr.sin_addr.s_addr = INADDR_ANY;
    }
    pt_node = node_head;
    while (pt_node) {
        if (pt_node->id == leaderId) {
            node_leader = pt_node;
            node_leader->status = LEADER;
        }
        pt_node = pt_node->next;
    }

    if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) <
        0) {
        perror("setsockopt(RCVTIMEO) failed");
        exit(EXIT_FAILURE);
    }
    tmp_addr = &(node_self->serv_addr);
    print_sockaddr_in(tmp_addr, "self_addr");
    if (bind(sock, (struct sockaddr *)tmp_addr, sizeof(*tmp_addr))) {
        perror("bind() failed");
        exit(EXIT_FAILURE);
    }

    sleep(STARTUP_LATANCY_SEC);
    printf("Program Start\n");

    tmpindex = 0;
    log_entries[tmpindex].term = currentTerm;
    mklog_message("Program Start", tmpindex);
    commit(tmpindex, self_id);
    reset_timer(&ts);

    while (1) {
        // リーダーの送信処理
        switch (node_self->status) {
        case LEADER:
            if (!check_timeout(&ts, INTERVAL)) {
                continue;
            }
            reset_timer(&ts);
            leader_info->num_agreed = 1; // 送信前に初期化
            pt_node = node_head;
            mklog_message(LOG_MESSAGE, tmpindex);
            while (pt_node) {
                if (pt_node->id == self_id) {
                    pt_node = pt_node->next;
                    continue;
                }

                memset(&send_buf, 0, sizeof(send_buf));
                send_buf.RPC_type = RPC_APPENDENTRIES;
                // tmpindexはprevIndexを指している
                tmpindex = pt_node->nextIndex;
                send_buf.id = node_self->id;
                send_buf.arg_appendentries.term = currentTerm;
                send_buf.arg_appendentries.leaderId = leaderId;
                send_buf.arg_appendentries.prevLogIndex = tmpindex - 1;
                send_buf.arg_appendentries.prevLogTerm = log_entries[tmpindex - 1].term;
                // クライアントからのログをコピー(今はログメッセージの複製)
                // TODO:複数のログを送信できるように変える
                send_buf.arg_appendentries.entries = log_entries[tmpindex];
                send_buf.arg_appendentries.entries_len = 1;
                send_buf.arg_appendentries.leaderCommit = commitIndex;

                printf("Send HB to [%d]\n", pt_node->id);
                // print_sockaddr_in(tmp_addr, "peer_addr");
                tmp_addr = &(pt_node->serv_addr);
                tmp_addrlen = sizeof(struct sockaddr_in);
                if (sendto(sock, &send_buf, sizeof(send_buf), 0,
                           (struct sockaddr *)tmp_addr, tmp_addrlen) < 0) {
                    perror("sendto() failed");
                    exit(EXIT_FAILURE);
                }
                pt_node = pt_node->next;
            }

            break;
        case FOLLOWER:
            break;
        default:
            break;
        }
        // 受信処理
        tmp_addrlen = sizeof(struct sockaddr_in);
        if (recvfrom(sock, &recv_buf, sizeof(recv_buf), 0,
                     (struct sockaddr *)tmp_addr, &tmp_addrlen) < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("recvfrom() failed");
            }
            continue;
        }
        pt_node = get_node(recv_buf.id); // pt_nodeには受信したノードの情報が入る
        if (pt_node == NULL) {
            printf("Unknown node id: %d\n", recv_buf.id);
            continue;
        }
        switch (recv_buf.RPC_type) {
        // フォロワーがリーダーからAppendEntriesRPCを受信した場合
        case RPC_APPENDENTRIES:
            if (node_self->status != FOLLOWER) {
                continue;
            }
            printf("HB receved\n");

            node_leader->status = ALIVE;
            send_buf.RPC_type = RES_APPENDENTRIES;
            send_buf.id = node_self->id;
            send_buf.res_appendentries = AppendEntries(&recv_buf.arg_appendentries);
            // AppendEntriesを開始する
            tmp_addr = &(node_leader->serv_addr);
            tmp_addrlen = sizeof(struct sockaddr_in);
            if (sendto(sock, &send_buf, sizeof(send_buf), 0,
                       (struct sockaddr *)tmp_addr, tmp_addrlen) < 0) {
                perror("sendto() failed");
                exit(EXIT_FAILURE);
            }
            break;
        // リーダーがフォロワーからAppendEntriesの返答を受信した場合
        case RES_APPENDENTRIES:
            if (node_self->status != LEADER) {
                continue;
            }
            Res_AppendEntries res_buffer = recv_buf.res_appendentries;
            printf("HB receved [%d]\t[%d]\t", pt_node->id, res_buffer.success);
            pt_node->nm = ALIVE;
            if (res_buffer.success == SUCCESS) {
                pt_node->agreed = AGREED;
                leader_info->num_agreed++;
                printf("Agreed:%d", leader_info->num_agreed);

                pt_node->nextIndex++;
                pt_node->matchIndex++;
            } else if (res_buffer.success == FAILURE) {
                pt_node->nextIndex--;
            } else {
                perror("Error");
                goto exit;
            }
            printf("\n");
            if (leader_info->num_agreed >= leader_info->majority) {
                printf("Majority Agreed");
                commit(lastApplied + 1, self_id);
            }
            break;
        case RPC_REQUESTVOTE:
            // 後々実装
            break;

        default:
            break;
        }
    }
/*****************************************************************************/
exit:
    cleanup_nodeinfo();
    close(sock);
}