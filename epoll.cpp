#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <fcntl.h>


void main(int argc, char const *argv[])
{
    printf("main start\n");
    while (1)
    {
        printf("main loop\n");
        // 建立socket
        /*
        创建监听 socket

初始化 epoll

将监听 fd 加入 epoll

epoll_wait 返回事件后打印一句“event triggered”
        */
        // 创建监听 socket 
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in listen_addr;
        listen_addr.sin_family = AF_INET;
        listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        listen_addr.sin_port = htons(1234);
        // 绑定监听 socket
        bind(listen_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr));
        // 监听 socket
        listen(listen_fd, 5);
        int epoll_fd = epoll_create(1);
        struct epoll_event event;
        event.data.fd = listen_fd;
        event.events = EPOLLIN;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event);
        while (1)
        {
            struct epoll_event events[1024];
            int nfds = epoll_wait(epoll_fd, events, 1024, -1);
            printf("event triggered\n");
            for (int i = 0; i < nfds; i++)
            {
                if (events[i].data.fd == listen_fd)
                {
                    struct sockaddr_in client_addr;
                }
            }
        }
        sleep(1);
        /* code */
    }
    

}