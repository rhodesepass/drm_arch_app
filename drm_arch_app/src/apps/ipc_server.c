// IPC 线程
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utils/log.h>
#include "apps/apps_types.h"
#include "apps/ipc_server.h"
#include "apps/ipc_handler.h"
#include "apps/ipc_common.h"


static int set_nonblock_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;

    int fdfl = fcntl(fd, F_GETFD, 0);
    if (fdfl >= 0) (void)fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
    return 0;
}

static int make_listen_sock(const char *path, int backlog) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return -1;
    if (set_nonblock_cloexec(fd) < 0) { close(fd); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    unlink(path);
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void close_fd(int epfd, int fd) {
    (void)epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}


void* apps_ipc_server_thread(void* arg){
    apps_t* apps = (apps_t*)arg;
    log_info("==> IPC server thread starting!");
    int lfd = make_listen_sock(APPS_IPC_SOCKET_PATH, APPS_IPC_BACKLOG);
    if (lfd < 0) { 
        log_error("make_listen_sock failed: %s", APPS_IPC_SOCKET_PATH); 
        return NULL; 
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { 
        log_error("epoll_create1 failed: %s", APPS_IPC_SOCKET_PATH);
        close(lfd); 
        return NULL; 
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = lfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev) < 0) {
        log_error("epoll_ctl ADD lfd failed: %s", APPS_IPC_SOCKET_PATH);
        close(epfd); 
        close(lfd); 
        unlink(APPS_IPC_SOCKET_PATH);
        return NULL;
    }

    uint8_t *rxbuf = (uint8_t*)malloc(APPS_IPC_MAX_MSG);
    uint8_t *txbuf = (uint8_t*)malloc(APPS_IPC_MAX_MSG);

    if (!rxbuf || !txbuf) {
        free(rxbuf); 
        free(txbuf);
        close(epfd); 
        close(lfd); 
        unlink(APPS_IPC_SOCKET_PATH);
        return NULL;
    }

    struct epoll_event events[16];

    log_info("==> IPC server thread started!");
    while (atomic_load(&apps->ipc_running)) {
        int n = epoll_wait(epfd, events, (int)(sizeof(events)/sizeof(events[0])), 1000);
        if (n == 0) continue;
        if (n < 0) {
            if (errno == EINTR) continue;
            log_error("epoll_wait failed: %s", APPS_IPC_SOCKET_PATH);
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t e = events[i].events;

            if (fd == lfd) {
                for (;;) {
                    int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept4");
                        break;
                    }
                    struct epoll_event cev;
                    memset(&cev, 0, sizeof(cev));
                    cev.events = EPOLLIN | EPOLLRDHUP;
                    cev.data.fd = cfd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
                        perror("epoll_ctl ADD cfd");
                        close(cfd);
                    }
                }
                continue;
            }

            if (e & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                close_fd(epfd, fd);
                continue;
            }

            if (e & EPOLLIN) {
                for (;;) {
                    // recvmsg 获取 MSG_TRUNC
                    struct iovec iov;
                    iov.iov_base = rxbuf;
                    iov.iov_len  = APPS_IPC_MAX_MSG;

                    struct msghdr msg;
                    memset(&msg, 0, sizeof(msg));
                    msg.msg_iov = &iov;
                    msg.msg_iovlen = 1;

                    ssize_t r = recvmsg(fd, &msg, 0);
                    if (r < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        log_error("recvmsg failed: %s", APPS_IPC_SOCKET_PATH);
                        close_fd(epfd, fd);
                        break;
                    }
                    if (r == 0) { // peer closed
                        close_fd(epfd, fd);
                        break;
                    }

                    if (msg.msg_flags & MSG_TRUNC) {
                        // 对端发的包 > max_msg，被截断了。丢弃并回错误码
                        (void)send(fd, &ipc_resp_too_large, sizeof(ipc_resp_type_t), 0);
                        continue;
                    }

                    int txlen = apps_ipc_handler(apps, rxbuf, (size_t)r, txbuf, APPS_IPC_MAX_MSG);
                    if ((size_t)txlen > APPS_IPC_MAX_MSG) txlen = (int)APPS_IPC_MAX_MSG;

                    // 由于不需要并发，这里简单阻塞式尝试发送；若 EAGAIN 则本连接可能写缓冲满
                    // 低QPS一般不会发生。严格做法是注册 EPOLLOUT + per-conn 发送队列。
                    ssize_t w = send(fd, txbuf, (size_t)txlen, 0);
                    if (w < 0) {
                        perror("send");
                        close_fd(epfd, fd);
                        break;
                    }
                }
            }
        }
    }

    log_info("==> IPC server thread ended!");
    free(rxbuf); free(txbuf);
    close(epfd); close(lfd);
    unlink(APPS_IPC_SOCKET_PATH);
    return 0;
}
