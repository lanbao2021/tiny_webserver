#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65536           // 最大可打开的文件描述符
#define MAX_EVENT_NUMBER 10000 // 最大可监听的事件数
#define TIMESLOT 5             // 设置最小超时单位，每TIMESLOT秒触发一次SIGALRM信号

#define SYNLOG // 同步写日志
// #define ASYNLOG // 异步写日志

// #define listenfdET // 设置ET模式
#define listenfdLT // 设置LT模式

//////////////////////////////////////////////////////////////////////////////////////////////////////
////// 知识点：ET模式下epoll_wait通知事件发生后需要立即处理完毕这个事件的所有内容，因为后续不会再通知此事件 ////////
////// 具体可以看后面的if (sockfd == listenfd)部分的内容，很好地诠释了LT和ET工作模式                 ////////
////////////////////////////////////////////////////////////////////////////////////////////////////

// 下面三个函数在http_conn.cpp中定义
extern int addfd(int epollfd, int fd, bool one_shot); // 把fd加入到内核事件监听表epollfd中
extern int remove(int epollfd, int fd);               // 从内核事件监听表epollfd中移除fd
extern int setnonblocking(int fd);                    // 设置fd的属性为非阻塞

static int pipefd[2];            // 传递监听到的信号的管道，交给主线程处理
static sort_timer_lst timer_lst; // 定时器升序链表实例，静态的，只能在当前文件内使用
static int epollfd = 0;          // 标识内核事件监听表的文件描述符

// 传入一个信号值sig，将它通过管道发送给主线程
void sig_handler(int sig)
{
    // （不理解）为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// 监听某个信号sig，并设置其信号处理函数handler，restart参数设置中断前的系统调用是否重启
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick(); // 检查链表上是否有到期任务
    alarm(TIMESLOT);  // 过TIMESLOT秒后再次触发SIGALRM信号
}

// 定时器回调函数，删除非活动连接在epollfd上的注册事件，并关闭
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0); // 取消监听
    assert(user_data);
    close(user_data->sockfd);                   // 关闭socket连接
    http_conn::m_user_count--;                  // http连接个数相应减少
    LOG_INFO("close fd %d", user_data->sockfd); // 输出日志
    Log::get_instance()->flush();               // 强制刷新缓冲区
}

// 给connfd发送info错误信息，然后关闭connfd对应的socket连接
void show_error(int connfd, const char *info)
{
    printf("%s", info);                  // 输出错误信息
    send(connfd, info, strlen(info), 0); // 发送错误信息
    close(connfd);                       // 关闭socket连接
}

