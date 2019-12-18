#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MSG 1
#define STOP 0
#define BUF_SIZE 1024

typedef struct {
    int id;
    int pipe[2];
    int free;
    int socket;
    pthread_t thread;
} thread_descr;

typedef struct {
    int id;
    int type;
    char * msg;
} msg;

int listenerPipe[2];
int msgPipe[2];
int threadsCount = 0;
thread_descr * threads;

void *listener(void *vargp)
{
    int *port = vargp;

    int listenSock = socket( AF_INET, SOCK_STREAM, IPPROTO_IP );
    if (listenSock == -1) {
        perror("socket error");
        exit(-1);
    }
    struct sockaddr_in servaddr;
    bzero( (void *)&servaddr, sizeof(servaddr) );

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl( INADDR_ANY );
    servaddr.sin_port = htons(*port);
    if (bind( listenSock, (struct sockaddr *)&servaddr, sizeof(servaddr) )) {
        perror("bind error");
        exit(-3);
    }
    if (listen( listenSock, 5 )) {
        perror("listen error");
        exit(-2);
    }
    int connSock;

    while(1)
    {
        connSock = accept( listenSock, (struct sockaddr *)NULL, (socklen_t *)NULL );
        printf("Connection established, socket: %d\n", connSock);
        if (write(listenerPipe[1], &connSock, sizeof(connSock)) == -1) {
            perror("write");
            exit(1);
        }
    }

    return NULL;
}

void *reader(void *vargp)
{
    thread_descr *params = vargp;
    printf("Tread %d start\n", params->id);

    char buf[BUF_SIZE + 1];
    msg message;
    message.id = params->id;
    
    int k;
    int sock;

    while(1) {
        if (read(params->pipe[0], &sock, sizeof(sock)) <= 0) {
            perror("read in thread");
            exit(2);
        }

        params->socket = sock;
        message.type = MSG;
        printf("thread %d got new task with socket %d\n", params->id, params->socket);

        while ((k = read(params->socket, &buf, BUF_SIZE)) > 0) {
            buf[k] = '\0';
            printf("thread %d got message: %s", params->id, buf);
            message.msg = malloc(sizeof(char) * k);
            strcpy(message.msg, buf);
            write(msgPipe[1], &message, sizeof(message));
        }

        printf("thread %d EOF\n", params->id);
        message.type = STOP;
        write(msgPipe[1], &message, sizeof(message));
    }
    
    return NULL;
}

int main(int argc, char ** argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s port\n", argv[0]);
        return 0;
    }

    int port = atoi(argv[1]);
    if (port <= 0) {
        fprintf(stderr, "Bad port: %s\n", argv[1]);
        return 0;
    }

    // init list pipe
    if (pipe(listenerPipe) < 0) {
        perror("pipe");
        return -1;
    }
    if (fcntl(listenerPipe[0], F_SETFL, O_NONBLOCK) < 0) {
        perror("fcntl");
        return -1;
    }
        
    // init msg pipe
    if (pipe(msgPipe) < 0) {
        perror("pipe");
        return -1;
    }
    if (fcntl(msgPipe[0], F_SETFL, O_NONBLOCK) < 0) {
        perror("fcntl");
        return -1;
    }

    // init listener thread
    pthread_t listenerThread;
    if (pthread_create(&listenerThread, NULL, listener, &port)) {
       perror("pthread_create");
       exit(3);
    }

    int socket;
    msg message;
    int freeThread;

    while(1) {

        switch (read(listenerPipe[0], &socket, sizeof(socket))) {
            case 0:
                perror("read from listenerPipe: EOF");
                exit(4);
            case -1:
                if (errno != EAGAIN) {
                    perror("read");
                    exit(5);
                }
                break;
            default:
                // find free thread or create a new one
                printf("got new socket: %d\n", socket);
                freeThread = -1;
                for (int i = 0; i < threadsCount; i++) {
                    if (threads[i].free) {
                        freeThread = i;
                        break;
                    }
                }
                printf("freeThread = %d\n", freeThread);
                if (freeThread == -1) {
                    freeThread = threadsCount;
                    threadsCount++;
                    // add thread
                    threads = realloc(threads, sizeof(thread_descr) * threadsCount);
                    if (threads == NULL) {
                        perror("realloc");
                        exit(6);
                    }
                    if (pipe(threads[freeThread].pipe) < 0) {
                        perror("pipe");
                        exit(7);
                    }
                    threads[freeThread].id = freeThread;
                    if (pthread_create(&threads[freeThread].thread, NULL, reader, &threads[freeThread])) {
                        perror("pthread_create");
                        exit(8);
                    }
                }

                // send new task to thread
                threads[freeThread].free = 0;
                threads[freeThread].socket = socket;
                if (write(threads[freeThread].pipe[1], &socket, sizeof(socket)) == -1) {
                    perror("write to thread");
                    shutdown(socket, SHUT_RDWR);
                    close(socket);
                }
        }

        switch (read(msgPipe[0], &message, sizeof(message))) {
            case 0:
                exit(9);
            case -1:
                if (errno != EAGAIN) {
                    perror("read");
                    exit(10);
                }
                break;
            default:
                if (message.type == MSG) {
                    // broadcast
                    for (int i = 0; i < threadsCount; i++) {
                        if (message.id != i && !threads[i].free) {
                            printf("send msg to thread %d\n", i);
                            if (write(threads[i].socket, message.msg, strlen(message.msg)) == -1) {
                                perror("write to socket");
                                threads[i].free = 1;
                                shutdown(threads[i].socket, SHUT_RDWR);
                                close(threads[i].socket);
                            }
                        }
                    }
                } else {
                    // free thread and close socket
                    printf("free thread %d\n", message.id);
                    threads[message.id].free = 1;
                    shutdown(threads[message.id].socket, SHUT_RDWR);
                    close(threads[message.id].socket);
                }
        }
    }

    return 0;
}
