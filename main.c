#include <sys/time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>

typedef void (*fs_callback)(void*);
typedef void (*callback)();
typedef struct {
    double time;
    callback c;
} Timer;

typedef struct {
    int top; 
    Timer** array;
} Stack;

typedef struct {
    int rear;
    int front;
    void** array;
} Queue;

typedef struct {
    char* path;
    void* content;
    callback c;
    pthread_t thread;
    int connection;
    int port;
} File;

typedef struct {
    int top; 
    File** array;
} FSStack;

typedef struct {
    double time;
    Stack* timers;
    Stack* microtasks;
    FSStack* fs;
    Queue* queue;
} Loop;

Loop loop;
struct timeval tv;

void enQueue(void* value) {
    if (loop.queue->front == -1) loop.queue->front = 0;
    loop.queue->rear++;
    loop.queue->array[loop.queue->rear] = value;
}
void deQueue() {
    loop.queue->front++;
    if (loop.queue->front > loop.queue->rear) {
        loop.queue->front = loop.queue->rear = -1;
    }
}

void uv_loop_init() {
    gettimeofday(&tv, NULL);
    loop.time = (tv.tv_sec) * 1000 + (tv.tv_usec) / 1000;

    Stack* s = (Stack*)malloc(sizeof(Stack)); 
    s->top = -1;
    s->array = malloc(10 * sizeof(Timer));
    loop.timers = s;

    Stack* m = (Stack*)malloc(sizeof(Stack)); 
    m->top = -1;
    m->array = malloc(10 * sizeof(Timer));
    loop.microtasks = m;

    FSStack* fs = (FSStack*)malloc(sizeof(FSStack)); 
    fs->top = -1;
    fs->array = malloc(10 * sizeof(File));
    loop.fs = fs;

    Queue* queue = (Queue*)malloc(sizeof(Queue)); 
    queue->front = -1;
    queue->rear = -1;
    queue->array = malloc(10 * sizeof(void*));
    loop.queue = queue;
}

void uv_run_timers() {
    for (int i = 0; i < loop.timers->top + 1; i++) {
        Timer* t = loop.timers->array[i];
        if (t->time < loop.time) {
            t->c();

            if (loop.timers->top > -1) {
                for(int j = i; j < loop.timers->top; j++) {
                    loop.timers->array[j] = loop.timers->array[j + 1];
                }
                loop.timers->top--;
                i--;
            }
        }
    }
}

void uv_run_tick() {
    for (int i = 0; i < loop.microtasks->top + 1; i++) {
        Timer* t = loop.microtasks->array[i];
        t->c();

        if (loop.microtasks->top > -1) {
            for(int j = i; j < loop.microtasks->top; j++) {
                loop.microtasks->array[j] = loop.microtasks->array[j + 1];
            }
            loop.microtasks->top--;
            i--;
        }
    }
}

void uv_set_timer(callback c, double time) {
    Timer* t = malloc(sizeof(Timer));
    t->time = loop.time + time;
    t->c = c;
    loop.timers->array[++loop.timers->top] = t;
}

void uv_next_tick(callback c) {
    Timer* t = malloc(sizeof(Timer));
    t->time = 0;
    t->c = c;
    loop.microtasks->array[++loop.microtasks->top] = t;
}

void* fs_read(void* data) {
    File* t = data;

    FILE *stream;
    stream = fopen("test.txt", "r");
    if (stream == NULL) {
        exit(1);
    }

    fseek(stream, 0, SEEK_END);
    size_t inputSize = ftell(stream);
    void* p = malloc(inputSize);
    memcpy(&(t->content), &p, sizeof(void*));

    fseek(stream, 0, SEEK_SET);
    fread(p, sizeof(char), inputSize, stream);
    fclose(stream);
    enQueue(t);

    return NULL;
}

void uv_fs_read(char* path, fs_callback c) {
    File* t = malloc(sizeof(File));
    t->c = c;
    t->path = path;
    t->content = malloc(sizeof(void*));
    loop.fs->array[++loop.fs->top] = t;
    int status = pthread_create(&(t->thread), NULL, fs_read, t);
}

