// tiny.c - A very simple HTTP server

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <linux/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include <time.h>
#include <locale.h>
#include <ctype.h>
#include "mynetlib.h"

#define MAXLINE 1024
#define MAXHOST 100
#define MAXPORT 100

#define BASEDIR "./wwwroot"
#define DEFAULT_FILENAME "index.html"
#define LOG_FILE BASEDIR "/access.log"

// 限流配置：同一IP在10秒内最多20次请求
#define RATE_LIMIT_WINDOW_SECS 10    // 时间窗口（秒）
#define RATE_LIMIT_MAX_REQUESTS 20   // 每个窗口最大请求数
#define RATE_LIMIT_MAX_IPS 100       // 最多跟踪多少个IP（防止内存增长）

extern char **environ;
int verbose = 0; // TODO: add command line flag

// 服务器统计信息
static long long totalRequests = 0;
static long long count2xx = 0;
static long long count4xx = 0;
static long long count5xx = 0;
static time_t lastRequestTime = 0;
static time_t startTime = 0;

// 限流记录：每个IP的请求计数和时间窗口
typedef struct
{
    char ip[MAXHOST];      // 客户端IP
    int count;              // 当前窗口内的请求数
    time_t windowStart;     // 当前时间窗口的开始时间
} rate_limit_entry_t;

// 限流表：固定大小数组，简化实现
// 策略：线性搜索，过期清理，满了就替换最旧的记录
static rate_limit_entry_t rateLimitTable[RATE_LIMIT_MAX_IPS];
static int rateLimitCount = 0;  // 当前已使用的条目数

// 服务器配置
typedef struct
{
    int port;
    int enable_access_log;  // 1=开启, 0=关闭
} server_config_t;

static server_config_t serverConfig;

// 简化版 JSON 解析器（只解析我们需要的字段）
// 支持格式：
// {
//   "port": 8080,
//   "enable_access_log": true
// }
// 注意：这是一个极简实现，不支持：
// - 嵌套对象
// - 数组
// - 字符串值（除了 key）
// - 转义字符
// - 注释

