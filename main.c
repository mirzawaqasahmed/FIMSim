#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include "fpm.h"
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <libnetlink.h>
#include <ifaddrs.h>
#include <assert.h>
#include <string.h>
#include <net/if.h>

#include "fpm.h"
#include "lsp.h"

#define FPM_DEFAULT_PORT 2620
#define BUFFER_SIZE 2000

/*
 * This allows me to access an IP address as a 32 bit inet_addr_t or 4 chars
 * Needed since inet_addr gives me an inet_addr_t and addattr_l needs 4 chars
 */

union ip_add {
  in_addr_t ip_addr;
  char ip_char[4];
};

/*
 * netlink_route_info_encode
 *
 * Returns the number of bytes written to the buffer. 0 or a negative
 * value indicates an error.
 *
 * based on the function in zebra_fpm_netlink.c
 */
int netlink_route_info_encode(char *in_buf, union ip_add gw, union ip_add dst,
                              char * str_mask, int interface_id,
                              size_t in_buf_len) {

  int mask = atoi(str_mask);
  size_t buf_offset;
  //netlink_nh_info_t *nhi;

  struct {
    struct nlmsghdr n;
    struct rtmsg r;
    char buf[1];
  }*req;

  req = (void *) in_buf;

  buf_offset = ((char *) req->buf) - ((char *) req);

  if (in_buf_len < buf_offset) {
    assert(0);
    return 0;
  }

  memset(req, 0, buf_offset);

  req->n.nlmsg_len = NLMSG_LENGTH (sizeof (struct rtmsg));
  req->n.nlmsg_flags = NLM_F_CREATE | NLM_F_REQUEST;
  req->n.nlmsg_type = RTM_NEWROUTE;
  req->r.rtm_family = AF_INET;
  req->r.rtm_table = RT_TABLE_MAIN;
  req->r.rtm_dst_len = mask;
  //printf("mask %i\n", req->r.rtm_dst_len);
  req->r.rtm_protocol = RTPROT_ZEBRA;
  //req->r.rtm_scope = RT_SCOPE_UNIVERSE;

  addattr_l(&req->n, in_buf_len, RTA_DST, dst.ip_char, 4);

  addattr_l(&req->n, in_buf_len, RTA_GATEWAY, gw.ip_char, 4);

  //printf("interface id = %i\n", interface_id);

  addattr32(&req->n, in_buf_len, RTA_OIF, interface_id);
  assert(req->n.nlmsg_len < in_buf_len);
  return req->n.nlmsg_len;
}

void send_netlink_packet(int sockfd, char command[]) {
  char *gateway, *network, *mask, *interface;

  network = strtok(command, " \n");

  gateway = strtok(NULL, " \n");
  interface = strtok(NULL, " \n");
  //printf("network_with_mask is %s \n", network);
  network = strtok(network, "/");
  mask = strtok(NULL, "/");
  //printf("mask is %s \n", mask);
  //printf("network is %s \n", network);
  //printf("gateway is %s \n", gateway);
  // printf("interface is %s\n",interface);

  union ip_add gw;
  gw.ip_addr = inet_addr(gateway);

  union ip_add dst;

  dst.ip_addr = inet_addr(network);

  unsigned char buf[BUFFER_SIZE];

  char* data;

  fpm_msg_hdr_t *hdr;

  hdr = (fpm_msg_hdr_t *) buf;

  hdr->version = FPM_PROTO_VERSION;
  hdr->msg_type = FPM_MSG_TYPE_NETLINK;
  data = fpm_msg_data(hdr);

  int interface_id = if_nametoindex(interface);

  //build a netlink packet to send
  int nl_len = netlink_route_info_encode(data, gw, dst, mask, interface_id,
                                         BUFFER_SIZE);

  int msg_len = fpm_data_len_to_msg_len(nl_len);

  hdr->msg_len = htons(msg_len);
  write(sockfd, buf, msg_len);
}

