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
#include "mynetlib.h"

#define MAXLINE 1024
#define MAXHOST 100
#define MAXPORT 100

#define BASEDIR "./wwwroot"
#define DEFAULT_FILENAME "index.html"
#define LOG_FILE BASEDIR "/access.log"

extern char **environ;
int verbose = 0; // TODO: add command line flag

// 服务器统计信息
static long long totalRequests = 0;
static long long count2xx = 0;
static long long count4xx = 0;
static long long count5xx = 0;
static time_t lastRequestTime = 0;
static time_t startTime = 0;

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

long readRequestHeaders(buffered_reader_t *pbr, char *contentType, size_t contentTypeSize)
{
    long contentLength = 0;
    char headerLine[MAXLINE];
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

int checkResource(int client, request_ctx_t *ctx, char *path, mode_t flag, long *psize)
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

void serveHeadRequest(int client, request_ctx_t *ctx, char *path, int isDynamic)
{
    printf("serving head request: %s\n", path);
    long size;
    if (checkResource(client, ctx, path, S_IRUSR, &size) != 0)
    {
        return;
    }
    ctx->statusCode = 200;
    ctx->bytesSent = 0;

    char buf[MAXLINE];
    snprintf(buf, MAXLINE, "HTTP/1.1 200 OK\r\n");
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    if (isDynamic)
    {
        // unknown size: payload headers may be omitted according to RFC 7231
        ctx->bytesSent += sendBytes(client, "\r\n", 2);
        return;
    }
    snprintf(buf, MAXLINE, "Content-Type: %s\r\n", getMimeTypeString(path));
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    snprintf(buf, MAXLINE, "Content-Length: %zu\r\n\r\n", size);
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
}

void serveStatic(int client, request_ctx_t *ctx, char *path)
{
    printf("serving static resource: %s\n", path);
    long size;
    if (checkResource(client, ctx, path, S_IRUSR, &size) != 0)
    {
        return;
    }
    ctx->statusCode = 200;
    ctx->bytesSent = 0;

    // TODO: handle large files by reading in blocks
    FILE *file = fopen(path, "rb");
    char *content = malloc(size);
    fread(content, size, 1, file);
    fclose(file);

    char buf[MAXLINE];
    snprintf(buf, MAXLINE, "HTTP/1.1 200 OK\r\n");
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    snprintf(buf, MAXLINE, "Content-Type: %s\r\n", getMimeTypeString(path));
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    snprintf(buf, MAXLINE, "Content-Length: %zu\r\n\r\n", size);
    ctx->bytesSent += sendBytes(client, buf, strlen(buf));
    ctx->bytesSent += sendBytes(client, content, size);

    free(content);
}

void serveDynamic(int client, request_ctx_t *ctx, char *path, char *args, char *method, long inputLength, char *inputType, char *inputBuffer)
{
    printf("serving dynamic resource: %s with args: %s\n", path, args);
    if (checkResource(client, ctx, path, S_IXUSR, NULL) != 0)
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
    long contentLength = readRequestHeaders(&br, contentType, sizeof(contentType));
    char path[MAXLINE], args[MAXLINE];
    int isDynamic = parseURI(uri, path, args);
    if (verbose)
    {
        printf("isDynamic: %d path: %s args: %s\n", isDynamic, path, args);
    }

    if (strcmp(method, "HEAD") == 0)
    {
        serveHeadRequest(client, &ctx, path, isDynamic);
    }
    else if (!isDynamic)
    {
        serveStatic(client, &ctx, path);
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
    // 初始化服务器启动时间
    startTime = time(NULL);

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *port = argv[1];

    int listenSocket = serverListen(port);
    if (listenSocket == -1)
    {
        fprintf(stderr, "cant't listen on port %s\n", port);
        exit(EXIT_FAILURE);
    }

    Signal(SIGCHLD, sigchldHandler);

    // avoid crashing the server if trying to write to client that prematurely closed the connection
    Signal(SIGPIPE, SIG_IGN);

    char clientHost[MAXHOST];
    char clientPort[MAXPORT];

    printf("listening on port %s\n", port);
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