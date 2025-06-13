#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "raft.h"
#include "testparam.h"

#define RECVTIMEOUT_SEC 0
#define RECVTIMEOUT_USEC 500000
#define ELECTIONTOMIN_MSEC 3000
#define ELECTIONTOMAX_MSEC 4000
#define HBINTERVAL_SEC 1
#define HBINTERVAL_USEC 0

#define MAX_FILENAME_LEN 64
#define FILENAME_LOGENTRIES "logentires%d.dat"
#define FILENAME_TERM "term%d.dat"
#define FILENAME_APPLIEDLOG "AppliedLog%d.txt"

/*全ノード情報の構造体が入った配列*/
Node_Info *node_head, *node_tail, *node_self, *node_leader;
unsigned int num_node;
unsigned int self_id;
unsigned int leaderId;
struct timespec ts, el_to, el_std, hb_to, hb_std;

// Persistent state on all services: (RPC に応答する前に安定記憶装置を更新する)

// サーバから見えている最新のターム
//(初回起動時に 0 に初期化され単調増加する)
unsigned int currentTerm;
// 現在投票している候補者ID
unsigned int votedFor;
// ログエントリ; 各エントリにはステートマシンのコマンドおよびリーダーによってエントリが受信されたタームが含まれている
//(最初のインデックスは 1)
Log_Entry logEntries[LOG_INDEX_MAX];

// 積まれたログの長さ
unsigned int lastLogIndex;

// Volatile state on all servers:

// コミットされている(過半数に複製されている)ことが分かっているログエントリの最大インデックス
//(0に初期化され単調増加)
unsigned int commitIndex;
// 各々がステートマシンに適用されたログエントリの最大インデックス
//(0 に初期化され単調増加)
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

int init_leader(Leader_Info *leader_info, unsigned int newleaderId) {
    Node_Info *pt_node;
    leaderId = newleaderId;
    node_leader = get_node(leaderId);
    node_leader->status = LEADER;
    if (self_id == leaderId) {
        // 実装がいまいち、、、リーダーは合意したことにする
        for (int i = 0; i < LOG_INDEX_MAX; i++) {
            leader_info->num_agreed[i] = 1;
        }
        leader_info->majority = num_node / 2 + 1;
        // Volatile state on leader: (選挙後に再初期化)
        pt_node = node_head;
        while (pt_node) {
            pt_node->nextIndex = lastLogIndex + 1;
            pt_node->matchIndex = 0;
            pt_node = pt_node->next;
        }
    };
    return 0;
}

void randomize_electionto(struct timespec *elto) {

    int elto_ms = ELECTIONTOMIN_MSEC + (rand() % (ELECTIONTOMAX_MSEC - ELECTIONTOMIN_MSEC + 1));
    elto->tv_sec = elto_ms / 1000;
    elto->tv_nsec = (elto_ms % 1000) * 1000000L;
}

void raft_log(const char *fmt, ...) {
    va_list args;
    char timebuf[32];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);

    // 時刻・インデックス・タームを先頭に表示
    printf("[%s] [T: %u] [I: %u] [A: %u] [C: %u] ", timebuf, currentTerm, lastLogIndex, lastApplied, commitIndex);

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");
}

int dump_term() {
    FILE *fp;
    char filename[MAX_FILENAME_LEN];
    snprintf(filename, sizeof(filename), FILENAME_TERM, node_self->id);
    if (NULL == (fp = fopen(filename, "wb"))) {
        perror("Cannot open Log file");
        exit(EXIT_FAILURE);
    }
    fwrite(&currentTerm, sizeof(currentTerm), 1, fp);

    fclose(fp);
}

int read_term() {
    FILE *fp;
    char filename[MAX_FILENAME_LEN];
    Log_Entry buf;
    snprintf(filename, sizeof(filename), FILENAME_TERM, node_self->id);
    memset(logEntries, 0, sizeof(logEntries));
    if (NULL == (fp = fopen(filename, "rb"))) {
        perror("Cannot open Log file");
        exit(EXIT_FAILURE);
    }
    fread(&currentTerm, sizeof(currentTerm), 1, fp);
    fclose(fp);
    return 0;
}

