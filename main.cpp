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

// #define listenfdET /* 设置ET模式（在addfd函数中修改这个属性）*/
#define listenfdLT /* 设置LT模式（在addfd函数中修改这个属性） */

/* ET模式下epoll_wait通知事件发生后需要立即处理完毕这个事件的所有内容，因为后续不会再通知此事件
具体可以看后面的if (sockfd == listenfd)部分的内容，很好地诠释了LT和ET工作模式*/

/* 下面三个函数在http_conn.cpp中定义 */
extern int addfd(int epollfd, int fd, bool one_shot); /* 把fd加入到内核事件监听表epollfd中 */
extern int remove(int epollfd, int fd);               /* 从内核事件监听表epollfd中移除fd */
extern int setnonblocking(int fd);                    /* 设置fd的属性为非阻塞 */

static int pipefd[2];            /* 主线程和子线程之间通信用的管道 */
static sort_timer_lst timer_lst; /* 定时器升序链表实例，静态的，只能在当前文件内使用 */
static int epollfd = 0;          /* 标识内核事件监听表的文件描述符 */

/* 传入一个信号值sig，将它通过管道发送给主线程 */
void sig_handler(int sig)
{
    // （不理解）为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

/* 监听某个信号sig，并设置其信号处理函数handler，restart参数设置中断前的系统调用是否重启 */
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

/* 定时处理任务，重新定时以不断触发SIGALRM信号 */
void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);
}

/* 定时器回调函数，删除非活动连接在epollfd上的注册事件，并关闭 */
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0); /* 取消监听 */
    assert(user_data);

    close(user_data->sockfd); /* 关闭socket连接 */

    http_conn::m_user_count--; /* http连接个数相应减少 */

    LOG_INFO("close fd %d", user_data->sockfd); /* 输出日志 */
    Log::get_instance()->flush();
}

/* 给connfd发送info错误信息，然后关闭connfd对应的socket连接 */
void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{
#ifdef ASYNLOG /* 异步日志 */
    Log::get_instance()->init("ServerLog", 2000, 800000, 8);
#endif

#ifdef SYNLOG /* 同步日志 */
    Log::get_instance()->init("ServerLog", 2000, 800000, 0);
#endif

    /* 判断程序输入参数是否正确 */
    if (argc <= 1)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]); /* 从argv获得端口号 */

    addsig(SIGPIPE, SIG_IGN); /* 监听到SIGPIPE信号直接忽略 */

    connection_pool *connPool = connection_pool::GetInstance(); /* 指向数据库连接池唯一实例的指针变量 */

    connPool->init("localhost", "root", "root", "web", 3306, 8); /* 数据库连接池初始化 */

    threadpool<http_conn> *pool = NULL; /* 指向http_conn类型线程池的指针变量 */
    try
    {
        pool = new threadpool<http_conn>(connPool);
    } /* 线程池初始化 */
    catch (...)
    {
        return 1;
    }

    http_conn *users = new http_conn[MAX_FD]; /* 存放http_conn连接用户的数据，可以用fd来索引对应用户数据 */
    assert(users);

    /* 作用仅仅是从数据库中取出用户名和密码
    注意一下，存放用户名和密码的map容器变量名也是users，不要搞混了 */
    users->initmysql_result(connPool);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0); /* 主线程中的监听描述符 */
    assert(listenfd >= 0);

    // struct linger tmp={1,0};
    // SO_LINGER若有数据待发送，延迟关闭
    // setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));

    int ret = 0; /* 存放系统调用返回值的临时变量 */

    struct sockaddr_in address; /* 存放socket地址的变量，包含协议族，IP地址，端口号 */
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY); /* INADDR_ANY：Address to accept any incoming messages. */
    address.sin_port = htons(port);

    /* 连接关闭后不用TIME_WAIT可以立即重用刚关闭的socket使用的IP和端口号 */
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    /* 将监听描述符和监听socket的地址信息绑定上 */
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    printf("bind return : %d", ret);
    assert(ret >= 0);

    /* 监听指定端口号上任意IP地址的消息 */
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    epoll_event events[MAX_EVENT_NUMBER]; /* 存放监听到的内核事件的数组 */
    epollfd = epoll_create(5);            /* 标识内核事件表的文件描述符真正建立了 */
    assert(epollfd != -1);

    addfd(epollfd, listenfd, false); /* 把监听文件描述符listenfd加入监听表 */
    http_conn::m_epollfd = epollfd;  /* http_conn类中m_epollfd其实就是主线程中的epollfd */

    /* 创建主线程和子线程之间通信用的管道
    注意，用socketpair创建的管道pipefd[0]和pipefd[1]都是可读可写的，但一般还是用0读，1写 */
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);

    /* 为什么管道写端要非阻塞？
    send是将信息发送给套接字缓冲区，如果缓冲区满了，则会阻塞，这时候会进一步增加信号处理函数的执行时间，为此，将其修改为非阻塞。 */
    setnonblocking(pipefd[1]);

    /* 监听管道的读端 */
    addfd(epollfd, pipefd[0], false);

    /* 添加监听信号，不使用restart参数 */
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);

    bool stop_server = false; /* 是否停止服务器运行 */

    client_data *users_timer = new client_data[MAX_FD]; /* 指向定时器链表中用户数据的指针变量，可用users_timer[fd]索引fd的用户数据*/

    bool timeout = false; /* 是否超时？ */
    alarm(TIMESLOT);      /* TIMESLOT秒后会触发SIGALRM信号 */

    while (!stop_server)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1); /* evnents上监听到的事件数量 */

        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        /* 依次遍历evnents上监听到的事件 */
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd; /* 存放所监听事件的文件描述符信息，方便判断如何处理接收到信息 */

            /* 有新的客户连接来了 */
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;                    /* 存放客户连接的socket地址信息 */
                socklen_t client_addrlength = sizeof(client_address); /* 存放客户socket地址的长度 */