static void skipWhitespace(const char **p)
{
    while (**p && (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r'))
        (*p)++;
}

// 解析字符串 key，返回 1 成功，0 失败
// 期望格式："key"
static int parseJsonString(const char **p, char *buf, int bufSize)
{
    if (**p != '"')
        return 0;
    (*p)++;

    int i = 0;
    while (**p && **p != '"' && i < bufSize - 1)
    {
        buf[i++] = **p;
        (*p)++;
    }
    buf[i] = '\0';

    if (**p != '"')
        return 0;
    (*p)++;
    return 1;
}

// 解析布尔值，返回 1=成功，0=失败
static int parseJsonBool(const char **p, int *result)
{
    if (strncmp(*p, "true", 4) == 0)
    {
        *p += 4;
        *result = 1;
        return 1;
    }
    if (strncmp(*p, "false", 5) == 0)
    {
        *p += 5;
        *result = 0;
        return 1;
    }
    return 0;
}

// 解析整数，返回 1=成功，0=失败
static int parseJsonInt(const char **p, int *result)
{
    int sign = 1;
    if (**p == '-')
    {
        sign = -1;
        (*p)++;
    }

    if (!isdigit(**p))
        return 0;

    int val = 0;
    while (isdigit(**p))
    {
        val = val * 10 + (**p - '0');
        (*p)++;
    }

    *result = sign * val;
    return 1;
}

// 从文件读取并解析配置
// 返回 0=成功，-1=失败
static int loadConfigFromFile(const char *filepath, server_config_t *cfg)
{
    FILE *f = fopen(filepath, "r");
    if (f == NULL)
    {
        fprintf(stderr, "Error: cannot open config file: %s (%s)\n",
                filepath, strerror(errno));
        return -1;
    }

    // 读取整个文件到内存（简化实现，假设配置文件很小）
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fileSize <= 0 || fileSize > 1024 * 10)  // 最大 10KB
    {
        fprintf(stderr, "Error: config file is too large or empty\n");
        fclose(f);
        return -1;
    }

    char *content = malloc(fileSize + 1);
    if (content == NULL)
    {
        perror("malloc");
        fclose(f);
        return -1;
    }

    size_t readSize = fread(content, 1, fileSize, f);
    content[readSize] = '\0';
    fclose(f);

    // 初始化配置标记（用于检测必填字段）
    int hasPort = 0;
    int hasEnableLog = 0;

    // 解析 JSON
    const char *p = content;
    skipWhitespace(&p);

    if (*p != '{')
    {
        fprintf(stderr, "Error: config file must be a JSON object (starts with '{')\n");
        free(content);
        return -1;
    }
    p++;

    while (1)
    {
        skipWhitespace(&p);

        if (*p == '}')
            break;

        if (*p == ',')
        {
            p++;
            skipWhitespace(&p);
        }

        // 解析 key
        char key[256];
        if (!parseJsonString(&p, key, sizeof(key)))
        {
            fprintf(stderr, "Error: invalid JSON key near position %ld\n", p - content);
            free(content);
            return -1;
        }

        skipWhitespace(&p);
        if (*p != ':')
        {
            fprintf(stderr, "Error: expected ':' after key '%s'\n", key);
            free(content);
            return -1;
        }
        p++;
        skipWhitespace(&p);

        // 解析 value
        if (strcmp(key, "port") == 0)
        {
            int port;
            if (!parseJsonInt(&p, &port))
            {
                fprintf(stderr, "Error: 'port' must be an integer\n");
                free(content);
                return -1;
            }
            if (port < 1 || port > 65535)
            {
                fprintf(stderr, "Error: 'port' must be between 1 and 65535\n");
                free(content);
                return -1;
            }
            cfg->port = port;
            hasPort = 1;
        }
        else if (strcmp(key, "enable_access_log") == 0)
        {
            int val;
            if (!parseJsonBool(&p, &val))
            {
                fprintf(stderr, "Error: 'enable_access_log' must be true or false\n");
                free(content);
                return -1;
            }
            cfg->enable_access_log = val;
            hasEnableLog = 1;
        }
        else
        {
            // 忽略未知字段，但跳过它的值
            if (*p == '"')
            {
                char dummy[256];
                parseJsonString(&p, dummy, sizeof(dummy));
            }
            else if (isdigit(*p) || *p == '-')
            {
                int dummy;
                parseJsonInt(&p, &dummy);
            }
            else if (*p == 't' || *p == 'f')
            {
                int dummy;
                parseJsonBool(&p, &dummy);
            }
            else
            {
                // 尝试跳过这个值（简化处理）
                while (*p && *p != ',' && *p != '}')
                    p++;
            }
        }
    }

    free(content);

    // 检查必填字段
    if (!hasPort)
    {
        fprintf(stderr, "Error: missing required field 'port' in config file\n");
        return -1;
    }
    if (!hasEnableLog)
    {
        fprintf(stderr, "Error: missing required field 'enable_access_log' in config file\n");
        return -1;
    }

    return 0;
}

// 请求上下文，用于在函数间传递请求信息
typedef struct
{
    const char *clientIp;
    const char *method;
    const char *url;
    int statusCode;
    long long bytesSent;
} request_ctx_t;

// 更新统计信息
void updateStats(int statusCode)
{
    totalRequests++;
    lastRequestTime = time(NULL);

    if (statusCode >= 200 && statusCode < 300)
    {
        count2xx++;
    }
    else if (statusCode >= 400 && statusCode < 500)
    {
        count4xx++;
    }
    else if (statusCode >= 500 && statusCode < 600)
    {
        count5xx++;
    }
}

// 写访问日志到文件，失败时仅输出到stderr
void writeAccessLog(const request_ctx_t *ctx)
{
    // 检查配置：如果关闭了访问日志，直接返回
    if (!serverConfig.enable_access_log)
        return;

    FILE *logFile = fopen(LOG_FILE, "a");
    if (logFile == NULL)
    {
        fprintf(stderr, "Warning: failed to open access log file: %s\n", LOG_FILE);
        return;
    }

    time_t now = time(NULL);
    char timeStr[64];
    struct tm *tmInfo = localtime(&now);
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", tmInfo);

    fprintf(logFile, "[%s] %s %s %s %d %lld\n",
            timeStr, ctx->clientIp, ctx->method, ctx->url, ctx->statusCode, ctx->bytesSent);

    fclose(logFile);
}

// 限流检查：返回 1 表示允许通过，0 表示超限
// 思路：
// 1. 先清理所有过期的记录（窗口已结束）
// 2. 查找该IP的记录
// 3. 如果存在且在窗口内：检查计数是否超限
// 4. 如果不存在或已过期：创建新记录（数组满了就替换最旧的）
static int checkRateLimit(const char *clientIp)
{
    time_t now = time(NULL);
    int i;

    // 步骤1：清理过期记录，同时查找该IP是否存在
    int foundIndex = -1;
    int oldestIndex = 0;     // 最旧记录的索引（用于替换）
    time_t oldestTime = now;

    for (i = 0; i < rateLimitCount; i++)
    {
        // 检查是否过期：当前时间 - 窗口开始时间 > 窗口大小
        if (now - rateLimitTable[i].windowStart > RATE_LIMIT_WINDOW_SECS)
        {
            // 过期记录：如果是最后一个，直接减少计数
            // 为简化实现，我们不立即删除，而是在需要时重用
            // 这里先标记，继续查找
        }

        // 记录最旧的时间（用于替换策略）
        if (rateLimitTable[i].windowStart < oldestTime)
        {
            oldestTime = rateLimitTable[i].windowStart;
            oldestIndex = i;
        }

        // 查找是否是目标IP
        if (strcmp(rateLimitTable[i].ip, clientIp) == 0)
        {
            foundIndex = i;
        }
    }

    // 步骤2：处理找到的IP记录
    if (foundIndex != -1)
    {
        // 检查该记录的窗口是否已过期
        if (now - rateLimitTable[foundIndex].windowStart > RATE_LIMIT_WINDOW_SECS)
        {
            // 窗口已过期：重置计数和时间
            rateLimitTable[foundIndex].count = 1;
            rateLimitTable[foundIndex].windowStart = now;
            return 1;  // 允许通过
        }
        else
        {
            // 窗口内：检查是否超限
            if (rateLimitTable[foundIndex].count >= RATE_LIMIT_MAX_REQUESTS)
            {
                return 0;  // 超限，拒绝
            }
            else
            {
                rateLimitTable[foundIndex].count++;
                return 1;  // 允许通过
            }
        }
    }

    // 步骤3：IP不存在，需要创建新记录
    // 先查找是否有过期的记录可以重用
    for (i = 0; i < rateLimitCount; i++)
    {
        if (now - rateLimitTable[i].windowStart > RATE_LIMIT_WINDOW_SECS)
        {
            // 重用过期的记录
            strncpy(rateLimitTable[i].ip, clientIp, MAXHOST - 1);
            rateLimitTable[i].ip[MAXHOST - 1] = '\0';
            rateLimitTable[i].count = 1;
            rateLimitTable[i].windowStart = now;
            return 1;
        }
    }

    // 步骤4：没有过期记录，检查数组是否已满
    if (rateLimitCount < RATE_LIMIT_MAX_IPS)
    {
        // 数组未满，添加新记录
        strncpy(rateLimitTable[rateLimitCount].ip, clientIp, MAXHOST - 1);
        rateLimitTable[rateLimitCount].ip[MAXHOST - 1] = '\0';
        rateLimitTable[rateLimitCount].count = 1;
        rateLimitTable[rateLimitCount].windowStart = now;
        rateLimitCount++;
        return 1;
    }

    // 步骤5：数组已满，替换最旧的记录（LRU简化版）
    // 注意：这是一个妥协策略，可能会导致少数IP的计数被错误重置
    // 但在内存受限的情况下，这是合理的权衡
    strncpy(rateLimitTable[oldestIndex].ip, clientIp, MAXHOST - 1);
    rateLimitTable[oldestIndex].ip[MAXHOST - 1] = '\0';
    rateLimitTable[oldestIndex].count = 1;
    rateLimitTable[oldestIndex].windowStart = now;
    return 1;
}

// 返回429 Too Many Requests响应
static void rateLimitResponse(int client, request_ctx_t *ctx)
{
    ctx->statusCode = 429;
    ctx->bytesSent = 0;

    char *message = "Too Many Requests - Rate limit exceeded\n";
    char buf[MAXLINE];

    snprintf(buf, MAXLINE, "HTTP/1.1 429 Too Many Requests\r\n");
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    snprintf(buf, MAXLINE, "Content-Type: text/plain\r\n");
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    snprintf(buf, MAXLINE, "Content-Length: %zu\r\n\r\n", strlen(message));
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    ctx->bytesSent += sendBytes(client, message, strlen(message));
}

// 手动解析 RFC1123 日期格式："Wed, 22 Apr 2026 13:37:46 GMT"
// 返回 time_t，失败返回 -1
static time_t parseHttpDate(const char *dateStr)
{
    printf("[DEBUG] parseHttpDate: input=\"%s\"\n", dateStr);

    struct tm tm;
    memset(&tm, 0, sizeof(tm));

    int day, year, hour, min, sec;
    char monthStr[4]; // 3字母月份 + 结束符

    // 格式字符串：
    // %*[^ ] - 跳过所有非空格字符（跳过 "Wed"）
    // %*[, ] - 跳过逗号和空格
    // %d - 读取日期
    // %3s - 读取3字符月份
    // %d - 读取年份
    // %d:%d:%d - 读取时:分:秒
    int result = sscanf(dateStr, "%*[^ ]%*[, ]%d %3s %d %d:%d:%d",
                        &day, monthStr, &year, &hour, &min, &sec);

    printf("[DEBUG] parseHttpDate: sscanf returned %d, day=%d, month=%s, year=%d, time=%d:%d:%d\n",
           result, day, monthStr, year, hour, min, sec);

    if (result != 6)
        return -1;

    // 解析月份
    const char *monthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int month = -1;
    for (int i = 0; i < 12; i++)
    {
        if (strcmp(monthStr, monthNames[i]) == 0)
        {
            month = i;
            break;
        }
    }
    if (month == -1)
    {
        printf("[DEBUG] parseHttpDate: month '%s' not found\n", monthStr);
        return -1;
    }

    // 验证范围
    if (year < 1970 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59)
        return -1;

    // 填充 tm 结构
    tm.tm_year = year - 1900;
    tm.tm_mon = month;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    tm.tm_isdst = 0; // 不考虑夏令时

    // 转换为 time_t（GMT 时间）
    time_t parsed = timegm(&tm);
    printf("[DEBUG] parseHttpDate: result=%lld\n", (long long)parsed);
    return parsed;
}

// 手动格式化 time_t 为 RFC1123 日期格式
// 格式: "Wed, 21 Oct 2015 07:28:00 GMT"
static void formatHttpDate(time_t t, char *buf, size_t bufSize)
{
    if (bufSize < 30) // 最小需要约 29 个字符
        return;

    struct tm *gmtTime = gmtime(&t);

    // 星期名称（tm_wday: 0=Sunday, 1=Monday...）
    const char *dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    // 月份名称（tm_mon: 0=January...）
    const char *monthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    int year = gmtTime->tm_year + 1900;
    int day = gmtTime->tm_mday;
    int hour = gmtTime->tm_hour;
    int min = gmtTime->tm_min;
    int sec = gmtTime->tm_sec;

    snprintf(buf, bufSize, "%s, %02d %s %d %02d:%02d:%02d GMT",
             dayNames[gmtTime->tm_wday],
             day,
             monthNames[gmtTime->tm_mon],
             year,
             hour, min, sec);
}

long readRequestHeaders(buffered_reader_t *pbr, char *contentType, size_t contentTypeSize, time_t *ifModifiedSince)
{
    long contentLength = 0;
    char headerLine[MAXLINE];
    *ifModifiedSince = -1; // 默认为 -1 表示没有这个头

    for (int i = 0;; i++)
    {
        ssize_t headerLength = bufReadLine(pbr, headerLine, MAXLINE);
        if (headerLength == -1)
        {
            perror("bufReadLine");
            break;
        }
        else if (headerLength == 0)
        {
            printf("client closed connection\n");
            break;
        }
        if (strncmp(headerLine, "\r\n", headerLength) == 0)
        {
            break;
        }
        if (verbose)
        {
            printf("Header #%d: %.*s", i, (int)headerLength, headerLine);
        }
        if (strncasecmp(headerLine, "Content-Length:", 15) == 0)
        {
            char *split = strchr(headerLine, ':');
            contentLength = atol(split + 1);
        }
        if (strncasecmp(headerLine, "Content-Type:", 13) == 0)
        {
            char *split = strchr(headerLine, ':');
            split++;              // skip ':'
            while (*split == ' ') // skip whitespaces
                split++;
            size_t actualSize = headerLength - (split - headerLine) - 2; // -2 for CRLF
            snprintf(contentType, contentTypeSize, "%.*s", (int)actualSize, split);
        }
        if (strncasecmp(headerLine, "If-Modified-Since:", 19) == 0)
        {
            char *split = strchr(headerLine, ':');
            split++;              // skip ':'
            while (*split == ' ') // skip whitespaces
                split++;
            // 去掉末尾的 \r\n
            char *end = split;
            while (*end && *end != '\r' && *end != '\n')
                end++;
            *end = '\0';
            printf("[DEBUG] readRequestHeaders: found If-Modified-Since, value=\"%s\"\n", split);
            *ifModifiedSince = parseHttpDate(split);
            printf("[DEBUG] readRequestHeaders: ifModifiedAfter parse = %lld\n", (long long)*ifModifiedSince);
        }
    }
    return contentLength;
}

void errorResponse(int client, request_ctx_t *ctx, int statusCode, char *shortMessage, char *longMessage)
{
    ctx->statusCode = statusCode;
    ctx->bytesSent = 0;

    char buf[MAXLINE];
    snprintf(buf, MAXLINE, "HTTP/1.1 %d %s\r\n", statusCode, shortMessage);
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    snprintf(buf, MAXLINE, "Content-Type: text/plain\r\n");
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    if (longMessage != NULL)
    {
        snprintf(buf, MAXLINE, "Content-Length: %zu\r\n\r\n", strlen(longMessage));
        ctx->bytesSent += sendBytes(client, buf, strlen(buf));
        ctx->bytesSent += sendBytes(client, longMessage, strlen(longMessage));
    }
    else
    {
        char defaultMessage[MAXLINE];
        snprintf(defaultMessage, MAXLINE, "%d %s", statusCode, shortMessage);
        snprintf(buf, MAXLINE, "Content-Length: %zu\r\n\r\n", strlen(defaultMessage));
        ctx->bytesSent += sendBytes(client, buf, strlen(buf));
        ctx->bytesSent += sendBytes(client, defaultMessage, strlen(defaultMessage));
    }
}

int parseURI(char *uri, char *path, char *args)
{
    char *splitPos = strchr(uri, '?');
    // TODO: check path traversal (../) with realpath
    // TODO: use snprintf where possible
    strcpy(path, BASEDIR);
    if (splitPos == NULL)
    {
        strcat(path, uri);
        strcpy(args, "");
    }
    else
    {
        size_t pathLength = splitPos - uri;
        strncat(path, uri, pathLength);
        strcpy(args, splitPos + 1);
    }
    size_t len = strlen(path);
    if (path[len - 1] == '/')
    {
        strcat(path, DEFAULT_FILENAME);
    }
    int isDynamic = strstr(path, "/cgi-bin/") != NULL;
    return isDynamic;
}

char *getMimeTypeString(char *path)
{
    char *extension = strrchr(path, '.');
    if (extension == NULL)
    {
        return "text/plain";
    }
    if (strcmp(extension, ".html") == 0 || strcmp(extension, ".htm") == 0)
    {
        return "text/html";
    }
    if (strcmp(extension, ".jpeg") == 0 || strcmp(extension, ".jpg") == 0)
    {
        return "image/jpeg";
    }
    if (strcmp(extension, ".gif") == 0)
    {
        return "image/gif";
    }
    if (strcmp(extension, ".png") == 0)
    {
        return "image/png";
    }
    if (strcmp(extension, ".mp4") == 0)
    {
        return "video/mp4";
    }
    return "text/plain";
}

int checkResource(int client, request_ctx_t *ctx, char *path, mode_t flag, long *psize, time_t *pmtime)
{
    struct stat fileinfo;
    if (stat(path, &fileinfo) == -1)
    {
        if (errno == ENOENT)
        {
            errorResponse(client, ctx, 404, "Not Found", NULL);
            return -1;
        }
        perror("stat");
        errorResponse(client, ctx, 500, "Internal Server Error", NULL);
        return -1;
    }
    if (!S_ISREG(fileinfo.st_mode) || (fileinfo.st_mode & flag) == 0)
    {
        errorResponse(client, ctx, 403, "Forbidden", NULL);
        return -1;
    }
    if (psize != NULL)
    {
        *psize = fileinfo.st_size;
    }
    if (pmtime != NULL)
    {
        *pmtime = fileinfo.st_mtime;
    }
    return 0;
}

// 处理 /server-status 请求，返回服务器统计信息
void serveServerStatus(int client, request_ctx_t *ctx)
{
    ctx->statusCode = 200;
    ctx->bytesSent = 0;

    char buf[MAXLINE];
    char response[2048];
    int pos = 0;

    pos += snprintf(response + pos, sizeof(response) - pos, "Server Status\n");
    pos += snprintf(response + pos, sizeof(response) - pos, "=============\n\n");

    char startTimeStr[64];
    struct tm *tmStart = localtime(&startTime);
    strftime(startTimeStr, sizeof(startTimeStr), "%Y-%m-%d %H:%M:%S", tmStart);
    pos += snprintf(response + pos, sizeof(response) - pos, "Start Time: %s\n", startTimeStr);

    pos += snprintf(response + pos, sizeof(response) - pos, "Total Requests: %lld\n", totalRequests);
    pos += snprintf(response + pos, sizeof(response) - pos, "2xx Responses: %lld\n", count2xx);
    pos += snprintf(response + pos, sizeof(response) - pos, "4xx Responses: %lld\n", count4xx);
    pos += snprintf(response + pos, sizeof(response) - pos, "5xx Responses: %lld\n", count5xx);

    if (lastRequestTime > 0)
    {
        char lastTimeStr[64];
        struct tm *tmLast = localtime(&lastRequestTime);
        strftime(lastTimeStr, sizeof(lastTimeStr), "%Y-%m-%d %H:%M:%S", tmLast);
        pos += snprintf(response + pos, sizeof(response) - pos, "Last Request: %s\n", lastTimeStr);
    }
    else
    {
        pos += snprintf(response + pos, sizeof(response) - pos, "Last Request: None\n");
    }

    snprintf(buf, MAXLINE, "HTTP/1.1 200 OK\r\n");
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    snprintf(buf, MAXLINE, "Content-Type: text/plain\r\n");
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    snprintf(buf, MAXLINE, "Content-Length: %zu\r\n\r\n", pos);
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    ctx->bytesSent += sendBytes(client, response, pos);
}

void serveHeadRequest(int client, request_ctx_t *ctx, char *path, int isDynamic, time_t ifModifiedSince)
{
    printf("serving head request: %s\n", path);
    long size;
    time_t mtime;

    if (checkResource(client, ctx, path, S_IRUSR, &size, &mtime) != 0)
    {
        return;
    }

    // 动态内容不做缓存处理
    if (isDynamic)
    {
        ctx->statusCode = 200;
        ctx->bytesSent = 0;
        char buf[MAXLINE];
        snprintf(buf, MAXLINE, "HTTP/1.1 200 OK\r\n");
        ctx->bytesSent += sendBytes(client, buf, strlen(buf));
        // unknown size: payload headers may be omitted according to RFC 7231
        ctx->bytesSent += sendBytes(client, "\r\n", 2);
        return;
    }

    // 静态资源：检查缓存
    // 如果 If-Modified-Since 存在且文件未修改，返回 304
    if (ifModifiedSince != -1 && mtime <= ifModifiedSince)
    {
        ctx->statusCode = 304;
        ctx->bytesSent = 0;
        char buf[MAXLINE];
        char lastModifiedStr[64];
        formatHttpDate(mtime, lastModifiedStr, sizeof(lastModifiedStr));

        snprintf(buf, MAXLINE, "HTTP/1.1 304 Not Modified\r\n");
        ctx->bytesSent += sendBytes(client, buf, strlen(buf));
        snprintf(buf, MAXLINE, "Last-Modified: %s\r\n", lastModifiedStr);
        ctx->bytesSent += sendBytes(client, buf, strlen(buf));
        snprintf(buf, MAXLINE, "\r\n");
        ctx->bytesSent += sendBytes(client, buf, strlen(buf));
        return;
    }

    // 文件已修改或没有 If-Modified-Since，返回 200
    ctx->statusCode = 200;
    ctx->bytesSent = 0;
    char buf[MAXLINE];
    char lastModifiedStr[64];
    formatHttpDate(mtime, lastModifiedStr, sizeof(lastModifiedStr));

    snprintf(buf, MAXLINE, "HTTP/1.1 200 OK\r\n");
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    snprintf(buf, MAXLINE, "Content-Type: %s\r\n", getMimeTypeString(path));
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    snprintf(buf, MAXLINE, "Last-Modified: %s\r\n", lastModifiedStr);
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    snprintf(buf, MAXLINE, "Content-Length: %zu\r\n\r\n", size);
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
}

void serveStatic(int client, request_ctx_t *ctx, char *path, time_t ifModifiedSince)
{
    printf("serving static resource: %s\n", path);
    printf("[DEBUG] serveStatic: ifModifiedSince=%lld\n", (long long)ifModifiedSince);

    long size;
    time_t mtime;

    if (checkResource(client, ctx, path, S_IRUSR, &size, &mtime) != 0)
    {
        return;
    }

    printf("[DEBUG] serveStatic: file mtime=%lld\n", (long long)mtime);
    printf("[DEBUG] serveStatic: mtime <= ifModifiedSince? %d\n", mtime <= ifModifiedSince);

    // 检查缓存：如果 If-Modified-Since 存在且文件未修改，返回 304
    if (ifModifiedSince != -1 && mtime <= ifModifiedSince)
    {
        printf("[DEBUG] serveStatic: returning 304 Not Modified\n");
        ctx->statusCode = 304;
        ctx->bytesSent = 0;
        char buf[MAXLINE];
        char lastModifiedStr[64];
        formatHttpDate(mtime, lastModifiedStr, sizeof(lastModifiedStr));

        snprintf(buf, MAXLINE, "HTTP/1.1 304 Not Modified\r\n");
        ctx->bytesSent += sendBytes(client, buf, strlen(buf));
        snprintf(buf, MAXLINE, "Last-Modified: %s\r\n", lastModifiedStr);
        ctx->bytesSent += sendBytes(client, buf, strlen(buf));
        snprintf(buf, MAXLINE, "\r\n");
        ctx->bytesSent += sendBytes(client, buf, strlen(buf));
        return;
    }

    // 文件已修改或没有 If-Modified-Since，返回 200 并发送内容
    ctx->statusCode = 200;
    ctx->bytesSent = 0;

    // TODO: handle large files by reading in blocks
    FILE *file = fopen(path, "rb");
    char *content = malloc(size);
    fread(content, size, 1, file);
    fclose(file);

    char buf[MAXLINE];
    char lastModifiedStr[64];
    formatHttpDate(mtime, lastModifiedStr, sizeof(lastModifiedStr));

    snprintf(buf, MAXLINE, "HTTP/1.1 200 OK\r\n");
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    snprintf(buf, MAXLINE, "Content-Type: %s\r\n", getMimeTypeString(path));
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    snprintf(buf, MAXLINE, "Last-Modified: %s\r\n", lastModifiedStr);
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    snprintf(buf, MAXLINE, "Content-Length: %zu\r\n\r\n", size);
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    ctx->bytesSent += sendBytes(client, content, size);

    free(content);
}

void serveDynamic(int client, request_ctx_t *ctx, char *path, char *args, char *method, long inputLength, char *inputType, char *inputBuffer)
{
    printf("serving dynamic resource: %s with args: %s\n", path, args);
    // 动态内容不做缓存，checkResource 的 mtime 参数传 NULL
    if (checkResource(client, ctx, path, S_IXUSR, NULL, NULL) != 0)
    {
        return;
    }

    // set a pipe to allow for input redirection (stdin) to child process
    // parent will write the buffered input to it
    int pipefd[2] = {-1, -1};
    if (inputLength > 0 && inputBuffer != NULL)
    {
        if (verbose)
        {
            printf("input (%ld length): %.*s\n", inputLength, (int)inputLength, inputBuffer);
        }
        pipe(pipefd);
    }

    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork");
        errorResponse(client, ctx, 500, "Internal Server Error", NULL);
        return;
    }

    // 动态内容：状态码设为200，字节数记为0（子进程直接输出，无法精确统计）
    ctx->statusCode = 200;
    ctx->bytesSent = 0;

    char buf[MAXLINE];
    snprintf(buf, MAXLINE, "HTTP/1.1 200 OK\r\n");
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));

    if (pid == 0) // child
    {
        char *empty[] = {NULL};
        if (pipefd[1] != -1)
        {
            close(pipefd[1]); // child doesn't need the write end of the pipe
        }
        if (pipefd[0] != -1)
        {
            if (verbose)
                printf("redirecting stdin to %d\n", pipefd[0]);
            dup2(pipefd[0], STDIN_FILENO);
            close(pipefd[0]);
        }
        dup2(client, STDOUT_FILENO);
        setenv("QUERY_STRING", args, 1);
        setenv("REQUEST_METHOD", method, 1);
        char inputLengthString[20];
        sprintf(inputLengthString, "%ld", inputLength);
        setenv("CONTENT_LENGTH", inputLengthString, 1);
        setenv("CONTENT_TYPE", inputType, 1);
        execve(path, empty, environ);
        perror("execve"); // shouldn't return
        return;
    }
    else // parent
    {
        if (pipefd[0] != -1)
        {
            close(pipefd[0]); // parent doesn't need the read end of the pipe
        }
        if (pipefd[1] != -1)
        {
            if (verbose)
                printf("writing to %d\n", pipefd[1]);
            if (sendBytes(pipefd[1], inputBuffer, inputLength) == -1)
            {
                // TODO: kill child and return error to client
                perror("sendBytes");
            }
            close(pipefd[1]);
        }
        printf("spawned child process pid=%d\n", pid);
    }
}

