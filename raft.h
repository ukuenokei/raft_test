#include <arpa/inet.h>

#define STARTUP_LATANCY_SEC 5
#define LOG_INDEX_MAX 100

enum Status {
    leader,
    candidate,
    follower
};
enum Agreed {
    agreed,
    disagreed
};

enum NodeMap {
    alive,
    dead
};

typedef struct {
    unsigned int id;
    struct sockaddr_in serv_addr;
    enum Status status;
    enum NodeMap nm;
} Server;

typedef struct {
    unsigned int term;
    unsigned int leaderId;
    unsigned int prevLogIndex;
    unsigned int prevLogTerm;
    char entries[LOG_INDEX_MAX];
    unsigned int leaderCommit;
} Arg_AppendEntries;

typedef struct {
    unsigned int term;
    unsigned int success;
} Res_AppendEntries;

// typedef struct {
//   unsigned int term;
//   unsigned int success;
// } Res_RequestVote;
