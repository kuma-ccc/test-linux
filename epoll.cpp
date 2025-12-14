#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <signal.h>

static bool g_running = true;
static void sigint_handler(int) 
{ 
    g_running = false; 
}

// 管理连接参数
struct ConnParams {
    int fd;
    std::string outbuf; // 存放待发送的数据（echo 的数据）
    bool closed{false};

    ConnParams(int fd_): fd(fd_) {}
    ~ConnParams() {}
};


/**
 * 将socket设置为非阻塞
*/
static int setFdNonblocking(int fd)
{
    // 获取socket的属性
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
    {
        return -1;
    }
    // 设置为非阻塞
    flags |= O_NONBLOCK;
    // 设置socket属性
    if (fcntl(fd, F_SETFL, flags) == -1)
    {
        return -1;
    }
    return 0;
}


// 修改 epoll 事件（enable/disable EPOLLOUT）
static bool mod_epoll(int epfd, int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        if (errno == ENOENT) {
            // Not found, try add
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
                perror("epoll_ctl ADD in mod_epoll");
                return false;
            } else return true;
        }
        perror("epoll_ctl MOD");
        return false;
    }
    return true;
}

/*
 *
 * 创建一个epoll实例
 */
int main(int argc, char const *argv[])
{
    int port = 0;
    if (argc < 2)
    {
        std::cerr << "usage: " << argv[0] << "<port> [bind_ip]\n";
        return 0;
    }
    if (argc >= 2)
    {
        port = atoi(argv[1]);
    }
    if (port <= 0 || port > 65535)
    {
        std::cerr << "invalid port: " << port << "\n";
        return 0;
    }
    std::string bind_ip = "0.0.0.0";
    if (argc >= 3)
    {
        bind_ip = argv[2];
    }
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    printf("port: %d, bind_ip: %s\n", port, bind_ip.c_str());
    // 建立监听的socket服务
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        std::cerr << "socket error: " << strerror(errno) << "\n"; 
    }
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET; // ipv4
    server_addr.sin_port = htons(port); // 端口
    server_addr.sin_addr.s_addr = inet_addr(bind_ip.c_str()); // ip
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind error");
        close(listen_fd);
        return 0;
    }
    // 开始监听    最多允许128 个连接
    if (listen(listen_fd, 128) < 0)
    {
        perror("listen error");
        close(listen_fd);
        return 0;
    }
    setFdNonblocking(listen_fd);
    // 创建epoll实例
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
    {
        perror("epoll_create error");
        close(listen_fd);
        return 0;
    }
    //  添加监听socket
    struct epoll_event event;
    event.data.fd = listen_fd;
    event.events = EPOLLIN | EPOLLET; // EPOLLIN 读事件 EPOLLET 边缘触发
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) < 0)
    {
        perror("epoll_ctl error");
        close(listen_fd);
        close(epoll_fd);
        return 0;
    }
    std::map<int, ConnParams*> connManager;
    const int MAX_EVENTS = 1024;
    std::vector<struct epoll_event> events(MAX_EVENTS);
    printf("main start\n");
    while (g_running)
    {
        printf("epoll_wait start\n");
        int nfds = epoll_wait(epoll_fd, events.data(), MAX_EVENTS, 1000);
        printf("epoll_wait %d\n", nfds);
        if (nfds < 0)
        {// 错误
            if (errno == EINTR)
            {// 是信号打断
                continue;
            }
            perror("epoll_wait error");
            break;
        }
        else if (nfds == 0)
        {// 超时
            continue;
        }
        for (int i = 0; i < nfds; i++)
        { 
            int fd = events[i].data.fd;
            uint32_t nowEvent = events[i].events;
            if (fd == listen_fd)
            {// 有新连接
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                if (conn_fd < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) // 非阻塞socket
                    {
                        continue; 
                    }
                }
                setFdNonblocking(conn_fd);
                connManager[conn_fd] = new ConnParams(conn_fd);
                event.data.fd = conn_fd;
                event.events = EPOLLIN | EPOLLET | EPOLLRDHUP; // EPOLLRDHUP 检测对端关闭
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &event) < 0)
                { 
                    perror("epoll_ctl error");
                    close(conn_fd);
                    delete connManager[conn_fd];
                    connManager.erase(conn_fd);
                    continue;
                }
                printf("new connection[%d] from %s:%d\n", conn_fd, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            }
            // 处理客户端数据
            // 先 判断错误事件
            if (nowEvent & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) 
            {//  EPOLLRDHUP 对端关闭 EPOLLHUP 句柄关闭 EPOLLERR 错误
                printf("client fd %d closed\n", fd);
                // 从epoll中删除
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                delete connManager[fd];
                connManager.erase(fd);
                close(fd);
                continue;
            }
            if (nowEvent & EPOLLOUT) // 写事件
            {
                auto it = connManager.find(fd); 
                if (it == connManager.end())
                {
                    printf("EPOLLOUT client fd %d not found\n", fd);
                    close(fd);
                    // 删除epoll
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    continue;
                }
                ConnParams* conn = it->second;
                while (conn->outbuf.size() > 0)
                {
                    int n = write(fd, conn->outbuf.data(), conn->outbuf.size());
                    if (n < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {// 非阻塞socket
                            continue;
                        }
                        perror("write error");
                        // 删除epoll
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        delete connManager[fd];
                        connManager.erase(fd);
                        continue;
                    }
                    conn->outbuf.erase(0, n); // 删除已经发送的数据
                }
                mod_epoll(epoll_fd, fd, EPOLLIN | EPOLLET | EPOLLRDHUP);

            }
            if (nowEvent & EPOLLIN) // 读事件
            {
                auto it = connManager.find(fd); 
                if (it == connManager.end())
                {
                    printf("EPOLLIN client fd %d not found\n", fd);
                    close(fd);
                    // 删除epoll
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    continue;
                }
                ConnParams* conn = it->second;
                while (1)
                {
                    char buf[1024];
                    int n = read(fd, buf, sizeof(buf));
                    if (n < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {// 非阻塞socket
                            break;
                        }
                        perror("read error"); 
                    } 
                    else if (n == 0)
                    {// 对端关闭
                        printf("client fd %d closed\n", fd);
                        // 删除epoll
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        delete connManager[fd];
                        connManager.erase(fd);
                        break;
                    }
                    int sendLen = 0;
                    int len = 0;
                    // 循环发送数据
                    while (sendLen < n)
                    { 
                        int len = write(fd, buf + sendLen, n - sendLen);
                        if (len < 0)
                        {
                            if (errno == EAGAIN || errno == EWOULDBLOCK)
                            {// 非阻塞socket
                                conn->outbuf.append(buf + sendLen, n - sendLen);
                                mod_epoll(epoll_fd, fd, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLOUT);
                                break;
                            } 
                            else if (errno == EINTR)
                            {
                                continue;
                            }
                            
                        }
                        // 发送成功
                        sendLen += len;
                    }
                }
            }
        }
        sleep(1);
        /* code */
    }
    return 0;

}