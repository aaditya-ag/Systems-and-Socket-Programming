#include "rsocket.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NUM_BUCKETS 50
#define RECV_BUF_SIZE 1600
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
typedef void (*free_func)(void *);

static __attribute__((noreturn)) void unreachable(const char *message)
{
	fprintf(stderr, "%s\n", message);
	fflush(stderr);
	exit(1);
}

struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

#define list_entry(p, t, m) ((t *)((char *)(p)-offsetof(t, m)))

#define list_free(p, t, m)                                                     \
	do {                                                                   \
		struct list_head *ptr = (p), *next;                            \
		do {                                                           \
			next = ptr->next;                                      \
			t *ob = list_entry(ptr, t, m);                         \
			if (ob->ff)                                            \
				ob->ff(ob);                                    \
			free(ob);                                              \
			ptr = next;                                            \
		} while (ptr != (p));                                          \
	} while (0);

static void list_init(struct list_head *list_head)
{
	list_head->next = list_head;
	list_head->prev = list_head;
}

static void list_add(struct list_head *prev, struct list_head *el)
{
	el->next = prev->next;
	el->next->prev = el;
	prev->next = el;
	el->prev = prev;
}

static void list_add_tail(struct list_head *next, struct list_head *el)
{
	el->prev = next->prev;
	el->prev->next = el;
	next->prev = el;
	el->next = next;
}

static void list_del(struct list_head *el)
{
	el->prev->next = el->next;
	el->next->prev = el->prev;
}

struct message {
	uint8_t *buf;
	size_t buf_len;
	struct list_head head;
	struct sockaddr_in addr;
	socklen_t addr_len;
	free_func ff;
};

static void free_message(void *ptr)
{
	struct message *mess = ptr;
	free(mess->buf);
}

static void init_message(struct message *mess, const uint8_t *buf,
			 size_t buf_len, const struct sockaddr_in *addr,
			 socklen_t addr_len)
{
	mess->buf = malloc(buf_len);
	memcpy(mess->buf, buf, buf_len);
	mess->buf_len = buf_len;
	mess->ff = free_message;
	list_init(&mess->head);
	memcpy(&mess->addr, addr, sizeof(*addr));
	mess->addr_len = addr_len;
}

struct message_list {
	struct list_head *msgs;
	size_t cnt;
	pthread_mutex_t lock;
};

static void init_message_list(struct message_list *list)
{
	list->msgs = NULL;
	list->cnt = 0;
	pthread_mutex_init(&list->lock, NULL);
}

static void free_message_list(struct message_list *list)
{
	if (list->msgs)
		list_free(list->msgs, struct message, head);
	pthread_mutex_destroy(&list->lock);
}

static size_t message_list_cnt(struct message_list *list)
{
	pthread_mutex_lock(&list->lock);
	size_t cnt = list->cnt;
	pthread_mutex_unlock(&list->lock);
	return cnt;
}

static struct message *message_list_pop_first(struct message_list *list)
{
	pthread_mutex_lock(&list->lock);
	assert(list->msgs);
	struct message *msg = list_entry(list->msgs, struct message, head);
	if (list->cnt == 1) {
		list->msgs = NULL;
	} else {
		struct list_head *next = list->msgs->next;
		list_del(list->msgs);
		list->msgs = next;
	}
	list->cnt--;
	pthread_mutex_unlock(&list->lock);
	return msg;
}

static void message_list_insert(struct message_list *list, struct message *msg)
{
	pthread_mutex_lock(&list->lock);
	if (list->msgs)
		list_add_tail(list->msgs, &msg->head);
	else
		list->msgs = &msg->head;
	list->cnt++;
	pthread_mutex_unlock(&list->lock);
}

// Global to store messages which have been received
// but not yet sent to upper layer.
struct message_list received_message;

struct bucket {
	int entry_cnt;
	struct list_head *entries;
};

struct hashtable {
	struct bucket buckets[NUM_BUCKETS];
	pthread_rwlock_t lock;
};

static void init_bucket(struct bucket *bucket)
{
	bucket->entries = NULL;
	bucket->entry_cnt = 0;
}

static void init_hashtable(struct hashtable *tbl)
{
	for (int i = 0; i < NUM_BUCKETS; i++)
		init_bucket(&tbl->buckets[i]);
	pthread_rwlock_init(&tbl->lock, NULL);
}

#define free_hashtable(tbl, type, name)                                        \
	do {                                                                   \
		for (int i = 0; i < NUM_BUCKETS; i++) {                        \
			struct bucket *bkt = &(tbl)->buckets[i];               \
			if (!bkt->entries)                                     \
				continue;                                      \
			list_free(bkt->entries, type, name);                   \
		}                                                              \
		pthread_rwlock_destroy(&(tbl)->lock);                          \
	} while (0);

