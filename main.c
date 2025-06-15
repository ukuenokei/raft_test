#include <arpa/inet.h>
#include <errno.h>
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
#define RECVTIMEOUT_USEC 500
#define ELECTIONTOMIN_MSEC 3000
#define ELECTIONTOMAX_MSEC 4000
#define HBINTERVAL_SEC 2
#define HBINTERVAL_USEC 0

#define MAX_FILENAME_LEN 64
#define FILENAME_LOGENTRIES "logentires%d.dat"
#define FILENAME_VOTEDFOR "votedFor%d.dat"
#define FILENAME_TERM "term%d.dat"
#define FILENAME_APPLIEDLOG "AppliedLog%d.txt"

/*全ノード情報の構造体が入った配列*/
Node_Info *node_head, *node_tail, *node_self, *node_leader;
unsigned int num_node;
unsigned int self_id;
int leaderId;
struct timespec el_to, el_std, hb_to, hb_std;

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
  FILE *fp;
  char line[MAX_LINE_LEN];
  int id;
  char ip[32];
  int port;
  int idx = 0;

  if (NULL == (fp = fopen("node_info.txt", "r"))) {
    perror("Failed to open node_info.txt");
    return -1;
  }

  while (fgets(line, sizeof(line), fp) != NULL) {
    if (sscanf(line, "%d %31s %d", &id, ip, &port) != 3) {
      continue; // フォーマット不正行はスキップ
    }
    tmp_node = (Node_Info *)malloc(sizeof(Node_Info));
    if (tmp_node == NULL) {
      perror("Failed to allocate memory for node");
      if (fp != NULL)
        fclose(fp);
      return -1;
    }
    tmp_node->id = id;
    tmp_node->status = FOLLOWER;
    tmp_node->nm = ALIVE;
    tmp_node->serv_addr.sin_family = AF_INET;
    tmp_node->serv_addr.sin_addr.s_addr = inet_addr(ip);
    tmp_node->serv_addr.sin_port = htons(port);
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
    if (id == self_id) {
      node_self = tmp_node;
    }
    idx++;
  }
  if (fp != NULL)
    fclose(fp);
  num_node = idx;
  return num_node;
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
  if (node_leader == NULL) {
    printf("Leader Node[%d] not found\n", leaderId);
    return -1;
  }
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
  // FIXME:選挙タムアウトの値を調整する
  int elto_ms = ELECTIONTOMIN_MSEC + (rand() % (ELECTIONTOMAX_MSEC - ELECTIONTOMIN_MSEC + 1));
  elto->tv_sec = elto_ms / 1000;
  elto->tv_nsec = (elto_ms % 1000) * 1000000L;
}

void raft_log(const char *fmt, ...) {
  va_list args;
  char timebuf[32];
  time_t now = time(NULL);
  struct tm *tm_info = localtime(&now);

  struct timeval tv;
  gettimeofday(&tv, NULL);
  strftime(timebuf, sizeof(timebuf), "%Y-%m-%2d %H:%M:%S", tm_info);
  snprintf(timebuf + strlen(timebuf), sizeof(timebuf) - strlen(timebuf), ".%03ld", tv.tv_usec / 1000);

  printf("[%s] [T:%u] [I:%2u] [S:%u] [A:%2u] [C:%2u] [V:%2d] [L:%2d] ",
         timebuf, currentTerm, lastLogIndex, node_self->status, lastApplied, commitIndex, votedFor, leaderId);

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

  if (fp != NULL)
    fclose(fp);
  return 0;
}

int read_term() {
  FILE *fp;
  char filename[MAX_FILENAME_LEN];
  snprintf(filename, sizeof(filename), FILENAME_TERM, node_self->id);
  memset(logEntries, 0, sizeof(logEntries));
  currentTerm = 0;
  if ((fp = fopen(filename, "rb")) != NULL) {
    fread(&currentTerm, sizeof(currentTerm), 1, fp);
  }
  if (fp != NULL)
    fclose(fp);
  return 0;
}

