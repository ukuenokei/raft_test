#include <arpa/inet.h>

#define STARTUP_LATANCY_SEC 5
#define LOG_INDEX_MAX 100

#define SUCCESS 0
#define FAILURE 1

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

typedef struct {
    unsigned int term;
    char log_command[LOG_INDEX_MAX];
} Log_Entry;

typedef struct {
    unsigned int id;
    struct sockaddr_in serv_addr;
    enum Status status;
    enum Agreed agreed;
    enum NodeMap nm;
} Server;

typedef struct {
    unsigned int term;
    unsigned int leaderId;
    unsigned int prevLogIndex;
    unsigned int prevLogTerm;
    Log_Entry entries; // 効率のため複数送信できるようにする
    unsigned int leaderCommit;
} Arg_AppendEntries;

typedef struct {
    unsigned int term;
    unsigned int success;
} Res_AppendEntries;

// typedef struct {
//     unsigned int term;
//     unsigned int success;
// } Res_RequestVote;
