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

#define MY_PORT 50031
#define OTHER_PORT 50030

int sockfd = -1;

void close_socket()
{
	if (sockfd >= 0)
		r_close(sockfd);
	fprintf(stderr, "Closed in user2\n");
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

	char ch;

	socklen_t addrlen = sizeof(other_addr);
	printf("Waiting for string from user 1\n");
	int idx = 0;
	while (r_recvfrom(sockfd, &ch, 1, 0, (struct sockaddr *)&other_addr,
			  &addrlen) >= 0) {
		printf("Received char %d = %c\n", idx, ch);
		fflush(stdout);
		idx++;
	}
	r_close(sockfd);
	return 0;
}
