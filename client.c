#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "raft.h"
#include "testparam.h"

Node_Info *node_head, *node_tail, *node_self, *node_leader;
unsigned int num_node;
unsigned int self_id;
int leaderId;

#define REQUEST_INTERVAL_SEC 5

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
      continue;
    }
    tmp_node = (Node_Info *)malloc(sizeof(Node_Info));
    if (tmp_node == NULL) {
      perror("Failed to allocate memory for node");
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
  fclose(fp);
  num_node = idx;
  return 0;
}

int main(int argc, char *argv[]) {
  int sock;
  Raft_Packet send_pkt, recv_pkt;
  Node_Info *pt_node;
  struct sockaddr_in tmp_addr;
  socklen_t tmp_addrlen;
  unsigned int leader_id = 0;      // 送信先リーダーID（必要に応じて変更）
  struct timeval timeout = {1, 0}; // 1秒タイムアウト
  struct timespec *ts;

  // ソケット作成
  if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
    perror("socket() failed");
    exit(EXIT_FAILURE);
  }

  // 受信タイムアウト設定
  //   if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
  //     perror("setsockopt() failed");
  //     exit(EXIT_FAILURE);
  //   }
  if (init_nodeinfo() < 0) {
    perror("read_node_info() failed");
    exit(EXIT_FAILURE);
  }
  tmp_addr.sin_family = AF_INET;
  tmp_addr.sin_addr.s_addr = inet_addr(CLIENT_IP);
  tmp_addr.sin_port = htons(CLIENT_PORT);
  if (bind(sock, (struct sockaddr *)&tmp_addr, sizeof(struct sockaddr))) {
    perror("bind() failed");
    exit(EXIT_FAILURE);
  }
  int req_id = 1;
  pt_node = get_node(leaderId);
  while (1) {
    // 送信処理
    memset(&send_pkt, 0, sizeof(send_pkt));
    send_pkt.packet_type = CLIENT_REQUEST;
    send_pkt.id = CLIENT_ID;
    snprintf(send_pkt.client_request.log_command, MAX_COMMAND_LEN, "client_request_%d", req_id);
    tmp_addr = pt_node->serv_addr;
    tmp_addrlen = sizeof(struct sockaddr_in);
    if (sendto(sock, &send_pkt, sizeof(send_pkt), 0,
               (struct sockaddr *)&tmp_addr, tmp_addrlen) < 0) {
      perror("sendto() failed");
    } else {
      printf("Sent ClientRequest to Node[%d] : %s\n", pt_node->id, send_pkt.client_request.log_command);
    }

    // 受信処理
    tmp_addr = pt_node->serv_addr;
    tmp_addrlen = sizeof(struct sockaddr_in);
    if (recvfrom(sock, &recv_pkt, sizeof(recv_pkt), 0, (struct sockaddr *)&tmp_addr, &tmp_addrlen) < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("recvfrom() failed");
      }
      continue;
    }
    if (recv_pkt.packet_type == CLIENT_RESPONSE) {
      printf("Received ClientResponse: %d\n", recv_pkt.client_response.sucess);
      if (recv_pkt.client_response.sucess) {
        printf("ClientRequest completed\n");
        req_id++;
      } else {
        printf("ClientRequest failed\n");
        if (leaderId != recv_pkt.client_response.leaderId) {
          leader_id = recv_pkt.client_response.leaderId;
          pt_node = get_node(recv_pkt.client_response.leaderId);
        } else {
          leaderId = recv_pkt.client_response.leaderId;
        }
      }
    }

    sleep(REQUEST_INTERVAL_SEC);
  }

  close(sock);
  return 0;
}