int main(int argc, char *argv[])
{
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); // 初始化异步日志
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); // 初始化同步日志
#endif

    if (argc <= 1) // 没有输入端口号
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0])); // basename()返回路径中的文件名部分
        return 1;
    }

    int port = atoi(argv[1]); // Convert a string to an integer

    addsig(SIGPIPE, SIG_IGN); // 监听到SIGPIPE信号直接忽略

    connection_pool *connPool = connection_pool::GetInstance();  // 指向数据库连接池唯一实例的指针变量
    connPool->init("localhost", "root", "root", "web", 3306, 8); // 数据库连接池初始化

    threadpool<http_conn> *pool = NULL; // 指向http_conn类型的工作线程池
    try
    {
        pool = new threadpool<http_conn>(connPool); // 创建线程池
    }
    catch (...) // 捕获所有异常
    {
        return 1;
    }

    // 用于存放「所有可能的」socket连接的数据，可以用fd来索引对应用户数据
    http_conn *users = new http_conn[MAX_FD];
    assert(users);

    // 作用仅仅是从数据库中取出用户名和密码，存放到http_conn.cpp里的全局变量map<string, string> users;
    // 注意这里的users(http_conn)和上一条注释的users(map容器)不是同一个变量
    // users = users[0]，就是随便取一个users数组中的元素，然后调用它的initmysql_result()函数
    // 所以其实map<string, string> users定义成类的静态成员变量会不会更合理？
    users->initmysql_result(connPool);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0); // 主线程中的监听描述符
    assert(listenfd >= 0);

    // SO_LINGER若有数据待发送，延迟关闭
    // 下面这两行注释掉就能让webbench测试通过，但是不注释掉的话，webbench测试不通过
    // struct linger tmp = {1, 0};
    // setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    int ret = 0; // 存放系统调用返回值的临时变量

    struct sockaddr_in address;                  // 存放socket地址的变量，包含协议族，IP地址，端口号
    bzero(&address, sizeof(address));            // 将address中前sizeof(address)个字节置为0
    address.sin_family = AF_INET;                // 协议族
    address.sin_addr.s_addr = htonl(INADDR_ANY); // INADDR_ANY：Address to accept any incoming messages.
    address.sin_port = htons(port);              // 端口号，htons()将主机字节序转换为网络字节序

    // 连接关闭后不用TIME_WAIT可以立即重用刚关闭的socket使用的IP和端口号
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    // 将监听描述符和监听socket的地址信息绑定上
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    printf("bind return : %d", ret);
    assert(ret >= 0);

    // 监听指定端口号上任意IP地址的消息
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    epoll_event events[MAX_EVENT_NUMBER]; // 存放监听到的内核事件的数组
    epollfd = epoll_create(5);            // 标识内核事件表的文件描述符真正建立了
    assert(epollfd != -1);

    addfd(epollfd, listenfd, false); // 把监听文件描述符listenfd加入监听表
    http_conn::m_epollfd = epollfd;  // http_conn类中m_epollfd其实就是主线程中的epollfd

    // 给信号处理函数用的，实现统一信号源
    // 注意，用socketpair创建的管道pipefd[0] 和pipefd[1] 都是可读可写的，但一般还是用0读，1写
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);

    // 为什么管道写端要非阻塞？
    // send是将信息发送给套接字缓冲区，如果缓冲区满了，则会阻塞，这时候会进一步增加信号处理函数的执行时间，为此，将其修改为非阻塞。
    setnonblocking(pipefd[1]);

    // 监听管道的读端
    addfd(epollfd, pipefd[0], false);

    // 添加监听信号，不使用restart参数
    addsig(SIGALRM, sig_handler, false); // 定时器信号
    addsig(SIGTERM, sig_handler, false); // 终止进程信号

    bool stop_server = false; // 是否停止服务器运行

    // 指向定时器链表中用户数据的指针变量，可用users_timer[fd]索引fd的用户数据
    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false; // 超时标志
    alarm(TIMESLOT);      // TIMESLOT秒后会触发SIGALRM信号

    while (!stop_server)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1); // evnents上监听到的事件数量

        if (number < 0 && errno != EINTR) // 出错
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        // 依次遍历evnents上监听到的事件
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd; // 存放所监听事件的文件描述符信息，方便判断如何处理接收到信息

            // 有新的客户连接来了
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;                    // 存放客户连接的socket地址信息
                socklen_t client_addrlength = sizeof(client_address); // 存放客户socket地址的长度

#ifdef listenfdLT

                // 存放所接受客户连接对应的文件描述符
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);

                // 判断是否有异常情况出现
                if (connfd < 0)
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }

                // 判断是否超过最大连接数
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy"); // 给connfd发送info错误信息，然后关闭connfd对应的socket连接
                    LOG_ERROR("%s", "Internal server busy");    // 输出日志
                    continue;
                }

                users[connfd].init(connfd, client_address); // 初始化该socket连接对应的http_conn对象的数据成员

                users_timer[connfd].address = client_address; // 初始化该socket连接对应的定时器链表中结点的用户数据
                users_timer[connfd].sockfd = connfd;          // 初始化该socket连接对应的定时器链表中结点的用户数据
                util_timer *timer = new util_timer;           // 创建定时器结点
                timer->user_data = &users_timer[connfd];      // 设置用户数据
                timer->cb_func = cb_func;                     // 设置回调函数

                time_t cur = time(NULL);            // 获取当前时间
                timer->expire = cur + 3 * TIMESLOT; // 设置超时时间为3倍的TIMESLOT

                users_timer[connfd].timer = timer; // 有点套娃，用户数据里的timer指针存放了定时器链表中的结点信息
                timer_lst.add_timer(timer);        // 将新的定时器结点插入到定时器链表的正确位置

#endif

