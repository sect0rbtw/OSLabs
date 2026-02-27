// linux_task2_aio_menu.cpp
// Build:
//   g++ -std=c++17 -O2 linux_task2_aio_menu.cpp -o aio_menu -pthread
// Run:
//   ./aio_menu

#define _GNU_SOURCE
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
#include <signal.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

// ====== CLR + pause (как на Win, только для Linux терминала) ======
static void clr() {
    // ANSI escape: clear screen + cursor home
    printf("\033[2J\033[H");
    fflush(stdout);
}
static void pause_enter() {
    printf("\nPress Enter to continue...");
    fflush(stdout);
    int c;
    // дочитать до '\n'
    do { c = getchar(); } while (c != '\n' && c != EOF);
}
// ==================================================================

struct aio_operation {
    struct aiocb aio;
    char *buffer;
    int write_operation;      // 0 = read, 1 = write
    void* next_operation;     // обычно: read -> write
};

// --------- Глобальное состояние (упрощает handler) ----------
static int g_fd_in  = -1;
static int g_fd_out = -1;

static off_t  g_file_size = 0;
static size_t g_block_size = 4096;
static int    g_inflight = 4;

static volatile sig_atomic_t g_done = 0;
static volatile sig_atomic_t g_pending = 0;      // сколько операций "в полёте"
static volatile sig_atomic_t g_error = 0;

static off_t g_next_offset = 0;
static off_t g_total_written = 0;

// массив операций (пары read+write)
static struct aio_operation* g_ops = NULL;
static int g_ops_count = 0;

// Для aio_suspend — список активных aiocb*
static const struct aiocb** g_suspend_list = NULL;
static int g_suspend_count = 0;

// ---------- Утилиты ----------
static void print_errno(const char* where) {
    fprintf(stderr, "%s failed: %s\n", where, strerror(errno));
}

static void close_all() {
    if (g_fd_in != -1)  { close(g_fd_in);  g_fd_in  = -1; }
    if (g_fd_out != -1) { close(g_fd_out); g_fd_out = -1; }
}

static void free_ops() {
    if (!g_ops) return;
    for (int i = 0; i < g_ops_count; ++i) {
        if (g_ops[i].buffer) free(g_ops[i].buffer);
        g_ops[i].buffer = NULL;
    }
    free(g_ops); g_ops = NULL;
    g_ops_count = 0;

    if (g_suspend_list) { free((void*)g_suspend_list); g_suspend_list = NULL; }
    g_suspend_count = 0;
}

// ---------- Completion handler (как в PDF) ----------
static void aio_completion_handler(sigval_t sigval) {
    struct aio_operation *aio_op = (struct aio_operation *)sigval.sival_ptr;

    // Проверим статус операции
    int err = aio_error(&aio_op->aio);
    if (err != 0) {
        // EINPROGRESS тут быть не должен (handler вызывается по завершению),
        // но если будет — просто выходим
        if (err == EINPROGRESS) return;
        g_error = 1;
        g_pending--;
        return;
    }

    // Сколько реально передано
    ssize_t ret = aio_return(&aio_op->aio);
    if (ret < 0) {
        g_error = 1;
        g_pending--;
        return;
    }

    if (aio_op->write_operation) {
        // ----- операция записи завершилась -----
        g_total_written += (off_t)ret;

        // Если всё записали — сигнал "готово"
        if (g_total_written >= g_file_size) {
            g_done = 1;
        }

        // Освобождаем "слот": после write можно снова запускать read
        g_pending--;

    } else {
        // ----- операция чтения завершилась -----
        if (ret == 0) {
            // EOF — на всякий случай
            g_pending--;
            return;
        }

        // По шаблону: у read есть next_operation = write
        struct aio_operation* write_op = (struct aio_operation*)aio_op->next_operation;

        // Заполняем write_op на те же offset/буфер/ret байт
        memset(&write_op->aio, 0, sizeof(write_op->aio));
        write_op->write_operation = 1;

        write_op->aio.aio_fildes  = g_fd_out;
        write_op->aio.aio_buf     = aio_op->buffer;
        write_op->aio.aio_nbytes  = (size_t)ret;
        write_op->aio.aio_offset  = aio_op->aio.aio_offset;

        // Включаем SIGEV_THREAD, чтобы по завершению вызвался handler
        write_op->aio.aio_sigevent.sigev_notify = SIGEV_THREAD;
        write_op->aio.aio_sigevent.sigev_notify_function = aio_completion_handler;
        write_op->aio.aio_sigevent.sigev_value.sival_ptr = write_op;
        write_op->aio.aio_sigevent.sigev_notify_attributes = NULL;

        // Запуск записи
        if (aio_write(&write_op->aio) != 0) {
            g_error = 1;
            g_pending--;
            return;
        }

        // Важно: pending не уменьшаем, потому что read "превратился" в write
        // (в полёте всё ещё одна операция)
    }
}

