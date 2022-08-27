#ifndef __RSOCKET_H__
#define __RSOCKET_H__

#include <arpa/inet.h>
#include <sys/types.h>

#define T 2
#define TIMEOUT (2 * T)
#define SOCK_MRP 12
#define DROP_PROBABILITY 0.10f

int r_socket(int family, int type, int protocol);
int r_bind(int sockfd, const struct sockaddr *addr, socklen_t addr_len);
ssize_t r_sendto(int sockfd, const void *buf, size_t nbytes, int flags,
		 const struct sockaddr *to, socklen_t addr_len);
ssize_t r_recvfrom(int sockfd, void *buf, size_t nbytes, int flags,
		   struct sockaddr *from, socklen_t *addr_len);
int r_close(int sockfd);

int dropMessage(float p);

#endif // __RSOCKET_H__