struct unack_mess {
	uint32_t seq_no;
	uint8_t *buf;
	size_t buf_len;
	time_t send_time;
	struct list_head head;
	struct sockaddr_in addr;
	socklen_t addr_len;
	free_func ff;
};

static void free_unack_mess(void *ptr)
{
	struct unack_mess *mess = ptr;
	free(mess->buf);
}

static void init_unack_mess(struct unack_mess *mess, uint32_t seq_no,
			    const uint8_t *buf, size_t buf_len,
			    const struct sockaddr_in *addr, socklen_t addr_len)
{
	mess->seq_no = seq_no;
	mess->buf = malloc(buf_len);
	memcpy(mess->buf, buf, buf_len);
	mess->buf_len = buf_len;
	mess->send_time = time(NULL);
	list_init(&mess->head);
	memcpy(&mess->addr, addr, sizeof(*addr));
	mess->addr_len = addr_len;
	mess->ff = free_unack_mess;
}

// Global to hold unacknowledged messages
struct hashtable unacknowledged_messages;

void hashtable_insert_message(struct hashtable *table, struct unack_mess *mess)
{
	int idx = mess->seq_no % NUM_BUCKETS;
	pthread_rwlock_wrlock(&table->lock);
	struct bucket *bkt = &table->buckets[idx];
	if (bkt->entries)
		list_add(bkt->entries, &mess->head);
	else
		bkt->entries = &mess->head;
	bkt->entry_cnt++;
	pthread_rwlock_unlock(&table->lock);
}

void hashtable_delete_message(struct hashtable *table, uint32_t seq_no)
{
	int idx = seq_no % NUM_BUCKETS;
	pthread_rwlock_wrlock(&table->lock);
	struct bucket *bkt = &table->buckets[idx];
	if (bkt->entry_cnt == 0)
		goto del_mess_exit;
	struct list_head *head, *ptr;
	head = ptr = bkt->entries;
	do {
		struct unack_mess *msg =
		    list_entry(ptr, struct unack_mess, head);
		if (msg->seq_no == seq_no) {
			if (bkt->entry_cnt == 1)
				bkt->entries = NULL;
			else
				list_del(ptr);
			free_unack_mess(msg);
			free(msg);
			bkt->entry_cnt--;
			break;
		}
		ptr = ptr->next;
	} while (ptr != head);
del_mess_exit:
	pthread_rwlock_unlock(&table->lock);
}

int curr_fd = -1;

enum __attribute__((packed)) message_type {
	MT_Data,
	MT_Ack,
};

// Thread R
static void *receiver_thread(__attribute__((unused)) void *data)
{
	static uint8_t buf[RECV_BUF_SIZE];
	const size_t min_mess_size = 4 + sizeof(enum message_type);
	for (;;) {
		struct sockaddr_in addr;
		socklen_t addr_len = sizeof(addr);
		ssize_t ret = recvfrom(curr_fd, buf, sizeof(buf), 0,
				       (struct sockaddr *)&addr, &addr_len);

		if (ret == -1)
			continue;
		// Message must contain at least seqence number and type
		// Drop packet if not satisfied
		if (ret < (ssize_t)min_mess_size)
			continue;
		// Fake unreliability
		if (dropMessage(DROP_PROBABILITY))
			continue;

		uint32_t seq_no;
		enum message_type type;
		memcpy(&seq_no, buf, 4);
		memcpy(&type, buf + 4, sizeof(type));

		if (type == MT_Data) {
			// Received data packet
			// printf("Received DATA %d\n", seq_no);
			struct message *msg = malloc(sizeof(*msg));
			init_message(msg, buf + min_mess_size,
				     ret - min_mess_size, &addr, addr_len);
			message_list_insert(&received_message, msg);
			// Send out ACK
			memcpy(buf, &seq_no, 4);
			type = MT_Ack;
			memcpy(buf + 4, &type, sizeof(type));

			// printf("sockfd = %d\n", curr_fd);
			// printf("Send addr: %s:%d, Addr len: %u\n",
			//        inet_ntoa(addr.sin_addr),
			//        ntohs(addr.sin_port), addr_len);
			int ret =
			    sendto(curr_fd, buf, min_mess_size, 0,
				   (const struct sockaddr *)&addr, addr_len);
			// printf("Sent ACK %d\n", seq_no);
			if (ret == -1) {
				perror("sendto failed in ack");
				exit(1);
			}
		} else if (type == MT_Ack) {
			// Recevied ack packet
			hashtable_delete_message(&unacknowledged_messages,
						 seq_no);
			// printf("Received ACK %d\n", seq_no);
		}
	}
	unreachable("Receiver thread shouldn't exit");
}

