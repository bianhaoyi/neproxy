/*
 * twemproxy - A fast and lightweight proxy for memcached protocol.
 * Copyright (C) 2011 Twitter, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <nc_core.h>

static struct logger logger;

int
log_init(int level, char *name)
{
    struct logger *l = &logger;

    l->level = MAX(LOG_EMERG, MIN(level, LOG_PVERB));
    l->name = name;
    if (name == NULL || !strlen(name)) {
        l->fd = STDERR_FILENO;
    } else {
        l->fd = open(name, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (l->fd < 0) {
            log_stderr("opening log file '%s' failed: %s", name,
                       strerror(errno));
            return -1;
        }
    }

    return 0;
}

void
log_deinit(void)
{
    struct logger *l = &logger;

    if (l->fd < 0 || l->fd == STDERR_FILENO) {
        return;
    }

    close(l->fd);
}

static void
log_rotate_name(char *buf, size_t len, const char *old)
{
    struct tm *local;
    time_t t;

    t = time(NULL);
    local = localtime(&t);
    snprintf(buf, len, "%s.%d%02d%02d%02d", old,
             1900+local->tm_year, 1+local->tm_mon, local->tm_mday, local->tm_hour);
}

void
log_reopen(void)
{
    struct logger *l = &logger;

#ifdef NC_LOG_ROTATE
    char buf[LOG_MAX_NAME_LEN];

    if (l->fd == STDERR_FILENO) {
        return;
    }

    log_rotate_name(buf, sizeof(buf), l->name);
    
    if (rename(l->name, buf) != 0) {
        log_stderr("renaming log file '%s' failed, ignored: %s", l->name,
                   strerror(errno));
        return;
    }
#endif

    close(l->fd);
    
    l->fd = open(l->name, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (l->fd < 0) {
        log_stderr_safe("reopening log file '%s' failed, ignored: %s", l->name,
                   strerror(errno));
    }

}

void
log_level_up(void)
{
    struct logger *l = &logger;

    if (l->level < LOG_PVERB) {
        l->level++;
        log_safe("up log level to %d", l->level);
    }
}

void
log_level_down(void)
{
    struct logger *l = &logger;

    if (l->level > LOG_EMERG) {
        l->level--;
        log_safe("down log level to %d", l->level);
    }
}

void
log_level_set(int level)
{
    struct logger *l = &logger;

    l->level = MAX(LOG_EMERG, MIN(level, LOG_PVERB));
    loga("set log level to %d", l->level);
}

void
log_stacktrace(void)
{
    struct logger *l = &logger;
    if (l->fd < 0) {
        return;
    }
    nc_stacktrace_fd(l->fd);
}

int
log_loggable(int level)
{
    struct logger *l = &logger;

    if (level > l->level) {
        return 0;
    }

    return 1;
}

const char *
log_level_str(int level)
{
    switch (level) {
    case LOG_ALWAYS:
        return "ALWAYS";
    case LOG_EMERG:
        return "EMERG";
    case LOG_ALERT:
        return "ALERT";
    case LOG_CRIT:
        return "CRIT";
    case LOG_WARN:
        return "WARN";
    case LOG_NOTICE:
        return "NOTICE";
    case LOG_INFO:
        return "INFO";
    case LOG_DEBUG:
        return "DEBUG";
    case LOG_VERB:
        return "VERB";
    case LOG_VVERB:
        return "VVERB";
    case LOG_VVVERB:
        return "VVVERB";
    case LOG_PVERB:
        return "PVERB";
    default:
        return "";
    }
}

void
_log(int level, const char *file, int line, int panic, const char *fmt, ...)
{
    struct logger *l = &logger;
    int len, size, errno_save;
    char buf[LOG_MAX_LEN], *timestr;
    va_list args;
    struct tm *local;
    time_t t;
    ssize_t n;

    if (l->fd < 0) {
        return;
    }

    errno_save = errno;
    len = 0;            /* length of output buffer */
    size = LOG_MAX_LEN; /* size of output buffer */

    t = time(NULL);
    local = localtime(&t);
    timestr = asctime(local);

    len += nc_scnprintf(buf + len, size - len, "[%s] [%.*s] %s:%d ",
                        log_level_str(level), strlen(timestr) - 1, timestr, file, line);

    va_start(args, fmt);
    len += nc_vscnprintf(buf + len, size - len, fmt, args);
    va_end(args);

    buf[len++] = '\n';

    n = nc_write(l->fd, buf, len);
    if (n < 0) {
        l->nerror++;
    }

    errno = errno_save;

    if (panic) {
        abort();
    }
}

