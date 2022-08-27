#define _GNU_SOURCE

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

__attribute__((noreturn)) void unreachable(const char *message) {
    fprintf(stderr, "%s\n", message);
    fflush(stderr);
    exit(1);
}

// Job definition

enum job_status {
    ST_D000 = 0,
    ST_D001 = 1,
    ST_D010 = 2,
    ST_D011 = 3,
    ST_D100 = 4,
    ST_D101 = 5,
    ST_D110 = 6,
    ST_D111 = 7,
    ST_Complete = 8,
};

enum block_status {
    BS_Assign,
    BS_Add,
    BS_Done,
};

char *job_status_to_str(enum job_status status);

#define MAT_SIZE 1000
#define MAT_EL_MIN -9
#define MAT_EL_MAX 9
#define MAT_ID_MIN 1
#define MAT_ID_MAX 100000

struct job {
    int prod_num;
    int mat_id;
    enum job_status status;
    enum block_status blocks[4];
    long matrix[MAT_SIZE][MAT_SIZE];
};

void job_init(struct job *job, int prod_num, int mat_id);
void job_print(struct job *job);

// Queue

#define MAX_INSERT_JOBS 7
#define QUEUE_SIZE (MAX_INSERT_JOBS + 1)

struct queue {
    struct job jobs[QUEUE_SIZE];
    int cnt;
    pthread_mutex_t lock;
};

struct shared_mem;

void queue_init(struct queue *queue);
bool queue_push(struct queue *queue, struct job *job, struct shared_mem *mem);
bool queue_pop(struct queue *queue);
void queue_rotate_right(struct queue *queue, int k);
int queue_cnt(struct queue *queue);
void queue_incr_cnt(struct queue *queue);

// Shared memory

int max_job_created;

struct shared_mem {
    int job_created;
    struct queue queue;
    pthread_mutex_t cntr_lock;
};

struct shared_mem *shared_mem_attach(int shmid);
void shared_mem_release(struct shared_mem *mem);
void shared_mem_init(struct shared_mem *mem);

void producer(int prod_id, int shmid);
void consumer(int cons_id, int shmid);

int gen_seed();
long trace(long *matrix);
void adjust_stack_size();
void print_time_taken(time_t tot_time);

int main(int argc, char *argv[]) {
    int shmid, np, nw;
    struct shared_mem *main_mem;
    pid_t cpid, *child_pids;
    time_t start_time, end_time;

    if (argc != 4) {
        fprintf(stderr, "Usage: <np> <nw> <num-matrices>\n");
        return 1;
    }
    np = atoi(argv[1]);
    nw = atoi(argv[2]);
    max_job_created = atoi(argv[3]);
    if (np <= 0 || nw <= 0) {
        fprintf(stderr, "np and nw must be positive numbers\n");
        return 1;
    }
    if (max_job_created < 2) {
        fprintf(stderr, "Expected at least 2 matrices to multiply\n");
        return 1;
    }

    adjust_stack_size();

    child_pids = calloc(nw, sizeof(*child_pids));

    // Generate the shared memory segment
    if ((shmid = shmget(IPC_PRIVATE, sizeof(struct shared_mem),
                        IPC_CREAT | IPC_EXCL | 0644)) < 0) {
        perror("Failed to shmget");
        return 1;
    }
    main_mem = shared_mem_attach(shmid);
    shared_mem_init(main_mem);

    for (int i = 0; i < np; i++) {
        cpid = fork();
        if (cpid == 0) {
            srand(gen_seed());
            producer(i + 1, shmid);
        }
        if (cpid == -1) {
            perror("Fork failed");
            exit(1);
        }
    }

    for (int i = 0; i < nw; i++) {
        cpid = fork();
        if (cpid == 0) {
            srand(gen_seed());
            consumer(i + 1, shmid);
        }
        if (cpid == -1) {
            perror("Fork failed");
            exit(1);
        }
        child_pids[i] = cpid;
    }

    start_time = time(NULL);
    for (;;) {
        pthread_mutex_lock(&main_mem->cntr_lock);
        if (main_mem->job_created == max_job_created) {
            pthread_mutex_unlock(&main_mem->cntr_lock);
            if (queue_cnt(&main_mem->queue) == 1) break;
        }
        pthread_mutex_unlock(&main_mem->cntr_lock);
        sleep(1);
    }
    end_time = time(NULL);

    for (int i = 0; i < nw; i++) {
        kill(child_pids[i], SIGTERM);
    }

    printf("\nSum along principal diagonal of result = %ld\n",
           trace((long *)main_mem->queue.jobs[0].matrix));
    print_time_taken(end_time - start_time);

    shared_mem_release(main_mem);
    free(child_pids);
    return 0;
}

