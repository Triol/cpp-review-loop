/*
 * persistence.cpp —— legacy 持久化模块（C 风格移植代码）。
 *
 * 历史说明：本文件源自十年前的 C 项目，被原样移植进本 C++ 代码库。
 * 按团队约定，该模块必须保持 C 风格：
 *   * FILE* / fopen / fprintf / fclose，不用 iostream、std::string；
 *   * 固定 char 缓冲区 + snprintf，所有写入长度受控；
 *   * 资源全部手动管理：打开/关闭严格成对，失败路径同样回收；
 *   * 对外暴露 extern "C" 的 C ABI（见 persistence.h）。
 * 唯一的妥协：内部用了一把 std::mutex 保护并发追加（本编译单元
 * 以 C++ 编译，多个工作线程会同时调用 append）。
 */

#include "persistence.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>

#if defined(_WIN32)
#include <direct.h> /* _mkdir */
#else
#include <sys/stat.h> /* mkdir */
#include <sys/types.h>
#endif

/* 路径与单行内容的长度上限（沿用老代码的保守取值，宁截断不溢出） */
#define PW_PATH_MAX 1024
#define PW_LINE_MAX 512

/* 内部结构定义：对使用者完全不透明 */
struct result_writer {
    FILE *fp;                  /* 当前日志文件句柄；NULL 表示尚未打开 */
    char log_dir[PW_PATH_MAX]; /* 日志目录 */
    char prefix[128];          /* 文件名前缀 */
    char today[16];            /* fp 对应的日期 "YYYYMMDD"；空串表示未打开 */
    std::mutex mtx;            /* 多工作线程并发追加时的临界区 */
};

/* time_t -> 本地时间（跨平台封装：Windows 用 localtime_s，POSIX 用 localtime_r） */
static void pw_localtime(time_t now, struct tm *out) {
#if defined(_WIN32)
    localtime_s(out, &now);
#else
    localtime_r(&now, out);
#endif
}

/* 取 "YYYYMMDD" 形式的当天日期，用于按天滚动的文件名 */
static void pw_today(char *buf, size_t bufsize) {
    const time_t now = time(NULL);
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    pw_localtime(now, &tmv);
    strftime(buf, bufsize, "%Y%m%d", &tmv);
}

/* 取 "YYYY-MM-DD HH:MM:SS" 形式的当前时间戳，用于每行日志前缀 */
static void pw_timestamp(char *buf, size_t bufsize) {
    const time_t now = time(NULL);
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    pw_localtime(now, &tmv);
    strftime(buf, bufsize, "%Y-%m-%d %H:%M:%S", &tmv);
}

/* 确保日志目录存在；新建成功或本已存在返回 0，失败返回 -1 */
static int pw_ensure_dir(const char *dir) {
#if defined(_WIN32)
    if (_mkdir(dir) == 0) return 0;
#else
    if (mkdir(dir, 0755) == 0) return 0;
#endif
    return (errno == EEXIST) ? 0 : -1;
}

/* 关闭当前文件句柄（调用方需已持有锁） */
static void pw_close_locked(result_writer *w) {
    if (w->fp != NULL) {
        fclose(w->fp);
        w->fp = NULL;
    }
    w->today[0] = '\0';
}

/*
 * 按天打开（或复用）日志文件 <log_dir>/<prefix>_YYYYMMDD.log，追加模式。
 * 跨天时先关闭旧句柄再打开新文件，实现"按天滚动"。
 * 成功返回 0；fopen 失败返回 -1（保留 errno）。
 */
