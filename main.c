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
#define TIMEOUT_SEC 1
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
// 各サーバに対して、そのサーバに送信する次のログエントリのインデックス
// (リーダーの最後のログインデックス + 1 に初期化)。 Index *nextIndex;
// 各サーバに対して、そのサーバで複製されていることが分かっている最も大きいログエントリのインデックス
// (0に初期化され単調増加)。 Index *matchIndex;

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
    tmp_node->status = FOLLOWER;
    tmp_node->nm = ALIVE;
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

Node_Info *get_node(unsigned int id) {
  Node_Info *current = node;
  while (current != NULL) {
    if (current->id == id) {
      return current;
    }
    current = current->next;
  }
  return NULL;
}

Res_AppendEntries AppendEntries(Arg_AppendEntries *arg_appendentries) {
  Res_AppendEntries res_appendentries;
  res_appendentries.success = SUCCESS;
  // リーダーが認識しているタームが遅れている場合はfalse
  if (arg_appendentries->term < currentTerm) {
    res_appendentries.success = FAILURE;
  }
  // 直前のログのインデックスとタームが同一でない場合は、ログが一貫していないのでfalse
  if (entries[arg_appendentries->prevLogIndex].term !=
          arg_appendentries->prevLogTerm ||
      lastLogIndex < arg_appendentries->prevLogIndex) {
    res_appendentries.success = FAILURE;
  }

  if (res_appendentries.success == SUCCESS) {
    // ログを自身に適用
    // for (int i = 0; i < arg_appendentries->entries_len; i++) {
    //   entries[arg_appendentries->prevLogIndex + i].term =
    //       arg_appendentries->term;
    //   strcpy(entries[arg_appendentries->prevLogIndex + i].log_command,
    //          arg_appendentries->entries[i].log_command);
    entries[arg_appendentries->prevLogIndex].term = arg_appendentries->term;
    strcpy(entries[arg_appendentries->prevLogIndex].log_command,
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
  pt_node = NULL;

  lastLogIndex = 0;
  struct timeval timeout;
  timeout.tv_sec = TIMEOUT_SEC;
  timeout.tv_usec = TIMEOUT_USEC;

  init_nodeinfo();

  if (self_id == leaderID) {
    leader_info = (Leader_Info *)malloc(sizeof(Leader_Info));
    if (leader_info == NULL) {
      perror("Failed to allocate memory for leader_info");
      exit(EXIT_FAILURE);
    }
    node_self->serv_addr.sin_addr.s_addr = INADDR_ANY;
  }
  pt_node = node;
  while (pt_node) {
    if (pt_node->id == leaderID) {
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

  while (1) {
    // 受信処理
    tmp_addrlen = sizeof(struct sockaddr_in);
    if (recvfrom(sock, &recv_buf, sizeof(recv_buf), 0,
                 (struct sockaddr *)tmp_addr, &tmp_addrlen) < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        // タイムアウトなので何もしない
        perror("recvfrom() failed");
      }

      // goto exit;
    }
    pt_node = get_node(recv_buf.id);  // pt_nodeには受信したノードの情報が入る
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
        }
        printf("\n");
        break;
      case RPC_REQUESTVOTE:
        // 後々実装
        break;

      default:
        break;
    }
    // リーダーの送信処理
    switch (node_self->status) {
      case LEADER:
        leader_info->num_agreed = 1;  // 送信前に初期化
        pt_node = node;
        while (pt_node) {
          if (pt_node->id == self_id) {
            pt_node = pt_node->next;
            continue;
          }
          tmp_addr = &(pt_node->serv_addr);
          tmp_addrlen = sizeof(struct sockaddr_in);

          // ループごとにsend_bufを初期化
          memset(&send_buf, 0, sizeof(send_buf));
          send_buf.RPC_type = RPC_APPENDENTRIES;
          send_buf.id = node_self->id;
          send_buf.arg_appendentries.term = currentTerm;
          send_buf.arg_appendentries.leaderId = leaderID;
          send_buf.arg_appendentries.prevLogIndex = pt_node->matchIndex;
          send_buf.arg_appendentries.prevLogTerm =
              entries[send_buf.arg_appendentries.prevLogIndex].term;
          // クライアントからのログをコピー
          strcpy(send_buf.arg_appendentries.entries.log_command, LOG_MESSAGE);
          send_buf.arg_appendentries.leaderCommit = commitIndex;
          printf("Send HB to [%d]\n", pt_node->id);
          // print_sockaddr_in(tmp_addr, "peer_addr");
          if (sendto(sock, &send_buf, sizeof(send_buf), 0,
                     (struct sockaddr *)tmp_addr, tmp_addrlen) < 0) {
            perror("sendto() failed");
            exit(EXIT_FAILURE);
          }
          pt_node = pt_node->next;
        }
        sleep(INTERVAL);
        break;
      case FOLLOWER:
        break;
      default:
        break;
    }
  }
/*****************************************************************************/
exit:
  cleanup_nodeinfo();
}