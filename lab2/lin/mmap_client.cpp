/*
Linux build & run:

# Build:
g++ -std=c++17 -O2 -Wall -Wextra -pedantic mmap_client.cpp -o mmap_client -pthread -lrt

# Run:
./mmap_client
*/

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <semaphore.h>
#include <time.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

static constexpr const char* FILENAME = "/tmp/vm_lab_mmap_file.bin";
static constexpr const char* SEM_DATA_READY = "/vm_lab_mmap_data_ready";
static constexpr const char* SEM_READ_DONE  = "/vm_lab_mmap_read_done";
static constexpr size_t FILESIZE = 4096;

static int g_fd = -1;
static void* g_ptr = nullptr;
static sem_t* g_dataReady = SEM_FAILED;
static sem_t* g_readDone = SEM_FAILED;
static bool g_mapped = false;

static void printErr(const char* where) {
    std::cerr << where << " failed: " << std::strerror(errno) << "\n";
}

static void doMap() {
    if (g_mapped) {
        std::cout << "Проецирование уже выполнено.\n";
        return;
    }

    g_fd = ::open(FILENAME, O_RDONLY);
    if (g_fd < 0) {
        std::cout << "Файл ещё не создан сервером. Сначала запусти сервер и выполни пункт 1.\n";
        return;
    }

    g_ptr = mmap(nullptr, FILESIZE, PROT_READ, MAP_SHARED, g_fd, 0);
    if (g_ptr == MAP_FAILED) {
        g_ptr = nullptr;
        printErr("mmap");
        ::close(g_fd);
        g_fd = -1;
        return;
    }

    g_dataReady = sem_open(SEM_DATA_READY, 0);
    if (g_dataReady == SEM_FAILED) {
        std::cout << "Семафор data_ready ещё не создан сервером.\n";
        munmap(g_ptr, FILESIZE);
        g_ptr = nullptr;
        ::close(g_fd);
        g_fd = -1;
        return;
    }

    g_readDone = sem_open(SEM_READ_DONE, 0);
    if (g_readDone == SEM_FAILED) {
        std::cout << "Семафор read_done ещё не создан сервером.\n";
        sem_close(g_dataReady);
        g_dataReady = SEM_FAILED;
        munmap(g_ptr, FILESIZE);
        g_ptr = nullptr;
        ::close(g_fd);
        g_fd = -1;
        return;
    }

    g_mapped = true;
    std::cout << "OK: клиент открыл файл и выполнил mmap (только чтение).\n";
}

static void readData() {
    if (!g_mapped || !g_ptr || g_dataReady == SEM_FAILED || g_readDone == SEM_FAILED) {
        std::cout << "Сначала выполните пункт 'выполнить проецирование'.\n";
        return;
    }

    std::cout << "Ожидать данные: 1) бесконечно  2) с таймаутом\nВыбор: ";
    std::string s;
    std::getline(std::cin, s);
    if (!std::cin) return;

    if (s == "1") {
        if (sem_wait(g_dataReady) != 0) {
            printErr("sem_wait(data_ready)");
            return;
        }
    } else if (s == "2") {
        std::cout << "Введите таймаут в секундах: ";
        std::getline(std::cin, s);
        if (!std::cin) return;

        int sec = 0;
        try {
            sec = std::stoi(s);
        } catch (...) {
            std::cout << "Некорректный таймаут.\n";
            return;
        }

        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += sec;

        if (sem_timedwait(g_dataReady, &ts) != 0) {
            if (errno == ETIMEDOUT) {
                std::cout << "Таймаут: данные от сервера не получены.\n";
                return;
            }
            printErr("sem_timedwait(data_ready)");
            return;
        }
    } else {
        std::cout << "Неизвестный режим ожидания.\n";
        return;
    }

    const char* p = static_cast<const char*>(g_ptr);
    std::cout << "Client received: " << p << "\n";

    if (sem_post(g_readDone) != 0) {
        printErr("sem_post(read_done)");
        return;
    }

    std::cout << "Подтверждение чтения отправлено серверу.\n";
}

static void cleanup() {
    if (g_ptr) {
        munmap(g_ptr, FILESIZE);
        g_ptr = nullptr;
    }
    if (g_fd >= 0) {
        ::close(g_fd);
        g_fd = -1;
    }
    if (g_dataReady != SEM_FAILED) {
        sem_close(g_dataReady);
        g_dataReady = SEM_FAILED;
    }
    if (g_readDone != SEM_FAILED) {
        sem_close(g_readDone);
        g_readDone = SEM_FAILED;
    }
    g_mapped = false;

    std::cout << "Клиент завершён: отображение снято, дескрипторы закрыты.\n";
}

static void menu() {
    std::cout << "\n===== CLIENT MENU =====\n"
              << "1) выполнить проецирование\n"
              << "2) прочитать данные\n"
              << "0) завершить работу\n"
              << "Выбор: ";
}

int main() {
    while (true) {
        menu();
        std::string s;
        std::getline(std::cin, s);
        if (!std::cin) break;

        if (s == "1") doMap();
        else if (s == "2") readData();
        else if (s == "0") {
            cleanup();
            break;
        } else {
            std::cout << "Неизвестная команда.\n";
        }
    }
    return 0;
}