int dump_votedFor() {
  FILE *fp;
  char filename[MAX_FILENAME_LEN];
  snprintf(filename, sizeof(filename), FILENAME_VOTEDFOR, node_self->id);
  if (NULL == (fp = fopen(filename, "w"))) {
    perror("Cannot open VotedFor file");
    exit(EXIT_FAILURE);
  }
  fprintf(fp, "%d\n", votedFor);
  // fwrite(&votedFor, sizeof(votedFor), 1, fp);

  if (fp != NULL)
    fclose(fp);
  return 0;
}

int read_votedFor() {
  FILE *fp;
  char filename[MAX_FILENAME_LEN];
  snprintf(filename, sizeof(filename), FILENAME_VOTEDFOR, node_self->id);
  votedFor = VOTEDFOR_NULL;
  if ((fp = fopen(filename, "rb")) != NULL) {
    // fread(&votedFor, sizeof(votedFor), 1, fp);
    fscanf(fp, "%d", &votedFor);
  }
  if (fp != NULL)
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
  } else {
    for (int i = 0; i <= lastLogIndex; i++) {
      fprintf(fp, "%2d\t%2d\t%s\n", logEntries[i].term, i, logEntries[i].log_command);
    }
  }

  if (fp != NULL)
    fclose(fp);

  return 0;
}

