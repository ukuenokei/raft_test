#include <arpa/inet.h>

#define STARTUP_LATANCY_SEC 5
#define LOG_INDEX_MAX 100
#define MAX_COMMAND_LEN 20
#define MAX_NUM_ENTRIES 3

#define SUCCESS 0
#define FAILURE 1

int min(int a, int b) {
    return (a < b) ? a : b;
}

enum Status {
    leader,
    candidate,
    follower
};
enum Agreed {
    undecided,
    agreed,
    disagreed
};

enum NodeMap {
    alive,
    dead
};

enum RPC_Type {
    RPC_AppendEntries,
    RES_AppendEntries,
    RPC_RequestVote,
    RES_RequestVote
};

typedef struct _Index {
    unsigned int index;
    struct _Index *next;
} Index;
typedef struct _Log_Entry {
    unsigned int term;
    char log_command[MAX_COMMAND_LEN];
} Log_Entry;

typedef struct _Leader_Info {
    unsigned int majority_agreed; /*T/F*/
    unsigned int num_agreed;
    unsigned int majority;
} Leader_Info;

typedef struct _Node_Info {
    unsigned int id;
    struct sockaddr_in serv_addr;
    enum Status status;
    enum Agreed agreed;
    enum NodeMap nm;
    // Volatile state on leader: (選挙後に再初期化)
    // 各サーバに対して、そのサーバに送信する次のログエントリのインデックス (リーダーの最後のログインデックス + 1 に初期化)。
    unsigned int nextIndex;
    // 各サーバに対して、そのサーバで複製されていることが分かっている最も大きいログエントリのインデックス (0に初期化され単調増加)。
    unsigned int matchIndex;
    struct _Node_Info *next;
} Node_Info;

typedef struct {
    unsigned int term;
    unsigned int leaderId;
    unsigned int prevLogIndex;
    unsigned int prevLogTerm;
    Log_Entry *entries; // FIXME:効率のため複数送信できるようにする
    unsigned int entries_len;
    unsigned int leaderCommit;
} Arg_AppendEntries;

typedef struct {
    unsigned int term;
    unsigned int success;
} Res_AppendEntries;

typedef struct {
    enum RPC_Type RPC_type;
    union {
        Arg_AppendEntries arg_appendentries;
        Res_AppendEntries res_appendentries;
        // struct _REQUEST_VOTE_REQ	request_req;
        // struct _REQUEST_VOTE_RES	request_res;
        // 後で追加
    };
} Raft_Packet;

// typedef struct {
//     unsigned int term;
//     unsigned int success;
// } Res_RequestVote;