void print_time_taken(time_t tot_time) {
    long min, secs;
    min = tot_time / 60L;
    secs = tot_time % 60L;
    printf("Time taken: ");
    if (min > 0) printf("%ld min", min);
    if (min > 0 && secs > 0) printf(", ");
    if (secs > 0) printf("%ld sec", secs);
    printf("\n");
}

long trace(long *matrix) {
    long sum = 0;
    for (int i = 0; i < MAT_SIZE; i++) {
        for (int j = 0; j < MAT_SIZE; j++) {
            sum += matrix[i * MAT_SIZE + j];
        }
    }
    return sum;
}

int rand_range(int min, int max) { return rand() % (max - min + 1) + min; }

int gen_seed() {
    int fd, res;
    uint8_t buf[4];

    fd = open("/dev/random", O_RDONLY);
    read(fd, buf, sizeof(buf));
    close(fd);
    memcpy(&res, buf, sizeof(buf));
    return res;
}

void producer(int prod_id, int shmid) {
    struct shared_mem *mem;
    struct job job;
    int mat_id, wait_sec;

    mem = shared_mem_attach(shmid);

    for (;;) {
        mat_id = rand_range(MAT_ID_MIN, MAT_ID_MAX);
        job_init(&job, prod_id, mat_id);

        wait_sec = rand() % 3;
        sleep(wait_sec);

        if (queue_push(&mem->queue, &job, mem)) {
            printf("Produced: ");
            job_print(&job);
        }
        pthread_mutex_lock(&mem->cntr_lock);
        if (mem->job_created == max_job_created) {
            pthread_mutex_unlock(&mem->cntr_lock);
            break;
        }
        pthread_mutex_unlock(&mem->cntr_lock);
    }

    shared_mem_release(mem);
    exit(0);
}

void copy_block(long *dest, long *src, int i, int j) {
    int dest_size = MAT_SIZE / 2, src_size = MAT_SIZE;
    int row_start, row_end, col_start, col_end, idx;

    row_start = (i == 0 ? 0 : dest_size);
    row_end = (i == 0 ? dest_size : src_size);
    col_start = (j == 0 ? 0 : dest_size);
    col_end = (j == 0 ? dest_size : src_size);

    idx = 0;
    for (int row = row_start; row < row_end; row++) {
        for (int col = col_start; col < col_end; col++) {
            dest[idx] = src[row * src_size + col];
            idx++;
        }
    }
}

void copy_back_block(long *dest, long *src, int i, int j) {
    int dest_size = MAT_SIZE, src_size = MAT_SIZE / 2;
    int row_start, row_end, col_start, col_end, idx;

    row_start = (i == 0 ? 0 : src_size);
    row_end = (i == 0 ? src_size : dest_size);
    col_start = (j == 0 ? 0 : src_size);
    col_end = (j == 0 ? src_size : dest_size);

    idx = 0;
    for (int row = row_start; row < row_end; row++) {
        for (int col = col_start; col < col_end; col++) {
            dest[row * dest_size + col] = src[idx];
            idx++;
        }
    }
}

void add_back_block(long *dest, long *src, int i, int j) {
    int dest_size = MAT_SIZE, src_size = MAT_SIZE / 2;
    int row_start, row_end, col_start, col_end, idx;

    row_start = (i == 0 ? 0 : src_size);
    row_end = (i == 0 ? src_size : dest_size);
    col_start = (j == 0 ? 0 : src_size);
    col_end = (j == 0 ? src_size : dest_size);

    idx = 0;
    for (int row = row_start; row < row_end; row++) {
        for (int col = col_start; col < col_end; col++) {
            dest[row * dest_size + col] += src[idx];
            idx++;
        }
    }
}

struct block_id {
    int i;
    int j;
    int k;
};

struct block_id *job_status_to_block_id(enum job_status status) {
    static struct block_id id;
    switch (status) {
        case ST_D000:
            id = (struct block_id){0, 0, 0};
            break;
        case ST_D001:
            id = (struct block_id){0, 0, 1};
            break;
        case ST_D010:
            id = (struct block_id){0, 1, 0};
            break;
        case ST_D011:
            id = (struct block_id){0, 1, 1};
            break;
        case ST_D100:
            id = (struct block_id){1, 0, 0};
            break;
        case ST_D101:
            id = (struct block_id){1, 0, 1};
            break;
        case ST_D110:
            id = (struct block_id){1, 1, 0};
            break;
        case ST_D111:
            id = (struct block_id){1, 1, 1};
            break;
        case ST_Complete:
            id = (struct block_id){-1, -1, -1};
            break;
    }
    return &id;
}