static ssize_t send_message(uint32_t seq_num, const uint8_t *data, size_t cnt,
			    int sockfd, int flags, const struct sockaddr *from,
			    socklen_t addrlen)
{
	const enum message_type type = MT_Data;

	size_t req_len = cnt + 4 + sizeof(enum message_type);
	uint8_t *mess_buf = malloc(req_len);
	memcpy(mess_buf, &seq_num, 4);
	memcpy(mess_buf + 4, &type, sizeof(type));
	memcpy(mess_buf + 4 + sizeof(type), data, cnt);

	// printf("sockfd = %d\n", sockfd);
	ssize_t ret = sendto(sockfd, mess_buf, req_len, flags, from, addrlen);
	// printf("Send addr: %s:%d, Addr len: %u\n",
	//    inet_ntoa(((const struct sockaddr_in *)from)->sin_addr),
	//    ntohs(((const struct sockaddr_in *)from)->sin_port), addrlen);
	if (ret == -1) {
		perror("Failed to sendto while resend");
		exit(1);
	}
	free(mess_buf);
	return ret;
}

static void scan_table_for_resend(struct hashtable *tbl)
{
	pthread_rwlock_rdlock(&tbl->lock);
	for (int idx = 0; idx < NUM_BUCKETS; idx++) {
		struct list_head *head = tbl->buckets[idx].entries;
		if (!head)
			continue;
		struct list_head *ptr = head;
		do {
			struct unack_mess *msg =
			    list_entry(ptr, struct unack_mess, head);
			time_t now = time(NULL);
			if (now - msg->send_time >= TIMEOUT) {
				send_message(
				    msg->seq_no, msg->buf, msg->buf_len,
				    curr_fd, 0,
				    (const struct sockaddr *)&msg->addr,
				    msg->addr_len);
				// printf("Resent DATA %d\n", msg->seq_no);

				msg->send_time = now;
			}
			ptr = ptr->next;
		} while (ptr != head);
	}
	pthread_rwlock_unlock(&tbl->lock);
}

// Thread S
static void *resender_thread(__attribute__((unused)) void *data)
{
	for (;;) {
		sleep(T);
		scan_table_for_resend(&unacknowledged_messages);
	}
	unreachable("resender loop should never exit");
}

pthread_t rcv_tid;
pthread_t snd_tid;

int r_socket(int family, int type, int protocol)
{
	if (type != SOCK_MRP) {
		errno = EINVAL;
		return -1;
	}
	int fd = socket(family, SOCK_DGRAM, protocol);
	if (fd == -1)
		return -1;

	// Setup threads and data structures
	init_message_list(&received_message);
	init_hashtable(&unacknowledged_messages);
	curr_fd = fd;
	pthread_create(&rcv_tid, NULL, receiver_thread, NULL);
	pthread_create(&snd_tid, NULL, resender_thread, NULL);

	// Seed rand for dropMessage
	srand(time(NULL));
	return fd;
}

int r_bind(int sockfd, const struct sockaddr *addr, socklen_t addr_len)
{
	return bind(sockfd, addr, addr_len);
}

int r_close(int sockfd)
{
	pthread_cancel(rcv_tid);
	pthread_cancel(snd_tid);
	free_message_list(&received_message);
	free_hashtable(&unacknowledged_messages, struct unack_mess, head);
	return close(sockfd);
}

ssize_t r_sendto(int sockfd, const void *buff, size_t nbytes, int flags,
		 const struct sockaddr *to, socklen_t addrlen)
{
	static uint32_t seq_num = 0;

	ssize_t ret =
	    send_message(seq_num, buff, nbytes, sockfd, flags, to, addrlen);
	if (ret == -1)
		return -1;
	struct unack_mess *mess = malloc(sizeof(*mess));
	init_unack_mess(mess, seq_num, buff, nbytes,
			(const struct sockaddr_in *)to, addrlen);
	hashtable_insert_message(&unacknowledged_messages, mess);
	seq_num++;

	return ret;
}

ssize_t r_recvfrom(__attribute__((unused)) int sockfd, void *buf, size_t nbytes,
		   __attribute__((unused)) int flags, struct sockaddr *from,
		   socklen_t *addr_len)
{
	const int sleep_time = 1;
	while (message_list_cnt(&received_message) == 0)
		sleep(sleep_time);
	struct message *msg = message_list_pop_first(&received_message);
	ssize_t len = (size_t)MIN(nbytes, msg->buf_len);
	memcpy(buf, msg->buf, len);

	*from = *(struct sockaddr *)&msg->addr;
	*addr_len = msg->addr_len;

	free_message(msg);
	free(msg);

	return len;
}

int dropMessage(float p)
{
	double rnd = (double)rand() / (double)RAND_MAX;
	return (rnd < p);
}