#ifdef listenfdET

                // ET模式需要一直接受新的连接，直到没有新的连接可以接受为止才会break，处理一下个内核事件
                while (1)
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);

                    if (connfd < 0) // 出错
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }

                    if (http_conn::m_user_count >= MAX_FD) // 超过最大连接数
                    {
                        show_error(connfd, "Internal server busy"); // 给connfd发送info错误信息，然后关闭connfd对应的socket连接
                        LOG_ERROR("%s", "Internal server busy");    // 输出日志
                        break;
                    }

                    users[connfd].init(connfd, client_address); // 初始化该socket连接对应的http_conn对象的数据成员

                    users_timer[connfd].address = client_address; // 初始化该socket连接对应的定时器链表中结点的用户数据
                    users_timer[connfd].sockfd = connfd;          // 初始化该socket连接对应的定时器链表中结点的用户数据

                    util_timer *timer = new util_timer;      // 创建定时器结点
                    timer->user_data = &users_timer[connfd]; // 设置用户数据
                    timer->cb_func = cb_func;                // 设置回调函数

                    time_t cur = time(NULL);            // 获取当前时间
                    timer->expire = cur + 3 * TIMESLOT; // 设置超时时间为3倍的TIMESLOT
                    users_timer[connfd].timer = timer;  // 有点套娃，用户数据里的timer指针存放了定时器链表中的结点信息

                    timer_lst.add_timer(timer); // 将新的定时器结点插入到定时器链表的正确位置
                }

                continue; // 接收完所有新连接后，处理下一个内核事件（后续用的是else if所以这个continue其实也没必要）
#endif
            }

            // 不管是哪个文件描述符出现以下3个错误，我们都服务器端关闭连接，移除对应的定时器
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                util_timer *timer = users_timer[sockfd].timer; // 获取该socket连接对应的定时器结点
                timer->cb_func(&users_timer[sockfd]);

                // 如果定时器结点存在（注意：管道的timer不存在）
                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }

            // 处理管道上的可读信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;                                            // 用来存放接收到的信号值
                char signals[1024];                                 // 用来存放接收到的信号值
                ret = recv(pipefd[0], signals, sizeof(signals), 0); // 接收信号值

                if (ret == -1) // 出错
                {
                    continue;
                }

                else if (ret == 0) // 没有接收到信号值
                {
                    continue;
                }

                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {

                        case SIGALRM:
                        {
                            timeout = true; // 收到SIGALRM信号等会就要去处理非活动连接了
                            break;
                        }

                        case SIGTERM:
                        {
                            stop_server = true; // 服务器停止运行的信号
                        }
                        }
                    }
                }
            }

            // 处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                util_timer *timer = users_timer[sockfd].timer; // 获取该socket连接对应的定时器结点，等会要更新其超时时间

                if (users[sockfd].read_once()) // 读取客户数据
                {

                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr)); // 将网络字节序的IP地址转换为点分十进制的IP地址
                    Log::get_instance()->flush();                                                           // 强制刷新缓冲区

                    pool->append(users + sockfd); // 若监测到读事件，将该事件放入请求队列中，工作线程池中的某个线程会处理这个事件

                    if (timer)
                    {
                        time_t cur = time(NULL);             // 获取当前时间
                        timer->expire = cur + 3 * TIMESLOT;  // 若有数据传输，则将定时器往后延迟3个单位
                        LOG_INFO("%s", "adjust timer once"); // 输出日志
                        Log::get_instance()->flush();        // 强制刷新缓冲区
                        timer_lst.adjust_timer(timer);       // 并对新的定时器在链表上的位置进行调整
                    }
                }

                else // 读取失败，关闭连接
                {
                    timer->cb_func(&users_timer[sockfd]); // 调用定时器的回调函数，即删除非活动连接在epollfd上的注册事件，并关闭
                    if (timer)
                    {
                        timer_lst.del_timer(timer); // 从定时器链表中删除该定时器结点
                    }
                }
            }

            // 处理客户连接上的可写事件
            else if (events[i].events & EPOLLOUT)
            {
                util_timer *timer = users_timer[sockfd].timer; // 获取该socket连接对应的定时器结点，等会要更新其超时时间

                if (users[sockfd].write()) // 向客户发送数据
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr)); // 将网络字节序的IP地址转换为点分十进制的IP地址
                    Log::get_instance()->flush();                                                              // 强制刷新缓冲区

                    if (timer)
                    {
                        time_t cur = time(NULL);             // 获取当前时间
                        timer->expire = cur + 3 * TIMESLOT;  // 若有数据传输，则将定时器往后延迟3个单位
                        LOG_INFO("%s", "adjust timer once"); // 输出日志
                        Log::get_instance()->flush();        // 强制刷新缓冲区
                        timer_lst.adjust_timer(timer);       // 并对新的定时器在链表上的位置进行调整
                    }
                }

                else // 写入失败，关闭连接
                {
                    timer->cb_func(&users_timer[sockfd]); // 调用定时器的回调函数，即删除非活动连接在epollfd上的注册事件，并关闭
                    if (timer)
                    {
                        timer_lst.del_timer(timer); // 从定时器链表中删除该定时器结点
                    }
                }
            }
        }

        if (timeout)
        {
            timer_handler(); // 处理非活动连接
            timeout = false; // 重置timeout标志
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
