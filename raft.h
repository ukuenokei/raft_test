#include <arpa/inet.h>
#include <stdbool.h>

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

enum RPC_Type {
    RPC_APPENDENTRIES,
    RES_APPENDENTRIES,
    RPC_REQUESTVOTE,
    RES_REQUESTVOTE
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
    enum Agreed agreed;
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
    // 保存するログエントリ (ハートビートの場合は空; FIXME:効率のため複数を送信することが可能)。
    Log_Entry entries[MAX_SEND_ENTRIES];
    // 送られたエントリの長さ(nextがNULLになるまで回せば良いから後で消せる)
    unsigned int entries_len;
    // リーダーの commitIndex。
    unsigned int leaderCommit;
} Arg_AppendEntries;

typedef struct {
    unsigned int term;
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

typedef struct {
    enum RPC_Type RPC_type;
    unsigned int id;
    union {
        Arg_AppendEntries arg_appendentries;
        Res_AppendEntries res_appendentries;
        // struct _REQUEST_VOTE_REQ	request_req;
        // struct _REQUEST_VOTE_RES	request_res;
        // クライアントリクエスト
    };
} Raft_Packet;