int dump_logentries() {
    FILE *fp;
    char filename[MAX_FILENAME_LEN];
    snprintf(filename, sizeof(filename), FILENAME_LOGENTRIES, node_self->id);
    // if (NULL == (fp = fopen(filename, "wb"))) {
    //     perror("Cannot open Log file");
    //     exit(EXIT_FAILURE);
    // }
    // fwrite(logEntries, sizeof(logEntries), 1, fp);
    if (NULL == (fp = fopen(filename, "w"))) {
        perror("Cannot open Log file");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i <= lastLogIndex; i++) {
        fprintf(fp, "%d\t%d\t%s\n", logEntries[i].term, i, logEntries[i].log_command);
    }

    fclose(fp);
}

int read_logentires() {
    FILE *fp;
    char filename[MAX_FILENAME_LEN];
    snprintf(filename, sizeof(filename), FILENAME_LOGENTRIES, node_self->id);
    memset(logEntries, 0, sizeof(logEntries));
    if (NULL == (fp = fopen(filename, "rb"))) {
        perror("Cannot open Log file");
        exit(EXIT_FAILURE);
    }
    fread(logEntries, sizeof(logEntries), 1, fp);
    fclose(fp);
    return 0;
}

// クライアントから来たログを積む(Leader)
void add_logentries(const char *log_message) {
    char buffer[MAX_COMMAND_LEN];
    lastLogIndex++;
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%s (Index : [%u] Term : [%u])", log_message, lastLogIndex, currentTerm);
    logEntries[lastLogIndex].term = currentTerm;
    strncpy(logEntries[lastLogIndex].log_command, buffer, MAX_COMMAND_LEN - 1);
    logEntries[lastLogIndex].log_command[MAX_COMMAND_LEN - 1] = '\0';
}

// 積まれたログをステートマシンに適用する(テキストファイルへの書き出し)
int apply_logentries(char *filename) {
    FILE *fp;

    char timebuf[32];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);
    if (NULL == (fp = fopen(filename, "a"))) {
        perror("Cannot open Log file");
        exit(EXIT_FAILURE);
    }
    while (commitIndex > lastApplied) {
        lastApplied++;
        fprintf(fp, "%s\t%d\t%d\t%s\n",
                timebuf, lastApplied, logEntries[lastApplied].term, logEntries[lastApplied].log_command);
        raft_log("Log[%d] Applied : %s", lastApplied, logEntries[lastApplied].log_command);
    }

    fclose(fp);

    return 0;
}

Res_AppendEntries AppendEntries(Arg_AppendEntries *arg_appendentries) {
    Res_AppendEntries res_appendentries;
    Log_Entry *buf;
    unsigned int tmpindex;

    currentTerm = arg_appendentries->term;
    res_appendentries.term = currentTerm;
    res_appendentries.prevLogIndex = arg_appendentries->prevLogIndex;
    res_appendentries.entries_len = arg_appendentries->entries_len;
    res_appendentries.success = true;
    // リーダーが認識しているタームが遅れている場合はfalse
    if (arg_appendentries->term < currentTerm) {
        res_appendentries.success = false;
        return res_appendentries;
    }
    // 直前のログのインデックスとタームが同一でない場合は、ログが一貫していないのでfalse
    if (logEntries[arg_appendentries->prevLogIndex].term != arg_appendentries->prevLogTerm ||
        lastLogIndex < arg_appendentries->prevLogIndex) {
        res_appendentries.success = false;
        return res_appendentries;
    }

    //  ログを複製
    if (arg_appendentries->entries_len != 0) {
        for (int i = 0; i < arg_appendentries->entries_len; i++) {
            tmpindex = arg_appendentries->prevLogIndex + 1 + i;
            // tmpindex = arg_appendentries->prevLogIndex + 1;
            // 既存のエントリが新しいエントリと競合する場合 (同じインデックスだが異なるターム)
            if (logEntries[tmpindex].term != arg_appendentries->entries.term) {
                // 既存のエントリとそれに続くものをすべて削除 (0埋めしてるだけ)
                buf = (Log_Entry *)malloc(sizeof(Log_Entry));
                memset(buf, 0, sizeof(Log_Entry));
                for (int j = tmpindex; j <= lastLogIndex; j++) {
                    logEntries[j] = *buf;
                }
                free(buf);
            }
            lastLogIndex = tmpindex;
            logEntries[lastLogIndex] = arg_appendentries->entries;
        }
    }
    // 自身のコミットインデックスを修正
    if (arg_appendentries->leaderCommit > commitIndex) {
        commitIndex =
            min(arg_appendentries->leaderCommit, lastLogIndex);
    }
    return res_appendentries;
}