void handleClient(int client, const char *clientIp)
{
    char reqLine[MAXLINE];
    char method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    request_ctx_t ctx;
    char *inputBuffer = NULL;
    time_t ifModifiedSince = -1; // 缓存相关：-1 表示没有 If-Modified-Since 头

    // 初始化请求上下文
    ctx.clientIp = clientIp;
    ctx.method = method;
    ctx.url = uri;
    ctx.statusCode = 0;
    ctx.bytesSent = 0;

    buffered_reader_t br;
    bufReaderInit(&br, client);
    ssize_t reqLineLength = bufReadLine(&br, reqLine, MAXLINE);
    if (reqLineLength == -1)
    {
        perror("bufReadLine");
        return;
    }
    else if (reqLineLength == 0)
    {
        printf("client closed connection\n");
        return;
    }

    sscanf(reqLine, "%s %s %s", method, uri, version);
    if (verbose)
    {
        printf("Request - Method: %s URL: %s Version: %s\n", method, uri, version);
    }

    // 限流检查：在解析请求后、实际处理前
    // 这样可以记录完整的 method 和 url 到日志
    if (!checkRateLimit(clientIp))
    {
        rateLimitResponse(client, &ctx);
        goto log_and_exit;
    }

    if (strcmp(version, "HTTP/1.1") != 0)
    {
        errorResponse(client, &ctx, 505, "HTTP Version Not Supported", NULL);
        goto log_and_exit;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0 && strcmp(method, "POST") != 0)
    {
        errorResponse(client, &ctx, 501, "Not Implemented", NULL);
        goto log_and_exit;
    }

    // 特殊处理 /server-status 接口
    if (strcmp(method, "GET") == 0 && strcmp(uri, "/server-status") == 0)
    {
        serveServerStatus(client, &ctx);
        goto log_and_exit;
    }

    char contentType[MAXLINE];
    strcpy(contentType, "");
    // 读取请求头，包括 If-Modified-Since
    long contentLength = readRequestHeaders(&br, contentType, sizeof(contentType), &ifModifiedSince);
    char path[MAXLINE], args[MAXLINE];
    int isDynamic = parseURI(uri, path, args);
    if (verbose)
    {
        printf("isDynamic: %d path: %s args: %s\n", isDynamic, path, args);
    }

    if (strcmp(method, "HEAD") == 0)
    {
        serveHeadRequest(client, &ctx, path, isDynamic, ifModifiedSince);
    }
    else if (!isDynamic)
    {
        // 只有 GET 请求走静态资源缓存逻辑，POST 不走
        serveStatic(client, &ctx, path, ifModifiedSince);
    }
    else
    {
        inputBuffer = NULL;
        if (strcmp(method, "POST") == 0 && contentLength > 0)
        {
            if (verbose)
            {
                printf("contentLength=%ld\n", contentLength);
                printf("contentType=%s\n", contentType);
            }
            inputBuffer = malloc(contentLength);
            if (inputBuffer == NULL)
            {
                perror("malloc");
                errorResponse(client, &ctx, 500, "Internal Server Error", NULL);
                goto log_and_exit;
            }
            if (bufReadBytes(&br, inputBuffer, contentLength) == -1)
            {
                perror("bufReadBytes");
                errorResponse(client, &ctx, 500, "Internal Server Error", NULL);
                goto log_and_exit;
            }
        }
        serveDynamic(client, &ctx, path, args, method, contentLength, contentType, inputBuffer);
    }

log_and_exit:
    // 更新统计信息并写日志
    if (ctx.statusCode != 0)
    {
        updateStats(ctx.statusCode);
        writeAccessLog(&ctx);
    }
    free(inputBuffer);
}