void
_log_stderr(const char *fmt, ...)
{
    struct logger *l = &logger;
    int len, size, errno_save;
    char buf[4 * LOG_MAX_LEN];
    va_list args;
    ssize_t n;

    errno_save = errno;
    len = 0;                /* length of output buffer */
    size = 4 * LOG_MAX_LEN; /* size of output buffer */

    va_start(args, fmt);
    len += nc_vscnprintf(buf, size, fmt, args);
    va_end(args);

    buf[len++] = '\n';

    n = nc_write(STDERR_FILENO, buf, len);
    if (n < 0) {
        l->nerror++;
    }

    errno = errno_save;
}

/*
 * Hexadecimal dump in the canonical hex + ascii display
 * See -C option in man hexdump
 */
void
_log_hexdump(const char *file, int line, char *data, int datalen,
             const char *fmt, ...)
{
    struct logger *l = &logger;
    char buf[8 * LOG_MAX_LEN];
    int i, off, len, size, errno_save;
    ssize_t n;

    if (l->fd < 0) {
        return;
    }

    /* log hexdump */
    errno_save = errno;
    off = 0;                  /* data offset */
    len = 0;                  /* length of output buffer */
    size = 8 * LOG_MAX_LEN;   /* size of output buffer */

    while (datalen != 0 && (len < size - 1)) {
        char *save, *str;
        unsigned char c;
        int savelen;

        len += nc_scnprintf(buf + len, size - len, "%08x  ", off);

        save = data;
        savelen = datalen;

        for (i = 0; datalen != 0 && i < 16; data++, datalen--, i++) {
            c = (unsigned char)(*data);
            str = (i == 7) ? "  " : " ";
            len += nc_scnprintf(buf + len, size - len, "%02x%s", c, str);
        }
        for ( ; i < 16; i++) {
            str = (i == 7) ? "  " : " ";
            len += nc_scnprintf(buf + len, size - len, "  %s", str);
        }

        data = save;
        datalen = savelen;

        len += nc_scnprintf(buf + len, size - len, "  |");

        for (i = 0; datalen != 0 && i < 16; data++, datalen--, i++) {
            c = (unsigned char)(isprint(*data) ? *data : '.');
            len += nc_scnprintf(buf + len, size - len, "%c", c);
        }
        len += nc_scnprintf(buf + len, size - len, "|\n");

        off += 16;
    }

    n = nc_write(l->fd, buf, len);
    if (n < 0) {
        l->nerror++;
    }

    errno = errno_save;
}

void
_log_safe(const char *fmt, ...)
{
    struct logger *l = &logger;
    int len, size, errno_save;
    char buf[LOG_MAX_LEN];
    va_list args;
    ssize_t n;

    if (l->fd < 0) {
        return;
    }

    errno_save = errno;
    len = 0;            /* length of output buffer */
    size = LOG_MAX_LEN; /* size of output buffer */

    len += safe_snprintf(buf + len, size - len, "[........................] ");

    va_start(args, fmt);
    len += safe_vsnprintf(buf + len, size - len, fmt, args);
    va_end(args);

    buf[len++] = '\n';

    n = nc_write(l->fd, buf, len);
    if (n < 0) {
        l->nerror++;
    }

    errno = errno_save;
}

void
_log_stderr_safe(const char *fmt, ...)
{
    struct logger *l = &logger;
    int len, size, errno_save;
    char buf[LOG_MAX_LEN];
    va_list args;
    ssize_t n;


    errno_save = errno;
    len = 0;            /* length of output buffer */
    size = LOG_MAX_LEN; /* size of output buffer */

    len += safe_snprintf(buf + len, size - len, "[........................] ");

    va_start(args, fmt);
    len += safe_vsnprintf(buf + len, size - len, fmt, args);
    va_end(args);

    buf[len++] = '\n';

    n = nc_write(STDERR_FILENO, buf, len);
    if (n < 0) {
        l->nerror++;
    }

    errno = errno_save;
}
