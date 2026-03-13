#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <aio.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

static void clr() {
    printf("\033[2J\033[H");
    fflush(stdout);
}

static void pause_enter() {
    printf("\nPress Enter to continue...");
    fflush(stdout);
    int c;
    do { c = getchar(); } while (c != '\n' && c != EOF);
}

static void print_errno(const char* where) {
    fprintf(stderr, "%s failed: %s\n", where, strerror(errno));
    fflush(stderr);
}

static void read_line(const char* prompt, char* out, size_t out_sz) {
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(out, (int)out_sz, stdin)) {
        out[0] = 0;
        return;
    }
    out[strcspn(out, "\n")] = 0;
}

static inline int64_t atomic_add64(volatile int64_t* p, int64_t v) {
    return __sync_add_and_fetch(p, v);
}

static inline int64_t atomic_load64(volatile int64_t* p) {
    return __sync_add_and_fetch(p, 0);
}

struct aio_operation {
    struct aiocb aio;
    char *buffer;
    int write_operation;   // 0 = read, 1 = write
    void* next_operation;  // read -> write
    int state;             // 0 idle, 1 read pending, 3 write pending, 5 buffer busy
    ssize_t result;
};

static int submit_read(struct aio_operation* read_op, struct aio_operation* write_op, off_t off);
static int submit_write(struct aio_operation* write_op, char* buf, size_t bytes, off_t off);
static void aio_completion_handler(sigval_t sigval);

static int g_fd_in  = -1;
static int g_fd_out = -1;

static off_t  g_file_size = 0;
static size_t g_block_size = 4096;
static int    g_inflight = 2;

static volatile sig_atomic_t g_done = 0;
static volatile sig_atomic_t g_error = 0;
static volatile int64_t g_pending64 = 0;
static volatile int64_t g_total_written64 = 0;

static off_t g_next_offset = 0;

static char g_src[1024] = {0};
static char g_dst[1024] = {0};

static struct aio_operation* g_ops = NULL;
static int g_ops_count = 0;

static void close_all() {
    if (g_fd_in != -1)  { close(g_fd_in);  g_fd_in  = -1; }
    if (g_fd_out != -1) { close(g_fd_out); g_fd_out = -1; }
}

static void free_ops() {
    if (!g_ops) return;
    for (int i = 0; i < g_ops_count; ++i) {
        if (g_ops[i].buffer) {
            free(g_ops[i].buffer);
            g_ops[i].buffer = NULL;
        }
    }
    free(g_ops);
    g_ops = NULL;
    g_ops_count = 0;
}

static int all_slots_idle() {
    for (int i = 0; i < g_ops_count; ++i) {
        if (g_ops[i].state != 0) return 0;
    }
    return 1;
}

static void aio_completion_handler(sigval_t sigval) {
    struct aio_operation *aio_op = (struct aio_operation *)sigval.sival_ptr;
    if (!aio_op) return;

    int err = aio_error(&aio_op->aio);
    if (err == EINPROGRESS) return;

    ssize_t ret = aio_return(&aio_op->aio);
    atomic_add64(&g_pending64, -1);
    aio_op->result = ret;

    if (err != 0 || ret < 0) {
        aio_op->state = 0;

        if (aio_op->write_operation) {
            for (int i = 0; i < g_inflight; ++i) {
                struct aio_operation* r = &g_ops[i*2 + 0];
                struct aio_operation* w = &g_ops[i*2 + 1];
                if (w == aio_op) {
                    r->state = 0;
                    break;
                }
            }
        }

        g_error = 1;
        return;
    }

    if (aio_op->write_operation) {
        size_t asked = (size_t)aio_op->aio.aio_nbytes;

        if ((size_t)ret < asked) {
            char* next_buf = ((char*)aio_op->aio.aio_buf) + ret;
            size_t left = asked - (size_t)ret;
            off_t next_off = aio_op->aio.aio_offset + ret;

            aio_op->state = 0;
            if (submit_write(aio_op, next_buf, left, next_off) < 0) {
                for (int i = 0; i < g_inflight; ++i) {
                    struct aio_operation* r = &g_ops[i*2 + 0];
                    struct aio_operation* w = &g_ops[i*2 + 1];
                    if (w == aio_op) {
                        r->state = 0;
                        break;
                    }
                }
                g_error = 1;
            }
            return;
        }

        atomic_add64(&g_total_written64, ret);

        aio_op->state = 0;
        for (int i = 0; i < g_inflight; ++i) {
            struct aio_operation* r = &g_ops[i*2 + 0];
            struct aio_operation* w = &g_ops[i*2 + 1];
            if (w == aio_op) {
                r->state = 0;
                break;
            }
        }

        if (atomic_load64(&g_total_written64) >= (int64_t)g_file_size) {
            g_done = 1;
        }
    } else {
        if (ret == 0) {
            aio_op->state = 0;
            return;
        }

        struct aio_operation* write_op = (struct aio_operation*)aio_op->next_operation;
        if (!write_op) {
            aio_op->state = 0;
            g_error = 1;
            return;
        }

        if (submit_write(write_op, aio_op->buffer, (size_t)ret, aio_op->aio.aio_offset) < 0) {
            aio_op->state = 0;
            g_error = 1;
            return;
        }

        aio_op->state = 5;
    }
}

