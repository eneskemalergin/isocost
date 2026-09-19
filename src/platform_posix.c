/* Linux and POSIX implementation of platform.h (also builds for macOS). */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE /* sysconf(_SC_NPROCESSORS_ONLN) */
#endif
#include "platform.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static _Thread_local int last_errno;

const char *pf_last_error(void) { return strerror(last_errno); }

static int kind_of(const struct stat *st) {
    if (S_ISREG(st->st_mode)) return PF_FILE;
    if (S_ISDIR(st->st_mode)) return PF_DIR;
    return PF_OTHER;
}

int pf_kind(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return PF_NONE;
    return kind_of(&st);
}

int pf_kind_nofollow(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return PF_NONE;
    if (S_ISLNK(st.st_mode)) {
        if (stat(path, &st) != 0) return PF_NONE;
        return S_ISREG(st.st_mode) ? PF_FILE : PF_OTHER; /* never walk into linked directories */
    }
    return kind_of(&st);
}

int pf_list_dir(const char *path, void (*visit)(void *ctx, const char *name), void *ctx) {
    DIR *dir = opendir(path);
    struct dirent *entry;
    if (!dir) {
        last_errno = errno;
        return 0;
    }
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.' && (!entry->d_name[1] || (entry->d_name[1] == '.' && !entry->d_name[2]))) continue;
        visit(ctx, entry->d_name);
    }
    closedir(dir);
    return 1;
}

int pf_mkdir(const char *path) {
    if (mkdir(path, 0755) == 0 || (errno == EEXIST && pf_kind(path) == PF_DIR)) return 1;
    last_errno = errno;
    return 0;
}

int pf_remove(const char *path) {
    if (unlink(path) == 0 || errno == ENOENT) return 1;
    last_errno = errno;
    return 0;
}

int pf_mkdir_all(const char *path) {
    char *copy = xstrdup(path);
    int ok = 1;
    for (char *p = copy + 1; *p && ok; p++)
        if (*p == '/') {
            *p = '\0';
            ok = pf_mkdir(copy);
            *p = '/';
        }
    if (ok) ok = pf_mkdir(copy);
    free(copy);
    return ok;
}

int pf_read_file(const char *path, unsigned char **data, size_t *length, size_t limit, const char **error) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    unsigned char *buffer;
    size_t done = 0;
    *data = NULL;
    *length = 0;
    if (fd < 0) {
        *error = strerror(errno);
        return 0;
    }
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        *error = strerror(errno);
        close(fd);
        return 0;
    }
    if ((size_t)st.st_size > limit) {
        *error = "file is larger than the input limit";
        close(fd);
        return 0;
    }
    buffer = xmalloc((size_t)st.st_size + 1);
    while (done < (size_t)st.st_size) {
        ssize_t got = read(fd, buffer + done, (size_t)st.st_size - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            *error = got < 0 ? strerror(errno) : "file shrank while reading";
            free(buffer);
            close(fd);
            return 0;
        }
        done += (size_t)got;
    }
    close(fd);
    buffer[done] = '\0';
    *data = buffer;
    *length = done;
    return 1;
}

int pf_write_file_atomic(const char *path, const void *data, size_t length) {
    char *tmp = xasprintf("%s.tmp.%ld", path, (long)getpid());
    const unsigned char *p = data;
    size_t done = 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        last_errno = errno;
        free(tmp);
        return 0;
    }
    while (done < length) {
        ssize_t put = write(fd, p + done, length - done);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) {
            last_errno = errno;
            close(fd);
            unlink(tmp);
            free(tmp);
            return 0;
        }
        done += (size_t)put;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        last_errno = errno;
        unlink(tmp);
        free(tmp);
        return 0;
    }
    free(tmp);
    return 1;
}

double pf_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int pf_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

void pf_run_threads(int count, void *(*worker)(void *), void *arg) {
    pthread_t *threads;
    int started = 0;
    if (count <= 1) {
        worker(arg);
        return;
    }
    threads = xcalloc((size_t)count, sizeof(pthread_t));
    for (int i = 0; i < count; i++)
        if (pthread_create(&threads[started], NULL, worker, arg) == 0) started++;
    if (!started) worker(arg);
    for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
    free(threads);
}

long pf_counter_next(PfCounter *counter) { return __atomic_fetch_add(&counter->value, 1, __ATOMIC_SEQ_CST); }