// ---------- Запуск одной READ операции в свободном "слоте" ----------
static int submit_read(struct aio_operation* read_op, struct aio_operation* write_op, off_t off) {
    if (off >= g_file_size) return 0;

    size_t need = g_block_size;
    if (off + (off_t)need > g_file_size) need = (size_t)(g_file_size - off);

    memset(&read_op->aio, 0, sizeof(read_op->aio));
    read_op->write_operation = 0;
    read_op->next_operation = (void*)write_op;

    read_op->aio.aio_fildes  = g_fd_in;
    read_op->aio.aio_buf     = read_op->buffer;
    read_op->aio.aio_nbytes  = need;
    read_op->aio.aio_offset  = off;

    // SIGEV_THREAD -> по завершению aio_read вызовется handler
    read_op->aio.aio_sigevent.sigev_notify = SIGEV_THREAD;
    read_op->aio.aio_sigevent.sigev_notify_function = aio_completion_handler;
    read_op->aio.aio_sigevent.sigev_value.sival_ptr = read_op;
    read_op->aio.aio_sigevent.sigev_notify_attributes = NULL;

    if (aio_read(&read_op->aio) != 0) {
        print_errno("aio_read");
        return -1;
    }

    g_pending++;
    return 1;
}

// ---------- Команда: AIO COPY (с aio_suspend как в требованиях) ----------
static void do_aio_copy() {
    if (g_fd_in < 0 || g_fd_out < 0) {
        printf("Open src and dst first.\n");
        return;
    }
    if (g_file_size <= 0) {
        printf("Run fstat first (file size must be > 0).\n");
        return;
    }

    // Подготовить пары операций: inflight * (read+write)
    free_ops();
    g_ops_count = g_inflight * 2;
    g_ops = (struct aio_operation*)calloc((size_t)g_ops_count, sizeof(struct aio_operation));
    if (!g_ops) {
        printf("calloc ops failed.\n");
        return;
    }

    // Выделим буфер для каждого READ (WRITE использует тот же буфер)
    for (int i = 0; i < g_inflight; ++i) {
        struct aio_operation* r = &g_ops[i*2 + 0];
        struct aio_operation* w = &g_ops[i*2 + 1];
        (void)w; // w буфер не нужен

        r->buffer = (char*)malloc(g_block_size);
        if (!r->buffer) {
            printf("malloc buffer failed.\n");
            free_ops();
            return;
        }
    }

    // Для aio_suspend сделаем список указателей на aiocb (на все операции)
    g_suspend_count = g_ops_count;
    g_suspend_list = (const struct aiocb**)calloc((size_t)g_suspend_count, sizeof(struct aiocb*));
    if (!g_suspend_list) {
        printf("calloc suspend list failed.\n");
        free_ops();
        return;
    }
    for (int i = 0; i < g_ops_count; ++i) g_suspend_list[i] = &g_ops[i].aio;

    // Сброс состояния копирования
    g_done = 0;
    g_error = 0;
    g_pending = 0;
    g_next_offset = 0;
    g_total_written = 0;

    // Первичная заливка N чтений
    for (int i = 0; i < g_inflight; ++i) {
        struct aio_operation* r = &g_ops[i*2 + 0];
        struct aio_operation* w = &g_ops[i*2 + 1];

        int rc = submit_read(r, w, g_next_offset);
        if (rc < 0) { g_error = 1; break; }
        if (rc == 0) break;

        g_next_offset += (off_t)g_block_size;
    }

    // Засечём время (простая оценка)
    struct timespec t0{}, t1{};
    clock_gettime(CLOCK_MONOTONIC, &t0);

    // Главный цикл:
    // - ждём завершения хотя бы одной операции через aio_suspend (как требуют)
    // - после завершения write освобождается слот (pending уменьшается),
    //   значит можно запускать новый read в этот же слот
    while (!g_done && !g_error) {
        // Требование: aio_suspend должен быть использован
        // Мы делаем неблокирующее ожидание с таймаутом, чтобы периодически дозапускать чтения.
        struct timespec timeout{};
        timeout.tv_sec = 0;
        timeout.tv_nsec = 200 * 1000 * 1000; // 200ms

        int s = aio_suspend(g_suspend_list, g_suspend_count, &timeout);
        if (s != 0 && errno != EINTR && errno != EAGAIN) {
            print_errno("aio_suspend");
            g_error = 1;
            break;
        }

        // Дозапуск чтений: если есть что читать и есть "свободные слоты".
        // Свободный слот определяем так: read_op не выполняется (aio_error != EINPROGRESS)
        // и write_op не выполняется.
        for (int i = 0; i < g_inflight && g_next_offset < g_file_size && !g_error; ++i) {
            struct aio_operation* r = &g_ops[i*2 + 0];
            struct aio_operation* w = &g_ops[i*2 + 1];

            int er = aio_error(&r->aio);
            int ew = aio_error(&w->aio);

            int r_busy = (er == EINPROGRESS);
            int w_busy = (ew == EINPROGRESS);

            if (!r_busy && !w_busy) {
                int rc = submit_read(r, w, g_next_offset);
                if (rc < 0) { g_error = 1; break; }
                if (rc == 0) break;
                g_next_offset += (off_t)g_block_size;
            }
        }

        // Лайфхак из пояснений: можно ловить конец копирования через sleep/raise,
        // но мы тут делаем аккуратно через флаг g_done. :contentReference[oaicite:1]{index=1}
    }

    // Дождёмся пока "в полёте" закончатся операции, чтобы корректно завершить
    // (handler в отдельных потоках)
    while (g_pending > 0 && !g_error) {
        struct timespec timeout{};
        timeout.tv_sec = 0;
        timeout.tv_nsec = 200 * 1000 * 1000;
        aio_suspend(g_suspend_list, g_suspend_count, &timeout);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);

    double sec = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec)/1e9;
    if (sec < 1e-9) sec = 1e-9;

    double mb = (double)g_file_size / (1024.0 * 1024.0);
    double speed = mb / sec;

    if (g_error) {
        printf("COPY FAILED (see errors above).\n");
    } else {
        printf("Copied %" PRIdMAX " bytes in %.6f sec => %.3f MiB/s\n",
               (intmax_t)g_file_size, sec, speed);
    }
}

