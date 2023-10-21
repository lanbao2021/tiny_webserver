#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool // 线程池类，将它定义为模板类是为了代码复用。模板参数T是任务类
{
public:
    threadpool(connection_pool *connPool, int thread_number = 8, int max_request = 10000); // 构造函数
    ~threadpool();                                                                         // 析构函数
    bool append(T *request);                                                               // 向请求队列中添加任务请求

private:
    // 工作线程运行的函数，它不断从工作队列中取出任务并执行之
    static void *worker(void *arg); //
    void run();

private:
    int m_thread_number;         // 线程池中的线程数
    int m_max_requests;          // 请求队列中允许的最大请求数
    pthread_t *m_threads;        // 描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue;  // 请求队列
    locker m_queuelocker;        // 保护请求队列的互斥锁
    sem m_queuestat;             // 是否有任务需要处理
    bool m_stop;                 // 是否结束线程
    connection_pool *m_connPool; // 指向数据库连接池的指针
};

template <typename T>
threadpool<T>::threadpool(connection_pool *connPool, int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL), m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0) // 线程数和请求队列中允许的最大请求数必须大于0
        throw std::exception();

    m_threads = new pthread_t[m_thread_number]; // 创建线程池中的线程数组

    if (!m_threads)
        throw std::exception(); // 创建线程数组失败

    for (int i = 0; i < thread_number; ++i)
    {
        // printf("create the %dth thread\n",i);
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) // 创建线程失败
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i])) // 分离线程在结束时会自动释放其资源，而无需显式调用 pthread_join 来等待它。
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop = true;
}

template <typename T>
bool threadpool<T>::append(T *request)
{
    m_queuelocker.lock(); // 操作工作队列时一定要加锁，因为它被所有线程共享

    if (m_workqueue.size() > m_max_requests) // 请求队列中的请求数超过了最大允许的数量
    {
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push_back(request); // 将任务添加到请求队列中

    m_queuelocker.unlock(); // 解锁
    m_queuestat.post();     // 信号量的值加1，表示有任务需要处理
    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg; // arg是传入的参数，即线程池对象，将void*转换为threadpool*
    pool->run();                          // 线程池对象调用run函数
    return pool;                          // 返回线程池对象，将threadpool*转换为void*
}

template <typename T>
void threadpool<T>::run()
{
    // 要明白工作线程池的工作模式
    // 每个线程都会执行下面的while循环，不断地从请求队列中取出任务并执行之
    // 也就是说通过信号量竞争互斥来控制线程的执行，没有采用Round Robin的方式
    while (!m_stop)
    {
        m_queuestat.wait();      // 等待信号量，当有任务需要处理时，将信号量减1，然后往下执行
        m_queuelocker.lock();    // 加锁
        if (m_workqueue.empty()) // 请求队列为空
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front(); // 取出请求队列中的第一个任务
        m_workqueue.pop_front();          // 删除第一个任务
        m_queuelocker.unlock();           // 解锁
        if (!request)                     // 任务为空
            continue;

        connectionRAII mysqlcon(&request->mysql, m_connPool); // 从连接池中取出一个数据库连接(T任务类有mysql成员)

        request->process(); // 执行任务
    }
}
#endif