static int submit_read(struct aio_operation* read_op, struct aio_operation* write_op, off_t off) {
    if (off >= g_file_size) return 0;

    size_t need = g_block_size;
    if (off + (off_t)need > g_file_size) {
        need = (size_t)(g_file_size - off);
    }

    memset(&read_op->aio, 0, sizeof(read_op->aio));
    read_op->write_operation = 0;
    read_op->next_operation = (void*)write_op;
    read_op->state = 1;
    read_op->result = 0;

    read_op->aio.aio_fildes = g_fd_in;
    read_op->aio.aio_buf = read_op->buffer;
    read_op->aio.aio_nbytes = need;
    read_op->aio.aio_offset = off;
    read_op->aio.aio_sigevent.sigev_notify = SIGEV_THREAD;
    read_op->aio.aio_sigevent.sigev_notify_function = aio_completion_handler;
    read_op->aio.aio_sigevent.sigev_value.sival_ptr = read_op;
    read_op->aio.aio_sigevent.sigev_notify_attributes = NULL;

    if (aio_read(&read_op->aio) != 0) {
        read_op->state = 0;
        print_errno("aio_read");
        return -1;
    }

    atomic_add64(&g_pending64, 1);
    return 1;
}

static int submit_write(struct aio_operation* write_op, char* buf, size_t bytes, off_t off) {
    memset(&write_op->aio, 0, sizeof(write_op->aio));
    write_op->write_operation = 1;
    write_op->state = 3;
    write_op->result = 0;

    write_op->aio.aio_fildes = g_fd_out;
    write_op->aio.aio_buf = buf;
    write_op->aio.aio_nbytes = bytes;
    write_op->aio.aio_offset = off;
    write_op->aio.aio_sigevent.sigev_notify = SIGEV_THREAD;
    write_op->aio.aio_sigevent.sigev_notify_function = aio_completion_handler;
    write_op->aio.aio_sigevent.sigev_value.sival_ptr = write_op;
    write_op->aio.aio_sigevent.sigev_notify_attributes = NULL;

    if (aio_write(&write_op->aio) != 0) {
        write_op->state = 0;
        print_errno("aio_write");
        return -1;
    }

    atomic_add64(&g_pending64, 1);
    return 0;
}

static void cmd_set_paths() {
    read_line("Enter source path: ", g_src, sizeof(g_src));
    read_line("Enter destination path: ", g_dst, sizeof(g_dst));
    printf("OK\n");
}

static void cmd_open_src() {
    if (g_fd_in != -1) {
        printf("src already open\n");
        return;
    }
    if (g_src[0] == 0) {
        printf("set paths first\n");
        return;
    }

    g_fd_in = open(g_src, O_RDONLY | O_NONBLOCK, 0666);
    if (g_fd_in < 0) {
        print_errno("open(src)");
        g_fd_in = -1;
        return;
    }
    printf("open(src) OK fd=%d\n", g_fd_in);
}

static void cmd_open_dst() {
    if (g_fd_out != -1) {
        printf("dst already open\n");
        return;
    }
    if (g_dst[0] == 0) {
        printf("set paths first\n");
        return;
    }

    g_fd_out = open(g_dst, O_CREAT | O_WRONLY | O_TRUNC | O_NONBLOCK, 0666);
    if (g_fd_out < 0) {
        print_errno("open(dst)");
        g_fd_out = -1;
        return;
    }
    printf("open(dst) OK fd=%d\n", g_fd_out);
}

static void cmd_fstat_src() {
    if (g_fd_in < 0) {
        printf("open src first\n");
        return;
    }

    struct stat sb;
    if (fstat(g_fd_in, &sb) != 0) {
        print_errno("fstat");
        return;
    }

    g_file_size = sb.st_size;
    printf("fstat OK, file size = %" PRIdMAX " bytes\n", (intmax_t)g_file_size);
}

static void cmd_set_block() {
    char tmp[64];
    read_line("Enter block size (bytes): ", tmp, sizeof(tmp));
    size_t v = (size_t)strtoull(tmp, NULL, 10);
    if (v == 0) v = 1;
    g_block_size = v;
    printf("block = %zu\n", g_block_size);
}

static void cmd_set_inflight() {
    char tmp[64];
    read_line("Enter inflight (1,2,4,8,12,16,23,26...): ", tmp, sizeof(tmp));
    int v = atoi(tmp);
    if (v < 1) v = 1;
    g_inflight = v;
    printf("inflight = %d\n", g_inflight);
}