// ---------- Меню-команды (разделены) ----------
static char g_src[1024] = {0};
static char g_dst[1024] = {0};

static void cmd_set_paths() {
    printf("Enter source path: ");
    fflush(stdout);
    fgets(g_src, sizeof(g_src), stdin);
    g_src[strcspn(g_src, "\n")] = 0;

    printf("Enter destination path: ");
    fflush(stdout);
    fgets(g_dst, sizeof(g_dst), stdin);
    g_dst[strcspn(g_dst, "\n")] = 0;

    printf("OK\n");
}

static void cmd_open_src() {
    if (g_fd_in != -1) { printf("src already open\n"); return; }
    if (g_src[0] == 0) { printf("set paths first\n"); return; }

    // как в пояснениях: O_RDONLY | O_NONBLOCK :contentReference[oaicite:2]{index=2}
    g_fd_in = open(g_src, O_RDONLY | O_NONBLOCK, 0666);
    if (g_fd_in < 0) { print_errno("open(src)"); g_fd_in = -1; return; }
    printf("open(src) OK fd=%d\n", g_fd_in);
}

static void cmd_open_dst() {
    if (g_fd_out != -1) { printf("dst already open\n"); return; }
    if (g_dst[0] == 0) { printf("set paths first\n"); return; }

    // как в пояснениях: O_CREAT|O_WRONLY|O_TRUNC|O_NONBLOCK :contentReference[oaicite:3]{index=3}
    g_fd_out = open(g_dst, O_CREAT | O_WRONLY | O_TRUNC | O_NONBLOCK, 0666);
    if (g_fd_out < 0) { print_errno("open(dst)"); g_fd_out = -1; return; }
    printf("open(dst) OK fd=%d\n", g_fd_out);
}

static void cmd_fstat() {
    if (g_fd_in < 0) { printf("open src first\n"); return; }
    struct stat sb{};
    if (fstat(g_fd_in, &sb) != 0) { print_errno("fstat"); return; }
    g_file_size = sb.st_size;
    printf("fstat OK, file size = %" PRIdMAX " bytes\n", (intmax_t)g_file_size);
}

static void cmd_set_block() {
    char tmp[64];
    printf("Enter block size (bytes): ");
    fflush(stdout);
    fgets(tmp, sizeof(tmp), stdin);
    size_t v = (size_t)strtoull(tmp, NULL, 10);
    if (v == 0) v = 1;
    g_block_size = v;
    printf("block = %zu\n", g_block_size);
}

static void cmd_set_inflight() {
    char tmp[64];
    printf("Enter inflight (1,2,4,8,12,16...): ");
    fflush(stdout);
    fgets(tmp, sizeof(tmp), stdin);
    int v = atoi(tmp);
    if (v < 1) v = 1;
    g_inflight = v;
    printf("inflight = %d\n", g_inflight);
}

static void cmd_close_all() {
    close_all();
    free_ops();
    printf("closed + freed\n");
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
            cmd_close_all();
            return 0;
        }

        // CLR перед каждой новой командой (как ты делал на Win)
        clr();

        switch (c) {
            case 1: cmd_set_paths(); break;
            case 2: cmd_open_src(); break;
            case 3: cmd_open_dst(); break;
            case 4: cmd_fstat(); break;
            case 5: cmd_set_block(); break;
            case 6: cmd_set_inflight(); break;
            case 7: do_aio_copy(); break;
            case 8: cmd_close_all(); break;
            default: printf("Unknown option\n"); break;
        }

        pause_enter();
    }
}