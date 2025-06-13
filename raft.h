#include <arpa/inet.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>

#define LOG_INDEX_MAX 100
#define MAX_COMMAND_LEN 128
#define MAX_SEND_ENTRIES 3

#define VOTEDFOR_NULL -1

int min(int a, int b) { return (a < b) ? a : b; }

enum Status { LEADER,
              CANDIDATE,
              FOLLOWER };
enum Agreed { UNDICIDED,
              AGREED,
              DISAGREED };

enum NodeMap { ALIVE,
               DEAD };

enum Packet_Type {
    RPC_APPENDENTRIES,
    RES_APPENDENTRIES,
    RPC_REQUESTVOTE,
    RES_REQUESTVOTE,
    CLIENT_REQUEST,
    RES_CLIENT_REQUEST
};

typedef struct _Index {
    unsigned int index;
    struct _Index *next;
} Index;

typedef struct _Leader_Info {
    unsigned int majority_agreed; /*T/F*/
    unsigned int num_agreed[LOG_INDEX_MAX];
    unsigned int majority;
} Leader_Info;

typedef struct _Node_Info {
    unsigned int id;
    struct sockaddr_in serv_addr;
    enum Status status;
    enum NodeMap nm;
    // Volatile state on leader: (選挙後に再初期化)

    // 各サーバに対して、そのサーバに送信する次のログエントリのインデックス
    // (リーダーの最後のログインデックス + 1 に初期化)。
    unsigned int nextIndex;
    // 各サーバに対して、そのサーバで複製されていることが分かっている最も大きいログエントリのインデックス
    // (0に初期化され単調増加)。
    unsigned int matchIndex;
    struct _Node_Info *next;
} Node_Info;

typedef struct _Log_Entry {
    unsigned int term;
    char log_command[MAX_COMMAND_LEN];
} Log_Entry;

typedef struct {
    // リーダーのターム。
    unsigned int term;
    // フォロワーがクライアントをリダイレクトできるようにするため。
    unsigned int leaderId;
    // 新しいエントリの直前のログエントリのインデックス。
    unsigned int prevLogIndex;
    // prevLogIndex のターム。
    unsigned int prevLogTerm;
    // 送られたエントリの長さ(nextがNULLになるまで回せば良いから後で消せる)
    unsigned int entries_len;
    // 保存するログエントリ (ハートビートの場合は空; FIXME:効率のため複数を送信することが可能)。
    Log_Entry entries;
    // Log_Entry entries[MAX_SEND_ENTRIES];
    // リーダーの commitIndex。
    unsigned int leaderCommit;
} Arg_AppendEntries;

typedef struct {
    unsigned int term;
    unsigned int prevLogIndex;
    unsigned int entries_len;
    bool success;
} Res_AppendEntries;

typedef struct {
    // 候補者側のターム
    unsigned int term;
    // 投票をリクエストしている候補者
    unsigned int candidateId;
    // 候補者の最後のログエントリのインデックス
    unsigned int lastLogIndex;
    //  候補者の最後のログエントリのターム
    unsigned int lastLogTerm;
} Arg_RequestVote;

typedef struct {
    unsigned int term;
    bool voteGranted;
} Res_RequestVote;

// クライアントリクエスト
typedef struct {
    unsigned int id;
    char log_command[MAX_COMMAND_LEN];
} ClientRequest;

typedef struct {
    unsigned int leaderId; // クライアントリクエストを受け取ったリーダーのID
    unsigned int id;       // クライアントリクエストのID
    bool sucess;
} Res_ClientRequest;

typedef struct {
    enum Packet_Type packet_type;
    unsigned int id;
    union {
        Arg_AppendEntries arg_appendentries;
        Res_AppendEntries res_appendentries;
        Arg_RequestVote arg_requestvote;
        Res_RequestVote res_requestvote;
        ClientRequest client_request;
        Res_ClientRequest res_clientrequest;
    };
} Raft_Packet;

bool check_timeout(struct timespec std, struct timespec timeout) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    time_t sec_diff = now.tv_sec - std.tv_sec;
    long nsec_diff = now.tv_nsec - std.tv_nsec;

    if (nsec_diff < 0) {
        sec_diff -= 1;
        nsec_diff += 1000000000L;
    }

    if (sec_diff > timeout.tv_sec ||
        (sec_diff == timeout.tv_sec && nsec_diff >= timeout.tv_nsec)) {
        return true; // timeout
    }

    return false; // not yet
}

void reset_timer(struct timespec *timer) {
    clock_gettime(CLOCK_MONOTONIC, timer);
}