int read_logentires() {
  FILE *fp;
  char line[MAX_LINE_LEN];
  int i = 0;
  char filename[MAX_FILENAME_LEN];
  snprintf(filename, sizeof(filename), FILENAME_LOGENTRIES, node_self->id);
  memset(logEntries, 0, sizeof(logEntries));
  lastLogIndex = 0;
  if ((fp = fopen(filename, "rb")) != NULL) {
    while (fgets(line, sizeof(line), fp) != NULL) {
      if (sscanf(line, "%d\t%d\t%s", &logEntries[i].term, &i, logEntries[i].log_command) != 3) {
        continue;
      }
      lastLogIndex++;
    }
  } else {
    logEntries[0].term = 0;
    strncpy(logEntries[0].log_command, "Initial Log Entry", MAX_COMMAND_LEN - 1);
    logEntries[0].log_command[MAX_COMMAND_LEN - 1] = '\0';
  }
  if (fp != NULL)
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
int apply_logentries(char *filename, int index) {
  FILE *fp;

  char timebuf[32];
  time_t now = time(NULL);
  struct tm *tm_info = localtime(&now);

  strftime(timebuf, sizeof(timebuf), "%Y-%m-%2d %H:%M:%S", tm_info);
  if (NULL == (fp = fopen(filename, "a"))) {
    perror("Cannot open Log file");
    exit(EXIT_FAILURE);
  }
  fprintf(fp, "%s\t%2d\t%2d\t%s\n",
          timebuf, index, logEntries[index].term, logEntries[index].log_command);
  raft_log("Log[%2d] Applied : %s", index, logEntries[index].log_command);

  if (fp != NULL)
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

  raft_log("Send AppendEntriesRPC to Node[%d]\t(prevLogIndex : [%2d] entries_len : [%d])",
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
  res_requestvote.voteGranted = false;
  // 候補者のタームが現在のタームよりも古い場合は投票しない
  if (arg_requestvote->term < currentTerm) {
    return res_requestvote;
  }
  // もし votedFor が null または candidateId であり、候補者のログが少なくとも受信者のログと同じように最新のものである場合、投票が許可される
  if (votedFor == VOTEDFOR_NULL || votedFor == arg_requestvote->candidateId) {
    if (lastLogIndex <= arg_requestvote->lastLogIndex && logEntries[lastLogIndex].term <= arg_requestvote->lastLogTerm) {
      votedFor = arg_requestvote->candidateId;
      res_requestvote.voteGranted = true;
    }
  }
  return res_requestvote;
}

int RequestVoteRPC(int sock, Node_Info *pt_node) {
  Raft_Packet send_buf;
  struct sockaddr_in tmp_addr;
  socklen_t tmp_addrlen;

  memset(&send_buf, 0, sizeof(send_buf));
  send_buf.packet_type = RPC_REQUESTVOTE;
  send_buf.id = node_self->id;
  send_buf.arg_requestvote.term = currentTerm;
  send_buf.arg_requestvote.candidateId = self_id;
  send_buf.arg_requestvote.lastLogIndex = lastLogIndex;
  send_buf.arg_requestvote.lastLogTerm = logEntries[lastLogIndex].term;

  raft_log("Send RequestVote to Node[%d]\t(Term : [%2d] lastLogIndex : [%2d] lastLogTerm : [%2d])",
           pt_node->id, currentTerm, lastLogIndex, logEntries[lastLogIndex].term);

  tmp_addr = pt_node->serv_addr;
  tmp_addrlen = sizeof(struct sockaddr_in);
  if (sendto(sock, &send_buf, sizeof(send_buf), 0,
             (struct sockaddr *)&tmp_addr, tmp_addrlen) < 0) {
    perror("sendto() failed");
    exit(EXIT_FAILURE);
  }
  return 0;
}

int main(int argc, char **argv) {
  int sock;
  int res;
  Raft_Packet recv_buf, send_buf;
  struct sockaddr_in tmp_addr, client_addr;
  socklen_t tmp_addrlen, client_addrlen;
  Node_Info *pt_node;
  Leader_Info *leader_info;
  char filename[MAX_FILENAME_LEN];
  bool exceed_hbto, exceed_elto, exceed_candto;
  unsigned int num_votes;

  struct timeval timeout;
  unsigned int tmpindex, entries_len;

  if (argc != 2) {
    printf("Usage: %s <total node number> <my node id>\n", argv[0]);
    exit(EXIT_SUCCESS);
  }
  self_id = atoi(argv[1]);

  leaderId = LEADERID_NULL;
  node_head = NULL;
  node_tail = NULL;
  node_self = NULL;
  pt_node = NULL;
  num_node = init_nodeinfo();

  commitIndex = 0;
  lastApplied = 0;
  lastLogIndex = 0;
  // FIXME: 復帰した際にPersistent stateを読み込む
  read_logentires();
  read_term();
  read_votedFor();

  timeout.tv_sec = RECVTIMEOUT_SEC;
  timeout.tv_usec = RECVTIMEOUT_USEC;
  hb_to.tv_sec = HBINTERVAL_SEC;
  hb_to.tv_nsec = HBINTERVAL_USEC;

  client_addr.sin_family = AF_INET;
  client_addr.sin_addr.s_addr = inet_addr(CLIENT_IP);
  client_addr.sin_port = htons(CLIENT_PORT);
  client_addrlen = sizeof(struct sockaddr_in);

  srand(time(NULL) + self_id);
  randomize_electionto(&el_to);

  leader_info = (Leader_Info *)malloc(sizeof(Leader_Info));
  if (leader_info == NULL) {
    perror("Failed to allocate memory for leader_info");
    exit(EXIT_FAILURE);
  }
  // init_leader(leader_info, LEADER_ID);

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

  snprintf(filename, sizeof(filename), FILENAME_APPLIEDLOG, node_self->id);
  remove(filename);
  reset_timer(&hb_std);
  reset_timer(&el_std);
  // 選挙タイムアウトを表示する
  raft_log("Program Start\tElection Timeout Value : [%ld.%09ld]", el_to.tv_sec, el_to.tv_nsec);

  while (1) {
    // ログの適用
    while (commitIndex > lastApplied) {
      lastApplied++;
      apply_logentries(filename, lastApplied);

      if (node_self->status == LEADER) {
        // クライアントに成功を返す
        send_buf.packet_type = CLIENT_RESPONSE;
        send_buf.id = node_self->id;
        send_buf.client_response.sucess = true;
        send_buf.client_response.leaderId = leaderId;

        client_addrlen = sizeof(struct sockaddr_in);
        raft_log("Send ClientResponse to Client\t(sucess : [%d])", send_buf.client_response.sucess);
        if (sendto(sock, &send_buf, sizeof(send_buf), 0,
                   (struct sockaddr *)&client_addr, client_addrlen) < 0) {
          perror("sendto() failed");
          exit(EXIT_FAILURE);
        }
      }
    }
    /**********************************************************************************************************/
    switch (node_self->status) {
    case FOLLOWER:
      exceed_elto = check_timeout(el_std, el_to);
      // 選挙タイムアウトしており、まだ選挙が始まっていなければ候補者に遷移
      if (exceed_elto && votedFor == VOTEDFOR_NULL) {
        node_self->status = CANDIDATE;
        currentTerm++;
        votedFor = self_id;
        num_votes = 1;
        raft_log("Election Timeout\tBecome Candidate\t(Term : [%2d])", currentTerm);
        reset_timer(&el_std);
      }
      break;

    case LEADER:
      if ((exceed_hbto = check_timeout(hb_std, hb_to))) {
        reset_timer(&hb_std);
      }
      pt_node = node_head;
      while (pt_node) {
        if (pt_node->id == self_id) {
          pt_node = pt_node->next;
          continue;
        }
        // 未送信ログがある場合は即送信、ハートビートタイムアウトしていたら送信
        // FIXME:失敗する(合意が得られていない)場合に無制限に処理を繰り返すようにする
        if (lastLogIndex >= pt_node->nextIndex) {
          entries_len = 1;
        } else if (exceed_hbto) {
          entries_len = 0;
        } else {
          pt_node = pt_node->next;
          continue;
        }
        AppendEntriesRPC(sock, pt_node, entries_len);
        pt_node = pt_node->next;
      }

      break;
    case CANDIDATE:
      // RequestVote RPCを送信する
      // 候補者になった瞬間はexceed_eltoがtrueになっている
      if (exceed_elto) {
        pt_node = node_head;
        while (pt_node) {
          if (pt_node->id == self_id) {
            pt_node = pt_node->next;
            continue;
          }
          RequestVoteRPC(sock, pt_node);
          pt_node = pt_node->next;
        }
      }
      // 投票が分散して候補者が決まらなかった場合、タームを増加させる
      if ((exceed_elto = check_timeout(el_std, el_to))) {
        reset_timer(&el_std);
        currentTerm++;
      }
      break;
    default:
      break;
    }

    /**********************************************************************************************************/
    tmp_addrlen = sizeof(struct sockaddr_in);
    if (recvfrom(sock, &recv_buf, sizeof(recv_buf), 0,
                 (struct sockaddr *)&tmp_addr, &tmp_addrlen) < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("recvfrom() failed");
      }
      continue;
    }
    pt_node = get_node(recv_buf.id); // pt_nodeには受信したノードの情報が入る
    if (pt_node == NULL && recv_buf.id != CLIENT_ID) {
      printf("Unknown node id: %2d\n", recv_buf.id);
      continue;
    }
    // 安定記憶装置の更新
    dump_logentries();
    dump_term();
    dump_votedFor();

    switch (recv_buf.packet_type) {
    case RPC_APPENDENTRIES:
      // if (node_self->status != FOLLOWER) {
      //     continue;
      // }
      reset_timer(&el_std);
      raft_log("Recv AppendEntriesRPC from Leader Node[%d]\t(prevLogIndex : [%2d] entries_len : [%d] term : [%2d])",
               recv_buf.id, recv_buf.arg_appendentries.prevLogIndex,
               recv_buf.arg_appendentries.entries_len, recv_buf.arg_appendentries.term);

      // 候補者の状態でAppendEntriesRPCを受信した場合
      // (その RPC に含まれる) リーダーのタームが少なくとも候補者の現在のタームと同じ大きさの場合
      // 候補者はリーダーを正当なものとして認識しフォロワー状態に戻る。
      // 新しいリーダーを認識した場合
      if (leaderId != recv_buf.arg_appendentries.leaderId && recv_buf.arg_appendentries.term >= currentTerm) {
        raft_log("Recognize New Leader\t(Term : [%2d])", recv_buf.arg_appendentries.term);
        currentTerm = recv_buf.arg_appendentries.term;
        leaderId = recv_buf.arg_appendentries.leaderId;
        if (node_self->status == CANDIDATE) {
          raft_log("Become Follower\t(Term : [%2d])", recv_buf.arg_appendentries.term);
          node_self->status = FOLLOWER;
        }
        votedFor = VOTEDFOR_NULL;
        init_leader(leader_info, leaderId);
      }

      node_leader->nm = ALIVE;
      // AppendEntriesを開始する
      send_buf.packet_type = RES_APPENDENTRIES;
      send_buf.id = node_self->id;
      send_buf.res_appendentries = AppendEntries(&recv_buf.arg_appendentries);

      tmp_addr = node_leader->serv_addr;
      tmp_addrlen = sizeof(struct sockaddr_in);
      raft_log("Send AppendEntriesRes to Leader Node[%d]\t(prevLogIndex : [%2d] entries_len : [%d] term : [%2d] success : [%d])",
               leaderId, recv_buf.arg_appendentries.prevLogIndex, recv_buf.arg_appendentries.entries_len,
               send_buf.res_appendentries.term, send_buf.res_appendentries.success);
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
      raft_log("Recv AppendEntriesRes from Node[%d]\t(prevLogIndex : [%2d] entries_len : [%d] term : [%2d] success : [%d])",
               pt_node->id, recv_buf.res_appendentries.prevLogIndex, recv_buf.res_appendentries.entries_len,
               recv_buf.res_appendentries.term, recv_buf.res_appendentries.success);
      pt_node->nm = ALIVE;
      if (recv_buf.res_appendentries.success == true) {
        // フォロワーにログを積んだ場合
        for (int i = 0; i < recv_buf.res_appendentries.entries_len; i++) {
          tmpindex = recv_buf.res_appendentries.prevLogIndex + 1 + i;
          leader_info->num_agreed[tmpindex]++;
          raft_log("Node[%d] Agreed about Log[%2d] (Agreed: %2d)", pt_node->id, tmpindex, leader_info->num_agreed[tmpindex]);
          if (leader_info->num_agreed[tmpindex] >= leader_info->majority) {
            raft_log("Majority Agreed\t(Index : [%2d] Number of Agreed: [%2d])", tmpindex, leader_info->num_agreed[tmpindex]);
            if (commitIndex < tmpindex) {
              // まだコミットされていないインデックスの場合はコミットする
              commitIndex += recv_buf.res_appendentries.entries_len;
              raft_log("Commit\t(Index : [%2d])", commitIndex);
            }
          }
          pt_node->matchIndex = recv_buf.res_appendentries.prevLogIndex + recv_buf.res_appendentries.entries_len;
          pt_node->nextIndex += recv_buf.res_appendentries.entries_len;
        }
      } else if (recv_buf.res_appendentries.success == false) {
        pt_node->nextIndex--;
        // FIXME:たまに0になることがある、、、
      }

      break;
    case RPC_REQUESTVOTE:

      // 実装中
      raft_log("Recv RequestVoteRPC from Node[%d]\t(Term : [%2d] lastLogIndex : [%2d] lastLogTerm : [%2d])",
               recv_buf.id, recv_buf.arg_requestvote.term, recv_buf.arg_requestvote.lastLogIndex, recv_buf.arg_requestvote.lastLogTerm);

      send_buf.packet_type = RES_REQUESTVOTE;
      send_buf.id = node_self->id;
      send_buf.res_requestvote = Vote(&recv_buf.arg_requestvote);
      tmp_addr = pt_node->serv_addr;
      tmp_addrlen = sizeof(struct sockaddr_in);
      raft_log("Send RequestVoteRes to Node[%d]\t(Term : [%2d] voteGranted : [%2d])",
               pt_node->id, send_buf.res_requestvote.term, send_buf.res_requestvote.voteGranted);
      if (sendto(sock, &send_buf, sizeof(send_buf), 0,
                 (struct sockaddr *)&tmp_addr, tmp_addrlen) < 0) {
        perror("sendto() failed");
        exit(EXIT_FAILURE);
      }
      break;
    case RES_REQUESTVOTE:
      if (node_self->status != CANDIDATE) {
        continue;
      }
      // 過半数のサーバから投票があった場合: リーダーに転向。
      raft_log("Recv RequestVoteRes from Node[%d]\t(Term : [%2d] voteGranted : [%2d])",
               recv_buf.id, recv_buf.res_requestvote.term, recv_buf.res_requestvote.voteGranted);
      if (recv_buf.res_requestvote.voteGranted == true) {
        num_votes++;
        // raft_log("Node[%d] Vote Granted\t(Term : [%2d] lastLogIndex : [%2d] lastLogTerm : [%2d])",
        //          recv_buf.id, recv_buf.arg_requestvote.term, recv_buf.arg_requestvote.lastLogIndex,
        //          recv_buf.arg_requestvote.lastLogTerm);
        if (num_votes >= (num_node / 2 + 1)) {
          // 過半数の投票を得た場合
          raft_log("Become Leader\t(Term : [%2d])", currentTerm);
          node_self->status = LEADER;
          votedFor = VOTEDFOR_NULL;
          init_leader(leader_info, self_id);
          // 当選したらAppendEntriesRPCを送信する
          entries_len = 0;
          pt_node = node_head;
          while (pt_node) {
            if (pt_node->id == self_id) {
              pt_node = pt_node->next;
              continue;
            }
            AppendEntriesRPC(sock, pt_node, entries_len);
            pt_node = pt_node->next;
          }
        }
      } else {
        raft_log("Node[%d] Vote Denied\t(Term : [%2d] lastLogIndex : [%2d] lastLogTerm : [%2d])",
                 recv_buf.id, recv_buf.arg_requestvote.term, recv_buf.arg_requestvote.lastLogIndex,
                 recv_buf.arg_requestvote.lastLogTerm);
      }
      break;
    case CLIENT_REQUEST:
      if (node_self->status == LEADER) {
        raft_log("Recv ClientRequest from Node[%d]\t(Index : [%2d] term : [%2d])",
                 recv_buf.id, lastLogIndex + 1, currentTerm);
        // クライアントからのログを積む
        add_logentries(recv_buf.client_request.log_command);
        // 安定記憶装置の更新
        dump_logentries();
        dump_term();
        dump_votedFor();

      } else {
        // リーダーでない場合はリクエストをリーダーIDをクライアントに送信
        send_buf.packet_type = CLIENT_RESPONSE;
        send_buf.id = node_self->id;
        send_buf.client_response.sucess = false;
        send_buf.client_response.leaderId = leaderId;
        client_addrlen = sizeof(struct sockaddr_in);
        print_sockaddr_in(&client_addr, "Client");
        raft_log("Send ClientResponse to Client\t(sucess : [%d])", send_buf.client_response.sucess);
        if (sendto(sock, &send_buf, sizeof(send_buf), 0,
                   (struct sockaddr *)&client_addr, client_addrlen) < 0) {
          perror("sendto() failed");
          exit(EXIT_FAILURE);
        }
      }
      break;
    default:
      break;
    }
    /**********************************************************************************************************/
  }
/*****************************************************************************/
exit:
  if (leader_info)
    free(leader_info);
  cleanup_nodeinfo();
  close(sock);
}