#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

// 封装信号量的类
class sem 
{
public:
    // 封装了sem_init来进行初始化，二进制信号量
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0) // 返回0表示没出错
        {
            throw std::exception();
        }
    }

    // 封装了sem_init来进行初始化，重载
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0) // 返回0表示没出错
        {
            throw std::exception();
        }
    }

    // 封装了sem_destroy来销毁信号量
    ~sem()
    {
        sem_destroy(&m_sem);
    }

    // 等待信号量
    bool wait()
    {
        return sem_wait(&m_sem) == 0; // 返回0表示没出错
    }

    // 封装sem_post增加信号量的值
    bool post()
    {
        return sem_post(&m_sem) == 0; // 返回0表示没出错
    }

private:
    sem_t m_sem; // 存放信号量的值
};

// 封装互斥锁的类
class locker
{
public:
    // 创建并初始化互斥锁
    locker()
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0) // 返回0表示没出错
        {
            throw std::exception();
        }
    }

    // 销毁互斥锁
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }

    // 获取互斥锁
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0; // 返回0表示没出错
    }

    // 释放互斥锁
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0; // 返回0表示没出错
    }

    // 获取指向互斥锁的指针？
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex; // 存放互斥锁的值
};

// 封装条件变量的类
// 注意，这个跟书上的有一定差别，将互斥锁放在类外了
class cond
{
public:
    // 创建并初始化条件变量
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            // 注意，这个跟书上的有一定差别，将互斥锁放在类外了
            // pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }

    // 销毁条件变量
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }

    // 等待条件变量
    // 注意，这个跟书上的有一定差别，将互斥锁放在类外了
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        // pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, m_mutex);
        // pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }

    // 等待条件变量，带超时时间
    // 注意，这个跟书上的有一定差别，将互斥锁放在类外了
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
    {
        int ret = 0;
        // pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        // pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }

    // 唤醒等待条件变量的其中一个线程
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0;
    }

    // 唤醒等待条件变量的线程，广播
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    // static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond; // 存放条件变量的值
};

#endif
