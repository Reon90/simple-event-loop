#include <time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/time.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>

#define __USE_GNU
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <libaio.h>

typedef struct Event {
    void (*callback)(struct Event*);
    int fd;
    void* data;
} Event;
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
    char* path;
    void* content;
    callback c;
    pthread_t thread;
    int connection;
    int port;
    int progress;
    int index;
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
    int epollfd;
    int eventfd;
    io_context_t ctx;
} Loop;

Loop loop;
struct timeval tv;

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

    loop.epollfd = epoll_create( 100 );
    loop.eventfd = eventfd(0, EFD_NONBLOCK);
    io_context_t ctx = 0;
	int r = io_setup(128, &ctx);
    if (r < 0) {
		printf("io_setup()");
	}
    loop.ctx = ctx;
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

void fs_on_read(Event* e) {
    int event_fd = loop.eventfd;
    uint64_t count = 0;
    int ret = read(event_fd, &count, sizeof(count));
    if (ret < 0) {
        printf("read fail");
    } else {
        while (count > 0) {
			struct io_event event;
			if (io_getevents(loop.ctx, 1, 1, &event, NULL) != 1) {
				printf("io_getevents");
			}
			
            File* t = (File*)e->data;
            close(e->fd);
            t->c(event.data);

            if (loop.fs->top > -1) {
                loop.fs->array[t->index] = loop.fs->array[t->index + 1];
                loop.fs->top--;
            }

			count--;
		}
    }
}

void* fs_read(void* data) {
    File* t = data;

    int fd = open("test.txt", O_DIRECT);
    if (fd < 0) {
		printf("test.txt");
	}

    void *p = NULL;
    long sz = sysconf(_SC_PAGESIZE);
	posix_memalign(&p, sz, sz * 4);
	struct iocb cb;
    io_prep_pread(&cb, fd, p, sz * 4, 0);
    cb.data = p;
	struct iocb *list_of_iocb[1] = {&cb};

    io_set_eventfd(&cb, loop.eventfd);

    Event* e = malloc(sizeof(Event));
    e->callback = fs_on_read;
    e->fd = fd;
    e->data = data;
    struct epoll_event read_event;
    read_event.events = EPOLLHUP | EPOLLERR | EPOLLIN;
    read_event.data.ptr = e;
    epoll_ctl( loop.epollfd, EPOLL_CTL_ADD, loop.eventfd, &read_event );

    int r = io_submit(loop.ctx, 1, list_of_iocb);
    if (r != 1) {
		printf("io_submit()");
	}
    
    memcpy(&(t->content), &p, sizeof(void*));

    return NULL;
}

void uv_fs_read(char* path, fs_callback c) {
    File* t = malloc(sizeof(File));
    t->c = c;
    t->path = path;
    t->content = malloc(sizeof(void*));
    loop.fs->array[++loop.fs->top] = t;
    t->index = loop.fs->top;
    fs_read(t);
}

void* response(Event* e) {
    File* t = (File*)e->data;

    if (t->progress == -2) {
        char* p = malloc(1024 * 5);
        memcpy(&(t->content), &p, sizeof(void*));
        t->progress = recv(e->fd, p, 1024 * 5, 0);
    } else if (t->progress == -1) {
        t->progress = recv(e->fd, t->content, 1024 * 5, 0);
    }

    if (t->progress > 0) {
        char* res = "HTTP/1.1 200 Ok\r\nContent-Length: 6\r\nContent-Type: text/html; charset=UTF-8\r\n\r\nTest\r\n";
        send(t->connection, res, strlen(res), 0);
        close(t->connection);
        t->c(t->content);

        if (loop.fs->top > -1) {
            loop.fs->array[t->index] = loop.fs->array[t->index + 1];
            loop.fs->top--;
        }
    } else {
        struct epoll_event ev = { 0 };
        ev.events = EPOLLIN | EPOLLONESHOT | EPOLLET;
        ev.data.ptr = e;
        epoll_ctl( loop.epollfd, EPOLL_CTL_MOD, e->fd, &ev );
    }
}