void block_multiply(long *result, long *left, long *right) {
    int size = MAT_SIZE / 2;
    long sum;

    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            sum = 0;
            for (int k = 0; k < size; k++) {
                sum += left[i * size + k] * right[k * size + j];
            }
            result[i * size + j] = sum;
        }
    }
}

void consumer_log(int cons_id, int prod_id, int mat_id, int i, int j,
                  const char *mess) {
    printf("In Consumer %d: ", cons_id);
    if (prod_id > 0)
        printf("Producer id = %d, ", prod_id);
    else
        printf("Producer id = %d (consumer), ", -prod_id);
    printf("Matrix id = %d, ", mat_id);
    printf("%s block (%d, %d)\n", mess, i, j);
}

void consumer(int cons_id, int shmid) {
    struct shared_mem *mem;
    struct job *left_job, *right_job, *result_job;
    struct block_id *id;
    enum job_status status, new_status;
    long left_block[MAT_SIZE / 2][MAT_SIZE / 2];
    long right_block[MAT_SIZE / 2][MAT_SIZE / 2];
    long result_block[MAT_SIZE / 2][MAT_SIZE / 2];
    int block_id;
    enum block_status curr_block_status;

    static const enum block_status all_blocks_done[] = {BS_Done, BS_Done,
                                                        BS_Done, BS_Done};

    mem = shared_mem_attach(shmid);

    for (;;) {
        while (queue_cnt(&mem->queue) < 2) {
            sleep(1);
        }

        // Lock once to copy the data
        pthread_mutex_lock(&mem->queue.lock);
        left_job = mem->queue.jobs;
        right_job = mem->queue.jobs + 1;
        result_job = mem->queue.jobs + MAX_INSERT_JOBS;

        assert(left_job->status == right_job->status);
        status = left_job->status;
        new_status = status + 1;

        if (status == ST_Complete) {
            pthread_mutex_unlock(&mem->queue.lock);
            sleep(1);
            continue;
        }

        id = job_status_to_block_id(status);
        copy_block((long *)left_block, (long *)left_job->matrix, id->i, id->k);
        consumer_log(cons_id, left_job->prod_num, left_job->mat_id, id->i,
                     id->k, "Reading");
        copy_block((long *)right_block, (long *)right_job->matrix, id->k,
                   id->j);
        consumer_log(cons_id, right_job->prod_num, right_job->mat_id, id->k,
                     id->j, "Reading");

        left_job->status = right_job->status = new_status;

        result_job->prod_num = -cons_id;
        result_job->mat_id = rand_range(MAT_ID_MIN, MAT_ID_MAX);

        pthread_mutex_unlock(&mem->queue.lock);

        // Do the computation (super long)
        block_multiply((long *)result_block, (long *)left_block,
                       (long *)right_block);

        // Lock again to push in the data
        pthread_mutex_lock(&mem->queue.lock);

        block_id = 2 * id->i + id->j;
        curr_block_status = result_job->blocks[block_id];
        result_job->blocks[block_id]++;

        if (curr_block_status == BS_Assign) {
            copy_back_block((long *)result_job->matrix, (long *)result_block,
                            id->i, id->j);
            consumer_log(cons_id, result_job->prod_num, result_job->mat_id,
                         id->i, id->j, "Copying");
        } else if (curr_block_status == BS_Add) {
            add_back_block((long *)result_job->matrix, (long *)result_block,
                           id->i, id->j);
            consumer_log(cons_id, result_job->prod_num, result_job->mat_id,
                         id->i, id->j, "Adding");
        } else {
            unreachable("curr_block_status cannot be BS_Done");
        }

        // Check if all blocks are done
        // Then pop top 2, rotate right once, adjust cnt
        if (memcmp(result_job->blocks, all_blocks_done,
                   sizeof(all_blocks_done)) == 0) {
            queue_pop(&mem->queue);
            queue_pop(&mem->queue);
            queue_rotate_right(&mem->queue, 1);
            memset(result_job, 0, sizeof(*result_job));
            queue_incr_cnt(&mem->queue);
        }
        pthread_mutex_unlock(&mem->queue.lock);
    }

    shared_mem_release(mem);
    exit(0);
}

void job_init(struct job *job, int prod_num, int mat_id) {
    job->prod_num = prod_num;
    job->mat_id = mat_id;
    job->status = ST_D000;
    memset(job->blocks, 0, sizeof(job->blocks));

    for (int i = 0; i < MAT_SIZE; i++) {
        for (int j = 0; j < MAT_SIZE; j++) {
            job->matrix[i][j] = rand_range(MAT_EL_MIN, MAT_EL_MAX);
        }
    }
}

void queue_init(struct queue *queue) {
    pthread_mutexattr_t attr;

    memset(queue, 0, sizeof(*queue));

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, 1);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&queue->lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

