/*
Linux build & run:

# Build:
g++ -std=c++17 -O2 -Wall -Wextra -pedantic mmap_server.cpp -o mmap_server -pthread -lrt

# Run:
./mmap_server
*/

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <semaphore.h>

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

    sem_unlink(SEM_DATA_READY);
    sem_unlink(SEM_READ_DONE);

    g_fd = ::open(FILENAME, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (g_fd < 0) {
        printErr("open");
        return;
    }

    if (ftruncate(g_fd, static_cast<off_t>(FILESIZE)) != 0) {
        printErr("ftruncate");
        ::close(g_fd);
        g_fd = -1;
        return;
    }

    g_ptr = mmap(nullptr, FILESIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (g_ptr == MAP_FAILED) {
        g_ptr = nullptr;
        printErr("mmap");
        ::close(g_fd);
        g_fd = -1;
        return;
    }

    g_dataReady = sem_open(SEM_DATA_READY, O_CREAT | O_EXCL, 0600, 0);
    if (g_dataReady == SEM_FAILED) {
        printErr("sem_open(data_ready)");
        munmap(g_ptr, FILESIZE);
        g_ptr = nullptr;
        ::close(g_fd);
        g_fd = -1;
        return;
    }

    g_readDone = sem_open(SEM_READ_DONE, O_CREAT | O_EXCL, 0600, 0);
    if (g_readDone == SEM_FAILED) {
        printErr("sem_open(read_done)");
        sem_close(g_dataReady);
        sem_unlink(SEM_DATA_READY);
        g_dataReady = SEM_FAILED;
        munmap(g_ptr, FILESIZE);
        g_ptr = nullptr;
        ::close(g_fd);
        g_fd = -1;
        return;
    }

    g_mapped = true;
    std::cout << "OK: сервер создал файл и выполнил mmap.\n";
    std::cout << "Файл: " << FILENAME << ", размер: " << FILESIZE << " байт\n";
}

static void writeData() {
    if (!g_mapped || !g_ptr || g_dataReady == SEM_FAILED || g_readDone == SEM_FAILED) {
        std::cout << "Сначала выполните пункт 'выполнить проецирование'.\n";
        return;
    }

    std::cout << "Введите строку для записи:\n> ";
    std::string msg;
    std::getline(std::cin, msg);
    if (!std::cin) return;

    if (msg.size() + 1 > FILESIZE) {
        std::cout << "Строка слишком длинная для отображённого региона.\n";
        return;
    }

    std::memset(g_ptr, 0, FILESIZE);
    std::memcpy(g_ptr, msg.c_str(), msg.size() + 1);

    if (msync(g_ptr, FILESIZE, MS_SYNC) != 0) {
        printErr("msync");
    }

    if (sem_post(g_dataReady) != 0) {
        printErr("sem_post(data_ready)");
        return;
    }

    std::cout << "Данные записаны. Ожидание подтверждения чтения от клиента...\n";
    if (sem_wait(g_readDone) != 0) {
        printErr("sem_wait(read_done)");
        return;
    }

    std::cout << "Клиент подтвердил чтение данных.\n";
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

    sem_unlink(SEM_DATA_READY);
    sem_unlink(SEM_READ_DONE);
    unlink(FILENAME);
    g_mapped = false;

    std::cout << "Сервер завершён: отображение снято, файл и семафоры удалены.\n";
}

static void menu() {
    std::cout << "\n===== SERVER MENU =====\n"
              << "1) выполнить проецирование\n"
              << "2) записать данные\n"
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
        else if (s == "2") writeData();
        else if (s == "0") {
            cleanup();
            break;
        } else {
            std::cout << "Неизвестная команда.\n";
        }
    }
    return 0;
}