void on_response(Event* e) {
    File* t = (File*)e->data;
    int connfd = accept(t->connection, NULL, NULL);
    pthread_t thread;
    t->connection = connfd;
    e->fd = connfd;
    response(e);
}

void* run_server(void* data) {
    File* t = data;
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    t->connection = listenfd;

    Event* e = malloc(sizeof(Event));
    e->callback = on_response;
    e->fd = listenfd;
    e->data = data;

    struct epoll_event ev = { 0 };
    ev.events = EPOLLIN | EPOLLONESHOT | EPOLLET;
    ev.data.ptr = e;
    fcntl(listenfd, F_SETFL, O_NONBLOCK);
    epoll_ctl( loop.epollfd, EPOLL_CTL_ADD, listenfd, &ev );

    struct sockaddr_in address;

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(t->port);

    bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    listen(listenfd, 10);

    return NULL;
}

void uv_tcp_listen(int port, fs_callback c) {
    File* t = malloc(sizeof(File));
    t->c = c;
    t->port = port;
    t->progress = -2;
    t->content = malloc(sizeof(void*));
    loop.fs->array[++loop.fs->top] = t;
    t->index = loop.fs->top;
    run_server(t);
}

void on_request(Event* e) {
    File* t = (File*)e->data;

    if (t->progress == -2) {
        char req[50];
        sprintf(req, "GET / HTTP/1.1\r\nHost: %s\r\n\r\n", "example.com");
        send(e->fd, req, 50, 0);
        char* p = malloc(1024 * 5);
        memcpy(&(t->content), &p, sizeof(void*));
        t->progress = recv(e->fd, p, 1024 * 5, 0);
    } else if (t->progress == -1) {
        t->progress = recv(e->fd, t->content, 1024 * 5, 0);
    }

    if (t->progress > 0) {
        close(e->fd);
        t->c(t->content);

        if (loop.fs->top > -1) {
            loop.fs->array[t->index] = loop.fs->array[t->index + 1];
            loop.fs->top--;
        }
    } else {
        struct epoll_event ev = { 0 };
        ev.events = EPOLLIN | EPOLLONESHOT | EPOLLET;
        ev.data.ptr = e;
        epoll_ctl( loop.epollfd, EPOLL_CTL_MOD, e->fd, &ev );
    }
}

void* request(void* data) {
    File* t = data;
    char* url = t->path;
    struct addrinfo *res;
    struct addrinfo hints;
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    getaddrinfo(url, "80", &hints, &res);

    struct sockaddr_in addr;
    int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    Event* e = malloc(sizeof(Event));
    e->callback = on_request;
    e->fd = sockfd;
    e->data = data;

    struct epoll_event ev = { 0 };
    ev.events =  EPOLLOUT | EPOLLONESHOT | EPOLLET;
    ev.data.ptr = e;
    fcntl(sockfd, F_SETFL, O_NONBLOCK);
    epoll_ctl( loop.epollfd, EPOLL_CTL_ADD, sockfd, &ev );

    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = inet_addr(url);
    connect(sockfd, res->ai_addr, res->ai_addrlen);

    return NULL;
}

void uv_request(char* url, fs_callback c) {
    File* t = malloc(sizeof(File));
    t->c = c;
    t->path = url;
    t->progress = -2;
    t->content = malloc(sizeof(void*));
    loop.fs->array[++loop.fs->top] = t;
    t->index = loop.fs->top;
    request(t);
}

void uv_update_time() {
    gettimeofday(&tv, NULL);
    loop.time = (tv.tv_sec) * 1000 + (tv.tv_usec) / 1000;
}

void uv_poll() {
    struct epoll_event pevents[ 20 ];

    int ret = epoll_wait( loop.epollfd, pevents, 20, 1000 );

    if ( ret == -1 ) {
        printf("error\n");
        fflush(stdout);
    } else if ( ret == 0 ) {
        printf("wait\n");
        fflush(stdout);
    } else {
        printf("emit\n");
        fflush(stdout);
        for ( int i = 0; i < ret; i++ ) {
            if ( (pevents[i].events & EPOLLIN) || (pevents[i].events & EPOLLOUT) ) {
                Event* e = (Event*)pevents[i].data.ptr;
                e->callback(e);
            }
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