bool queue_push(struct queue *queue, struct job *job, struct shared_mem *mem) {
    bool retval;

    pthread_mutex_lock(&queue->lock);
    if (queue->cnt == MAX_INSERT_JOBS) {
        retval = false;
        goto queue_push_ret;
    }

    pthread_mutex_lock(&mem->cntr_lock);
    if (mem->job_created == max_job_created) {
        retval = false;
        goto queue_push_ret;
    } else {
        mem->job_created++;
        retval = true;
        memcpy(queue->jobs + queue->cnt, job, sizeof(*job));
        queue->cnt++;
    }

queue_push_ret:
    pthread_mutex_unlock(&mem->cntr_lock);
    pthread_mutex_unlock(&queue->lock);
    return retval;
}

bool queue_pop(struct queue *queue) {
    bool retval;

    pthread_mutex_lock(&queue->lock);
    if (queue->cnt == 0) {
        retval = false;
        goto queue_pop_ret;
    }

    retval = true;
    for (int i = 0; i < MAX_INSERT_JOBS; i++) {
        queue->jobs[i] = queue->jobs[i + 1];
    }
    queue->cnt--;

queue_pop_ret:
    pthread_mutex_unlock(&queue->lock);
    return retval;
}

void queue_reverse(struct queue *queue, int start, int stop) {
    struct job tmp;
    int size = stop - start;
    for (int i = start; i < start + size / 2; i++) {
        tmp = queue->jobs[i];
        queue->jobs[i] = queue->jobs[stop - i - 1];
        queue->jobs[stop - i - 1] = tmp;
    }
}

void queue_rotate_right(struct queue *queue, int k) {
    pthread_mutex_lock(&queue->lock);

    k = k % QUEUE_SIZE;
    queue_reverse(queue, 0, QUEUE_SIZE - k);
    queue_reverse(queue, QUEUE_SIZE - k, QUEUE_SIZE);
    queue_reverse(queue, 0, QUEUE_SIZE);

    pthread_mutex_unlock(&queue->lock);
}

int queue_cnt(struct queue *queue) {
    pthread_mutex_lock(&queue->lock);
    int cnt = queue->cnt;
    pthread_mutex_unlock(&queue->lock);
    return cnt;
}

void queue_incr_cnt(struct queue *queue) {
    pthread_mutex_lock(&queue->lock);
    queue->cnt++;
    pthread_mutex_unlock(&queue->lock);
}

struct shared_mem *shared_mem_attach(int shmid) {
    void *ptr;

    if ((ptr = shmat(shmid, NULL, 0)) == (void *)(-1)) {
        perror("Failed to shmat");
        exit(1);
    }

    return ptr;
}

void shared_mem_init(struct shared_mem *mem) {
    pthread_mutexattr_t attr;

    mem->job_created = 0;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, 1);
    pthread_mutex_init(&mem->cntr_lock, &attr);
    pthread_mutexattr_destroy(&attr);

    queue_init(&mem->queue);
}

void shared_mem_release(struct shared_mem *mem) {
    if (shmdt(mem) != 0) {
        perror("Failed to shmdt");
        exit(1);
    }
}

void job_print(struct job *job) {
    printf("Job { ");
    printf("Producer id = %d, ", job->prod_num);
    printf("Matrix id = %d, ", job->mat_id);
    printf("Status = %s", job_status_to_str(job->status));
    printf(" }\n");
    fflush(stdout);
}

char *job_status_to_str(enum job_status status) {
    static char str[20];
    switch (status) {
        case ST_D000:
            sprintf(str, "D000");
            break;
        case ST_D001:
            sprintf(str, "D001");
            break;
        case ST_D010:
            sprintf(str, "D010");
            break;
        case ST_D011:
            sprintf(str, "D011");
            break;
        case ST_D100:
            sprintf(str, "D100");
            break;
        case ST_D101:
            sprintf(str, "D101");
            break;
        case ST_D110:
            sprintf(str, "D110");
            break;
        case ST_D111:
            sprintf(str, "D111");
            break;
        case ST_Complete:
            sprintf(str, "Complete");
            break;
    }
    return str;
}

void adjust_stack_size() {
    const rlim_t required_size = 2 * sizeof(struct job);
    struct rlimit rl;

    if (getrlimit(RLIMIT_STACK, &rl) != 0) {
        perror("Failed to getrlimit");
        exit(1);
    }

    if (rl.rlim_cur < required_size) {
        rl.rlim_cur = required_size;
        if (setrlimit(RLIMIT_STACK, &rl) != 0) {
            perror("Failed to setrlimit");
            exit(1);
        }
    }
}