int AppendEntriesRPC(int sock, Node_Info *pt_node, unsigned int entries_len) {
    Raft_Packet send_buf;
    struct sockaddr_in tmp_addr;
    socklen_t tmp_addrlen;

    memset(&send_buf, 0, sizeof(send_buf));
    send_buf.packet_type = RPC_APPENDENTRIES;
    send_buf.id = node_self->id;
    send_buf.arg_appendentries.term = currentTerm;
    send_buf.arg_appendentries.leaderId = leaderId;
    send_buf.arg_appendentries.prevLogIndex = pt_node->nextIndex - 1;
    send_buf.arg_appendentries.prevLogTerm = logEntries[pt_node->nextIndex - 1].term;
    send_buf.arg_appendentries.entries_len = entries_len;
    // for (int i = 0; i < entries_len; i++) {
    //     send_buf.arg_appendentries.entries[i] = logEntries[pt_node->nextIndex + i];
    // }
    send_buf.arg_appendentries.entries = logEntries[pt_node->nextIndex];
    send_buf.arg_appendentries.leaderCommit = commitIndex;

    raft_log("Send AppendEntriesRPC to Node[%d]\t(prevLogIndex : [%d] entries_len: [%d])",
             pt_node->id, pt_node->nextIndex - 1, entries_len);

    tmp_addr = pt_node->serv_addr;
    tmp_addrlen = sizeof(struct sockaddr_in);
    if (sendto(sock, &send_buf, sizeof(send_buf), 0,
               (struct sockaddr *)&tmp_addr, tmp_addrlen) < 0) {
        perror("sendto() failed");
        exit(EXIT_FAILURE);
    }
    return 0;
}

Res_RequestVote Vote(Arg_RequestVote *arg_requestvote) {
    Res_RequestVote res_requestvote;
    res_requestvote.term = currentTerm;
    res_requestvote.voteGranted = true;
    // 候補者のタームが現在のタームよりも古い場合は投票しない
    if (arg_requestvote->term < currentTerm) {
        res_requestvote.voteGranted = false;
        return res_requestvote;
    }
    // もし votedFor が null または candidateId であり、候補者のログが少なくとも受信者のログと同じように最新のものである場合、投票が許可される
    if (votedFor != VOTEDFOR_NULL && votedFor != arg_requestvote->candidateId) {
        if (lastLogIndex > arg_requestvote->lastLogIndex || logEntries[lastLogIndex].term > arg_requestvote->lastLogTerm) {
            res_requestvote.voteGranted = false;
            return res_requestvote;
        }
    }
    return res_requestvote;
}