void* response(void* data) {
    File* t = data;
    char* p = malloc(1024 * 16);
    char* s = p;
    memcpy(&(t->content), &p, sizeof(void*));
    char* res = "HTTP/1.1 200 Ok\r\nContent-Length: 6\r\nContent-Type: text/html; charset=UTF-8\r\n\r\nTest\r\n";
    bool reading = true;
    while(reading) {
        int bytes = recv( t->connection, p, 1024 * 16, 0);
        p += bytes;
        if (strstr(s, "\r\n\r\n") != NULL) {
            reading = false;
        }
    }
    send(t->connection, res, strlen(res), 0);
    close(t->connection);
    enQueue(t);

    return NULL;
}

void* run_server(void* data) {
    File* t = data;
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(t->port);

    bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    listen(listenfd, 10);

    while (true) {
        int connfd = accept(listenfd, NULL, NULL);
        pthread_t thread;
        t->connection = connfd;
        int status = pthread_create(&thread, NULL, response, t);
    }

    return NULL;
}

void uv_tcp_listen(int port, fs_callback c) {
    File* t = malloc(sizeof(File));
    t->c = c;
    t->port = port;
    t->content = malloc(sizeof(void*));
    loop.fs->array[++loop.fs->top] = t;
    int status = pthread_create(&(t->thread), NULL, run_server, t);
}

void* request(void* data) {
    File* t = data;
    char* url = t->path;
    struct addrinfo *res;
    struct addrinfo hints;
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    getaddrinfo(url, "80", &hints, &res);
    char* p = malloc(1024 * 5);
    memcpy(&(t->content), &p, sizeof(void*));
    char req[50];
    sprintf(req, "GET / HTTP/1.1\r\nHost: %s\r\n\r\n", url);

    struct sockaddr_in addr;
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = inet_addr(url);
    connect(sock, res->ai_addr, res->ai_addrlen);

    send(sock, req, 50, 0);
    while (recv(sock, p, 1024 * 5, 0) > 0) {};
    enQueue(t);
    close(sock);

    return NULL;
}

void uv_request(char* url, fs_callback c) {
    File* t = malloc(sizeof(File));
    t->c = c;
    t->path = url;
    t->content = malloc(sizeof(void*));
    loop.fs->array[++loop.fs->top] = t;
    int status = pthread_create(&(t->thread), NULL, request, t);
}

void uv_update_time() {
    gettimeofday(&tv, NULL);
    loop.time = (tv.tv_sec) * 1000 + (tv.tv_usec) / 1000;
}

void uv_poll() {
    if(loop.queue->rear == -1) {
        return;
    }
    for (int i = loop.queue->front; i <= loop.queue->rear; i++) {
        File* t = loop.queue->array[i];
        deQueue();
        t->c(t->content);
        
        if (loop.fs->top > -1) {
            for(int j = i; j < loop.fs->top; j++) {
                loop.fs->array[j] = loop.fs->array[j + 1];
            }
            loop.fs->top--;
            i--;
        }
    }
}

void uv_run() {
    bool alive = true;
    while(alive) {
        uv_update_time();
        uv_run_timers();
        uv_run_tick();
        uv_poll();
        uv_run_tick();

        if (loop.fs->top == -1 && loop.timers->top == -1) {
            alive = false;
        }
    }
}

void timer_callback() {
    printf("I'm a timer\n");
    fflush(stdout);
}

void tick_callback() {
    printf("I'm a tick\n");
    fflush(stdout);
}

void read_fs_callback(void* data) {
    printf("I'm a file: %s\n", (char*)data);
    fflush(stdout);
}

void listen_callback(void* data) {
    printf("I'm a request: %s\n", (char*)data);
    fflush(stdout);
}

void request_callback(void* data) {
    printf("I'm a response: %s\n", (char*)data);
    fflush(stdout);
}

int main() {
    uv_loop_init();
    uv_request("example.com", request_callback);
    uv_next_tick(tick_callback);
    uv_set_timer(timer_callback, 1000);
    uv_set_timer(timer_callback, 2000);
    uv_set_timer(timer_callback, 3000);
    uv_fs_read("test.txt", read_fs_callback);
    uv_tcp_listen(9000, listen_callback);
    uv_run();
}