static void cmd_close_free() {
    close_all();
    free_ops();
    g_done = 0;
    g_error = 0;
    g_pending64 = 0;
    g_total_written64 = 0;
    g_next_offset = 0;
    g_file_size = 0;
    printf("closed + freed\n");
}

static void cmd_copy() {
    if (g_fd_in < 0 || g_fd_out < 0) {
        printf("open src and dst first\n");
        return;
    }
    if (g_file_size <= 0) {
        printf("run fstat first (file size must be > 0)\n");
        return;
    }

    free_ops();

    g_ops_count = g_inflight * 2;
    g_ops = (struct aio_operation*)calloc((size_t)g_ops_count, sizeof(struct aio_operation));
    if (!g_ops) {
        printf("calloc ops failed\n");
        return;
    }

    for (int i = 0; i < g_inflight; ++i) {
        struct aio_operation* r = &g_ops[i*2 + 0];
        r->buffer = (char*)malloc(g_block_size);
        if (!r->buffer) {
            printf("malloc buffer failed\n");
            free_ops();
            return;
        }
    }

    g_done = 0;
    g_error = 0;
    g_pending64 = 0;
    g_total_written64 = 0;
    g_next_offset = 0;

    const struct aiocb** activeList =
        (const struct aiocb**)malloc((size_t)g_ops_count * sizeof(struct aiocb*));
    if (!activeList) {
        printf("malloc activeList failed\n");
        free_ops();
        return;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (!g_done && !g_error) {
        for (int i = 0; i < g_inflight && g_next_offset < g_file_size && !g_error; ++i) {
            struct aio_operation* r = &g_ops[i*2 + 0];
            struct aio_operation* w = &g_ops[i*2 + 1];

            if (r->state == 0 && w->state == 0) {
                int sr = submit_read(r, w, g_next_offset);
                if (sr < 0) {
                    g_error = 1;
                    break;
                }
                if (sr > 0) {
                    g_next_offset += (off_t)g_block_size;
                }
            }
        }

        if (g_next_offset >= g_file_size &&
            atomic_load64(&g_pending64) == 0 &&
            all_slots_idle()) {
            g_done = 1;
            break;
        }

        if (g_error) break;

        int activeCount = 0;
        for (int i = 0; i < g_ops_count; ++i) {
            if (g_ops[i].state == 1 || g_ops[i].state == 3) {
                activeList[activeCount++] = &g_ops[i].aio;
            }
        }

        if (activeCount == 0) {
            usleep(1000);
            continue;
        }

        int s = aio_suspend(activeList, activeCount, NULL);
        if (s != 0 && errno != EINTR) {
            print_errno("aio_suspend");
            g_error = 1;
            break;
        }
    }

    while (atomic_load64(&g_pending64) > 0 && !g_error) {
        int activeCount = 0;
        for (int i = 0; i < g_ops_count; ++i) {
            if (g_ops[i].state == 1 || g_ops[i].state == 3) {
                activeList[activeCount++] = &g_ops[i].aio;
            }
        }
        if (activeCount == 0) break;
        aio_suspend(activeList, activeCount, NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);

    free(activeList);

    double sec = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (sec < 1e-9) sec = 1e-9;

    double mib = (double)g_file_size / (1024.0 * 1024.0);
    double speed = mib / sec;

    if (g_error) {
        printf("COPY FAILED (see errors above)\n");
    } else {
        printf("Copied %" PRIdMAX " bytes in %.6f sec => %.3f MiB/s\n",
               (intmax_t)g_file_size, sec, speed);
    }
    fflush(stdout);
}

static int menu() {
    printf("\n=== Linux Task2 AIO Menu (split) ===\n");
    printf(" 1) Set paths (src/dst)\n");
    printf(" 2) open(src)  [O_RDONLY | O_NONBLOCK]\n");
    printf(" 3) open(dst)  [O_CREAT|O_WRONLY|O_TRUNC|O_NONBLOCK]\n");
    printf(" 4) fstat(src) -> file size\n");
    printf(" 5) Set block size\n");
    printf(" 6) Set inflight\n");
    printf(" 7) Copy (aio_read/aio_write/aio_return/aio_suspend + handler)\n");
    printf(" 8) close/free\n");
    printf(" 0) Exit\n");
    printf("Select: ");
    fflush(stdout);

    char buf[32];
    if (!fgets(buf, sizeof(buf), stdin)) return -1;
    return atoi(buf);
}

int main() {
    for (;;) {
        int c = menu();
        if (c == 0) {
            cmd_close_free();
            return 0;
        }

        clr();

        switch (c) {
            case 1: cmd_set_paths(); break;
            case 2: cmd_open_src(); break;
            case 3: cmd_open_dst(); break;
            case 4: cmd_fstat_src(); break;
            case 5: cmd_set_block(); break;
            case 6: cmd_set_inflight(); break;
            case 7: cmd_copy(); break;
            case 8: cmd_close_free(); break;
            default: printf("Unknown option\n"); break;
        }

        pause_enter();
    }
}