int main(int argc, char **argv) {
    int sock;
    int res;
    Raft_Packet recv_buf, send_buf;
    struct sockaddr_in tmp_addr;
    socklen_t tmp_addrlen;
    Node_Info *pt_node;
    Leader_Info *leader_info;
    char filename[MAX_FILENAME_LEN];

    struct timeval timeout;
    unsigned int tmpindex, entries_len;

    if (argc != 3) {
        printf("Usage: %s <total node number> <my node id>\n", argv[0]);
        exit(EXIT_SUCCESS);
    }
    num_node = atoi(argv[1]);
    self_id = atoi(argv[2]);

    leaderId = LEADER_ID;
    node_head = NULL;
    node_tail = NULL;
    node_self = NULL;
    pt_node = NULL;
    init_nodeinfo();

    currentTerm = 0;
    commitIndex = 0;
    lastApplied = 0;
    lastLogIndex = 0;

    timeout.tv_sec = RECVTIMEOUT_SEC;
    timeout.tv_usec = RECVTIMEOUT_USEC;
    hb_to.tv_sec = HBINTERVAL_SEC;
    hb_to.tv_nsec = HBINTERVAL_USEC;

    srand(time(NULL) + self_id);
    randomize_electionto(&el_to);

    memset(logEntries, 0, sizeof(logEntries));
    leader_info = (Leader_Info *)malloc(sizeof(Leader_Info));
    if (leader_info == NULL) {
        perror("Failed to allocate memory for leader_info");
        exit(EXIT_FAILURE);
    }
    init_leader(leader_info, LEADER_ID);

    if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt(RCVTIMEO) failed");
        exit(EXIT_FAILURE);
    }

    tmp_addr = node_self->serv_addr;
    print_sockaddr_in(&tmp_addr, "self_addr");
    if (bind(sock, (struct sockaddr *)&tmp_addr, sizeof(struct sockaddr))) {
        perror("bind() failed");
        exit(EXIT_FAILURE);
    }

    sleep(STARTUP_LATANCY_SEC);
    snprintf(filename, sizeof(filename), FILENAME_APPLIEDLOG, node_self->id);
    remove(filename);
    raft_log("Program Start");
    tmpindex = 0;
    // add_logentries("Program Start");
    reset_timer(&hb_std);

    while (1) {
        for (int i = lastLogIndex - 2; i <= lastLogIndex; i++) {
            if (i > 0) {
                printf("%d\t%s\n", i, logEntries[i].log_command);
            }
        }
        apply_logentries(filename);
        // リーダーの送信処理
        switch (node_self->status) {
        case FOLLOWER:
            if (!check_timeout(el_std, el_to)) {
                break;
            }
            // raft_log("HBTO");
            reset_timer(&el_std);

            break;

        case LEADER:
            pt_node = node_head;
            while (pt_node) {
                if (pt_node->id == self_id) {
                    pt_node = pt_node->next;
                    continue;
                }
                // 未送信ログがある場合は即送信、なければハートビートタイムアウト時のみ送信
                if (lastLogIndex >= pt_node->nextIndex) {
                    entries_len = 1;
                } else if (check_timeout(hb_std, hb_to)) {
                    entries_len = 0;
                } else {
                    pt_node = pt_node->next;
                    continue;
                }
                AppendEntriesRPC(sock, pt_node, entries_len);
                pt_node = pt_node->next;
            }
            if (check_timeout(hb_std, hb_to)) {
                reset_timer(&hb_std);
            }
            break;
        default:
            break;
        }
        // 受信処理
        tmp_addrlen = sizeof(struct sockaddr_in);
        if (recvfrom(sock, &recv_buf, sizeof(recv_buf), 0,
                     (struct sockaddr *)&tmp_addr, &tmp_addrlen) < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("recvfrom() failed");
            }
            continue;
        }
        switch (recv_buf.packet_type) {
        // フォロワーがリーダーからAppendEntriesRPCを受信した場合
        case RPC_APPENDENTRIES:
            if (node_self->status != FOLLOWER) {
                continue;
            }
            raft_log("Recv AppendEntriesRPC from Leader\t(prevLogIndex : [%d] entries_len : [%d] term : [%d])",
                     recv_buf.arg_appendentries.prevLogIndex, recv_buf.arg_appendentries.entries_len, recv_buf.arg_appendentries.term);
            node_leader->status = ALIVE;

            // 安定記憶装置の更新
            dump_logentries();
            dump_term();

            // AppendEntriesを開始する
            send_buf.packet_type = RES_APPENDENTRIES;
            send_buf.id = node_self->id;
            send_buf.res_appendentries = AppendEntries(&recv_buf.arg_appendentries);

            tmp_addr = node_leader->serv_addr;
            tmp_addrlen = sizeof(struct sockaddr_in);
            raft_log("Send AppendEntriesRes to Leader Node[%d]\t(prevLogIndex : [%d] term : [%d] success : [%d])",
                     leaderId, recv_buf.arg_appendentries.prevLogIndex, send_buf.res_appendentries.term, send_buf.res_appendentries.success);
            if (sendto(sock, &send_buf, sizeof(send_buf), 0,
                       (struct sockaddr *)&tmp_addr, tmp_addrlen) < 0) {
                perror("sendto() failed");
                exit(EXIT_FAILURE);
            }
            break;
        // リーダーがフォロワーからAppendEntriesの返答を受信した場合
        case RES_APPENDENTRIES:
            if (node_self->status != LEADER) {
                continue;
            }
            pt_node = get_node(recv_buf.id); // pt_nodeには受信したノードの情報が入る
            if (pt_node == NULL) {
                printf("Unknown node id: %d\n", recv_buf.id);
                continue;
            }
            raft_log("Recv AppendEntriesRes from Node[%d]\t(prevLogIndex : [%d] entries_len : [%d] term : [%d] success : [%d])",
                     pt_node->id, recv_buf.res_appendentries.prevLogIndex, recv_buf.res_appendentries.entries_len,
                     recv_buf.res_appendentries.term, recv_buf.res_appendentries.success);
            pt_node->nm = ALIVE;
            if (recv_buf.res_appendentries.success == true) {
                // フォロワーにログを積んだ場合
                for (int i = 0; i < recv_buf.res_appendentries.entries_len; i++) {
                    // nextIndexには各ノードに直近送ったインデックスが入っている
                    tmpindex = recv_buf.res_appendentries.prevLogIndex + 1 + i;
                    leader_info->num_agreed[tmpindex]++;
                    raft_log("Node[%d] Agreed about Log[%d] (Agreed: %d)", pt_node->id, tmpindex, leader_info->num_agreed[tmpindex]);
                    if (leader_info->num_agreed[tmpindex] >= leader_info->majority) {
                        raft_log("Majority Agreed\t(Index : [%d] Number of Agreed: [%d])", tmpindex, leader_info->num_agreed[tmpindex]);
                        if (commitIndex < tmpindex) {
                            // まだコミットされていないインデックスの場合はコミットする
                            commitIndex++;
                            raft_log("Commit\t(Index : [%d])", commitIndex);
                        }
                    }
                    pt_node->nextIndex += recv_buf.res_appendentries.entries_len;
                    pt_node->matchIndex += recv_buf.res_appendentries.entries_len;
                }
            } else if (recv_buf.res_appendentries.success == false) {
                pt_node->nextIndex--;
                pt_node->matchIndex--;
            } else {
                perror("Error\n");
                goto exit;
            }

            break;
        case RPC_REQUESTVOTE:
            // 後々実装
            break;
        case RES_REQUESTVOTE:
            // 後々実装
            break;
        case CLIENT_REQUEST:
            if (node_self->status == LEADER) {
                raft_log("Recv ClientRequest from Node[%d]\t(Index : [%d] term : [%d])",
                         recv_buf.id, lastLogIndex + 1, currentTerm);
                // クライアントからのログを積む
                add_logentries(recv_buf.client_request.log_command);
                // 安定記憶装置の更新
                dump_logentries();
                dump_term();
            } else {
                // リーダーでない場合はリクエストをリーダーにリダイレクトする
                if (node_leader != NULL) {
                    send_buf = recv_buf;
                    tmp_addr = node_leader->serv_addr;
                    tmp_addrlen = sizeof(struct sockaddr_in);
                    raft_log("Redirect ClientRequest to Leader Node[%d]", node_leader->id);
                    if (sendto(sock, &send_buf, sizeof(send_buf), 0,
                               (struct sockaddr *)&tmp_addr, tmp_addrlen) < 0) {
                        perror("sendto() failed");
                        exit(EXIT_FAILURE);
                    }
                } else {
                    printf("Leader unknown. Cannot redirect client request.\n");
                }
            }
            break;
        default:
            break;
        }
    }
/*****************************************************************************/
exit:
    if (leader_info)
        free(leader_info);
    cleanup_nodeinfo();
    close(sock);
}