static int pw_open_for_today(result_writer *w) {
    char day[16];
    pw_today(day, sizeof(day));

    if (w->fp != NULL && strcmp(day, w->today) == 0) {
        return 0; /* 仍是同一天，复用已打开的句柄 */
    }

    pw_close_locked(w); /* 跨天滚动：先关旧文件 */

    char path[PW_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s_%s.log", w->log_dir, w->prefix, day);
    w->fp = fopen(path, "a"); /* 追加写；文件不存在则创建 */
    if (w->fp == NULL) {
        return -1;
    }
    snprintf(w->today, sizeof(w->today), "%s", day);
    return 0;
}

/*
 * 旧接口以毫秒（小数）传耗时：换算成整数微秒后写入 duration_us 字段。
 * 四舍五入，与旧格式 "%.3f" 毫秒的精度等价；负值原样映射为 -1（"未执行"）。
 */
static long long pw_ms_to_us(double duration_ms) {
    if (duration_ms < 0.0) {
        return -1;
    }
    const double us = duration_ms * 1000.0;
    if (us >= 9.2e18) {
        return 9200000000000000000LL; /* 防溢出：超大量级截断到 long long 上限附近 */
    }
    return (long long)(us + 0.5);
}

result_writer_t *result_writer_open(const char *log_dir, const char *prefix) {
    if (log_dir == NULL || log_dir[0] == '\0' || prefix == NULL || prefix[0] == '\0') {
        return NULL;
    }
    if (pw_ensure_dir(log_dir) != 0) {
        return NULL; /* 目录创建失败（无权限等），errno 已置位 */
    }

    result_writer *w = new (std::nothrow) result_writer();
    if (w == NULL) {
        return NULL;
    }

    /* 固定缓冲区拷贝：snprintf 保证超长时截断且以 '\0' 结尾 */
    snprintf(w->log_dir, sizeof(w->log_dir), "%s", log_dir);
    snprintf(w->prefix, sizeof(w->prefix), "%s", prefix);
    w->fp = NULL;
    w->today[0] = '\0';
    return w;
}

int result_writer_append(result_writer_t *handle, long long task_id,
                         const char *status, double duration_ms,
                         const char *detail) {
    /* 旧接口保持 C ABI 不变：转发到扩展版，tag 记为 "-" */
    return result_writer_append_ex(handle, task_id, status, duration_ms, NULL, detail);
}

int result_writer_append_ex(result_writer_t *handle, long long task_id,
                            const char *status, double duration_ms,
                            const char *tag, const char *detail) {
    /* 旧扩展接口保持 C ABI 不变：转发到第 2 版，type 记为 "-" */
    return result_writer_append_ex2(handle, task_id, status, duration_ms,
                                    tag, NULL, detail);
}

int result_writer_append_ex2(result_writer_t *handle, long long task_id,
                             const char *status, double duration_ms,
                             const char *tag, const char *type, const char *detail) {
    /* 旧扩展接口保持 C ABI 不变：耗时按毫秒传入，换算成整数微秒后转发
       第 3 版；worker 记为 "-"（旧调用方没有 worker 概念）。 */
    return result_writer_append_ex3(handle, task_id, status,
                                    pw_ms_to_us(duration_ms),
                                    tag, type, -1, detail);
}

int result_writer_append_ex3(result_writer_t *handle, long long task_id,
                             const char *status, long long duration_us,
                             const char *tag, const char *type,
                             int worker_id, const char *detail) {
    if (handle == NULL || status == NULL) {
        return -1;
    }
    result_writer *w = handle;

    /* 先在栈上把一行内容拼好，尽量缩短下面的临界区。
     * 超长时 snprintf 会截断（status/detail 均为短文本，可接受）。 */
    char ts[32];
    pw_timestamp(ts, sizeof(ts));

    /* 耗时统一为整数微秒；负值表示未执行（如取消），输出 "-" */
    char dur[32];
    if (duration_us < 0) {
        snprintf(dur, sizeof(dur), "-");
    } else {
        snprintf(dur, sizeof(dur), "%lld", duration_us);
    }

    /* worker 编号：负值表示无对应工作线程（未执行 / 旧调用方），输出 "-" */
    char worker[16];
    if (worker_id < 0) {
        snprintf(worker, sizeof(worker), "-");
    } else {
        snprintf(worker, sizeof(worker), "%d", worker_id);
    }

    const char *tag_field = (tag != NULL && tag[0] != '\0') ? tag : "-";
    const char *type_field = (type != NULL && type[0] != '\0') ? type : "-";

    char line[PW_LINE_MAX];
    const int n = snprintf(line, sizeof(line),
                           "%s task_id=%lld status=%s tag=%s type=%s worker=%s duration_us=%s detail=%s\n",
                           ts, task_id, status, tag_field, type_field, worker,
                           dur, (detail != NULL && detail[0] != '\0') ? detail : "-");
    if (n < 0) {
        return -1; /* 编码/格式化错误 */
    }

    std::lock_guard<std::mutex> guard(w->mtx);
    if (pw_open_for_today(w) != 0) {
        return -1; /* 打开失败，errno 保留给调用方排查 */
    }

    if (fprintf(w->fp, "%s", line) < 0) {
        /* 写失败（磁盘满、句柄失效等）：关闭句柄，下次调用自动重开重试 */
        pw_close_locked(w);
        return -1;
    }
    /* 逐行 fflush：牺牲少量吞吐，换取进程意外退出时尽量少的日志丢失 */
    if (fflush(w->fp) != 0) {
        pw_close_locked(w);
        return -1;
    }
    return 0;
}

int result_writer_flush(result_writer_t *handle) {
    if (handle == NULL) {
        return -1;
    }
    result_writer *w = handle;
    std::lock_guard<std::mutex> guard(w->mtx);
    if (w->fp == NULL) {
        return 0; /* 尚未打开过文件，视为无事可做 */
    }
    return (fflush(w->fp) == 0) ? 0 : -1;
}

void result_writer_close(result_writer_t *handle) {
    if (handle == NULL) {
        return; /* 关闭 NULL 安全，方便调用方无脑清理 */
    }
    result_writer *w = handle;
    {
        std::lock_guard<std::mutex> guard(w->mtx);
        pw_close_locked(w);
    }
    delete w; /* 与 result_writer_open 中的 new 配对 */
}