void sendLSPUpdate(int sockfd, char * command) {
  unsigned char buf[BUFFER_SIZE];

  char* data;
  char* table_operation, *label_operation, *next_hop_ip, *in_label, *out_label;

  table_operation = strtok(command, " \n");
  label_operation = strtok(NULL, " \n");
  next_hop_ip = strtok(NULL, " \n");
  in_label = strtok(NULL, " \n");
  out_label = strtok(NULL, " \n");

  fpm_msg_hdr_t *hdr;

  hdr = (fpm_msg_hdr_t *) buf;

  hdr->version = FPM_PROTO_VERSION;
  hdr->msg_type = FPM_MSG_TYPE_LSP;
  data = fpm_msg_data(hdr);
  lsp_msg_t * lsp_msg = (void *) data;

  lsp_msg->ip_version = IPv4;

  if (strcmp(table_operation, "add") == 0) {
    lsp_msg->table_operation = ADD_LSP;

  } else if (strcmp(table_operation, "remove") == 0) {
    lsp_msg->table_operation = REMOVE_LSP;
  } else {
    printf("Invalid table operation, options are add or remove\n");
    printf("given %s\n", table_operation);
    printf("aborting\n");
    return;
  }

  if (strcmp(label_operation, "push") == 0) {
    lsp_msg->lsp_operation = PUSH;
  } else if (strcmp(label_operation, "pop") == 0) {
    lsp_msg->lsp_operation = POP;
  } else if (strcmp(label_operation, "swap") == 0) {
    lsp_msg->lsp_operation = SWAP;
  } else {
    printf("Invalid label operation, options are push, pop and swap\n");
    printf("given %s\n", label_operation);
    printf("aborting\n");
    return;
  }

  lsp_msg->in_label = htonl(atoi(in_label));
  lsp_msg->out_label = htonl(atoi(out_label));

  lsp_msg->next_hop_ip.ipv4 = inet_addr(next_hop_ip);

  int msg_len = fpm_data_len_to_msg_len(sizeof(lsp_msg_t));

  printf("mesg_len = %i\n", msg_len);

  hdr->msg_len = htons(msg_len);

  write(sockfd, buf, msg_len);
}

int main(int argc, char *argv[]) {

  if (argc < 2 || argc > 2) {
    puts("Usage sim ip-address");
    return EXIT_FAILURE;
  }

  struct sockaddr_in serv_addr;
  int sockfd = 0;
  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    printf("\n Error : Could not create socket \n");
    return 1;
  }

  serv_addr.sin_family = AF_INET;

  serv_addr.sin_port = htons(FPM_DEFAULT_PORT);
  unsigned char buf[100];
  if (inet_pton(AF_INET, argv[1], buf) <= 0) {
    printf("\n inet_pton error occured\n");
    return 1;
  }

  if (connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
    printf("\n Error : Connect Failed \n");
    return 1;
  }
  char input_buf[100];
  printf("type help for help\n");
  while (1) {
    printf(">>");
    gets(input_buf);

    char *command_type, *command;
    command = input_buf;
    command_type = strtok(command, " \n");  //this tell us which type of packet we want to send

    command += strlen(command) + 1;  //strtok breaks up the buffer, I want the command to point to the next section which is the rest of the data (ip addresses etc)

    if (strcmp(command_type, "r") == 0) {
      send_netlink_packet(sockfd, command);
    } else if (strcmp(command_type, "exit") == 0) {
      return EXIT_SUCCESS;

    } else if (strcmp(command_type, "help") == 0) {
      printf("Type \"help-r\" for help on adding a route");
      printf("Type \"help-l\" for help on adding a lsp update");
    } else if (strcmp(command_type, "help-r") == 0) {
      printf("To send route update:\n");
      printf("r network-ip/mask gateway-ip interface\n");
      printf("eg: r 10.0.0.0/8 172.31.1.2 eth1\n");
      printf(
          "That would be network 10.0.0.0/8 can be found via gateway 172.31.1.2 out interface eth0\n");
    } else if (strcmp(command_type, "help-l") == 0) {
      printf("To send lsp update:\n");
      printf(
          "l table_operation lable_operation next_hop_ip in_label out_label\n");
      printf("eg: l add swap 10.0.0.1 123 456\n");
      printf(
          "That would be add a swap operation with next hop 10.0.0.1\nchanging label 123 to label 456\n");
    } else if (strcmp(command_type, "l") == 0) {
      sendLSPUpdate(sockfd, command);
    } else {
      printf("type help for help\n");
    }
  }
}