void sigchldHandler(int sig)
{
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0)
    {
        if (WIFEXITED(status))
        {
            printf("child pid=%d exit status=%d\n", pid, WEXITSTATUS(status));
        }
        if (WIFSIGNALED(status))
        {
            printf("child pid=%d terminated by signal=%d\n", pid, WTERMSIG(status));
        }
    }
}

typedef void handler_t(int);

handler_t *Signal(int signum, handler_t *handler)
{
    struct sigaction action, old_action;

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        perror("sigaction");
    return (old_action.sa_handler);
}

int main(int argc, char *argv[])
{
    // 设置 locale 为 "C"，确保 strptime/strftime 正确解析英文日期
    setlocale(LC_TIME, "C");

    // 初始化服务器启动时间
    startTime = time(NULL);

    // 初始化默认配置
    serverConfig.port = 0;
    serverConfig.enable_access_log = 1;  // 默认开启日志

    // 解析命令行参数
    if (argc == 2)
    {
        // 旧方式：./tiny 8080
        // 检查是否是数字（port）还是 --config
        char *endptr;
        long portNum = strtol(argv[1], &endptr, 10);

        if (*endptr == '\0' && portNum > 0 && portNum <= 65535)
        {
            // 是 port 数字
            serverConfig.port = (int)portNum;
            serverConfig.enable_access_log = 1;  // 默认开启
        }
        else
        {
            fprintf(stderr, "usage: %s <port>\n", argv[0]);
            fprintf(stderr, "   or: %s --config <config_file>\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }
    else if (argc == 3)
    {
        // 新方式：./tiny --config ./server.json
        if (strcmp(argv[1], "--config") != 0)
        {
            fprintf(stderr, "usage: %s <port>\n", argv[0]);
            fprintf(stderr, "   or: %s --config <config_file>\n", argv[0]);
            exit(EXIT_FAILURE);
        }

        // 从配置文件加载
        if (loadConfigFromFile(argv[2], &serverConfig) != 0)
        {
            exit(EXIT_FAILURE);
        }
    }
    else
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        fprintf(stderr, "   or: %s --config <config_file>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // 将 port 转换为字符串（port 传递给 serverListen 需要字符串）
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", serverConfig.port);

    int listenSocket = serverListen(portStr);
    if (listenSocket == -1)
    {
        fprintf(stderr, "cant't listen on port %d\n", serverConfig.port);
        exit(EXIT_FAILURE);
    }

    Signal(SIGCHLD, sigchldHandler);

    // avoid crashing the server if trying to write to client that prematurely closed the connection
    Signal(SIGPIPE, SIG_IGN);

    char clientHost[MAXHOST];
    char clientPort[MAXPORT];

    printf("listening on port %d\n", serverConfig.port);
    printf("config: access_log=%s\n", serverConfig.enable_access_log ? "enabled" : "disabled");
    while (1)
    {
        struct sockaddr_storage address;
        socklen_t addressLength = sizeof(address);
        int client = accept(listenSocket, (SA *)&address, &addressLength);
        if (client == -1)
        {
            perror("accept");
            continue;
        }
        int result = getnameinfo((SA *)&address, addressLength, clientHost, MAXHOST, clientPort, MAXPORT, NI_NUMERICSERV);
        if (result != 0)
        {
            fprintf(stderr, "error getting name information: %s\n", gai_strerror(result));
            strcpy(clientHost, "unknown");
        }
        else
        {
            printf("client connected %s:%s\n", clientHost, clientPort);
        }
        handleClient(client, clientHost);
        close(client);
    }

    // unreachable
    close(listenSocket);
    return 0;
}