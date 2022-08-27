#include "rsocket.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define MY_PORT 50030
#define OTHER_PORT 50031

int sockfd = -1;

void close_socket()
{
	if (sockfd >= 0)
		r_close(sockfd);
	fprintf(stderr, "Closed socket in user1\n");
	exit(0);
}

int main()
{
	signal(SIGTERM, close_socket);
	signal(SIGINT, close_socket);

	struct sockaddr_in my_addr, other_addr;

	if ((sockfd = r_socket(AF_INET, SOCK_MRP, 0)) < 0) {
		printf("Unable to create socket\n");
		exit(0);
	}

	my_addr.sin_family = AF_INET;
	inet_pton(AF_INET, "127.0.0.1", &my_addr.sin_addr);
	my_addr.sin_port = htons(MY_PORT);

	if ((r_bind(sockfd, (struct sockaddr *)&my_addr, sizeof(my_addr))) <
	    0) {
		printf("Couldn't bind to port1\n");
		exit(0);
	}

	other_addr.sin_family = AF_INET;
	inet_pton(AF_INET, "127.0.0.1", &other_addr.sin_addr);
	other_addr.sin_port = htons(OTHER_PORT);

	char msg[51];
	memset(msg, 0, sizeof(msg));
	do {
		printf("Enter string of size min 25 and max 50:\n");
		scanf("%s", msg);
	} while (strlen(msg) < 25);
	size_t msg_len = strlen(msg);

	socklen_t addrlen = sizeof(other_addr);
	for (size_t i = 0; i < msg_len; i++) {
		r_sendto(sockfd, msg + i, 1, 0, (struct sockaddr *)&other_addr,
			 addrlen);
	}

	for (;;)
		sleep(10);
	return 0;
}

// ''.join(list(map(chr, range(ord('a'), ord('z') + 1))))