/* LT模式 */
#ifdef listenfdLT

                /* 存放所接受客户连接对应的文件描述符 */
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);

                /* 判断是否有异常情况出现 */
                if (connfd < 0)
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }

                /* 初始化connfd对应的http_conn对象里存放的数据 */
                users[connfd].init(connfd, client_address);

                // 初始化client_data数据
                // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中

                /* 初始化定时器链表中结点对应的用户数据 */
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;

                /* 一个指向定时器链表中结点的指针变量，用于初始化 */
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd]; /* 设置用户数据 */
                timer->cb_func = cb_func;                /* 设置回调函数 */

                /* 设置超时时间为3倍的TIMESLOT */
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;

                /* 有点套娃，用户数据里又存放了定时器链表中的结点信息 */
                users_timer[connfd].timer = timer;

                /* 将新的定时器结点插入到定时器链表的正确位置 */
                timer_lst.add_timer(timer);

#endif

/* ET模式
可以看到ET模式需要一直接受新的连接，直到没有新的连接可以接受为止才会break，处理一下个内核事件 */
#ifdef listenfdET
                while (1)
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);

                    // 初始化client_data数据
                    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif
            }

            /* 不管是哪个文件描述符（管道、客户连接）出现以下3个错误，我们都关闭这个连接并移除定时器
            但既然定时器都出来了，那难道不是只针对客户连接吗，管道好像没有对应的定时器才对？
            此外，定时器的作用不就是关闭非活动连接吗，所以更不可能扯上管道了
            嗯，是这样的*/
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);
                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }

            /* 处理管道上的可读信号 */
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        /* 收到SIGALRM信号等会就要去处理非活动连接了 */
                        case SIGALRM:
                        {
                            timeout = true;
                            break;
                        }

                        /* 服务器停止运行的信号 */
                        case SIGTERM:
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }

            /* 处理客户连接上接收到的数据 */
            else if (events[i].events & EPOLLIN)
            {
                util_timer *timer = users_timer[sockfd].timer;

                if (users[sockfd].read_once())
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    // 若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);

                    // 若有数据传输，则将定时器往后延迟3个单位
                    // 并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }

            /* 处理客户连接上的可写事件 */
            else if (events[i].events & EPOLLOUT)
            {
                util_timer *timer = users_timer[sockfd].timer;

                if (users[sockfd].write())
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    // 若有数据传输，则将定时器往后延迟3个单位
                    // 并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if (timeout)
        {
            timer_handler();
            timeout = false;
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
