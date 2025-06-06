#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "raft.h"
#include "testparam.h"

#define REQUEST_INTERVAL_SEC 2

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_in leader_addr;
    Raft_Packet send_pkt, recv_pkt;
    socklen_t addrlen;
    int leader_id = 0;               // 送信先リーダーID（必要に応じて変更）
    struct timeval timeout = {1, 0}; // 1秒タイムアウト

    // ソケット作成
    if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }

    // 受信タイムアウト設定
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt() failed");
        exit(EXIT_FAILURE);
    }

    memset(&leader_addr, 0, sizeof(leader_addr));
    leader_addr.sin_family = AF_INET;
    leader_addr.sin_addr.s_addr = inet_addr(IP);
    leader_addr.sin_port = htons(PORT + 100);
    addrlen = sizeof(leader_addr);

    int req_id = 1;
    while (1) {
        // 送信パケット作成
        memset(&send_pkt, 0, sizeof(send_pkt));
        send_pkt.RPC_type = RPC_APPENDENTRIES; // 本来はRPC_CLIENTREQUESTだが未定義の場合はこれで
        send_pkt.id = req_id;
        snprintf(send_pkt.client_request.log_command, MAX_COMMAND_LEN, "client_request_%d", req_id);

        // 送信
        if (sendto(sock, &send_pkt, sizeof(send_pkt), 0,
                   (struct sockaddr *)&leader_addr, addrlen) < 0) {
            perror("sendto() failed");
        } else {
            printf("Sent ClientRequest: %s\n", send_pkt.client_request.log_command);
        }

        // 応答受信
        ssize_t n = recvfrom(sock, &recv_pkt, sizeof(recv_pkt), 0,
                             (struct sockaddr *)&leader_addr, &addrlen);
        if (n > 0 && recv_pkt.RPC_type == RES_CLIENTREQUEST) {
            printf("Received Res_ClientRequest: id=%d, leaderId=%d, success=%d\n",
                   recv_pkt.res_clientrequest.id,
                   recv_pkt.res_clientrequest.leaderId,
                   recv_pkt.res_clientrequest.sucess);
        } else {
            printf("No response or unexpected response\n");
        }

        req_id++;
        sleep(REQUEST_INTERVAL_SEC);
    }

    close(sock);
    return 0;
}