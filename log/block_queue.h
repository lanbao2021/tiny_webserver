#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"

using namespace std;

/****************************************************************************************/
/* STL容器都是非线程安全的，这是因为STL容器的设计者认为，线程安全的代价太高了，所以不提供线程安全的容器 */
/* 循环数组实现的阻塞队列，m_back = (m_back + 1) % m_max_size;                              */
/* 线程安全，每个操作前都要先加互斥锁，操作完后，再解锁。                                        */
/****************************************************************************************/

template <class T>
class block_queue // 循环数组 + 生产者消费者模型实现的线程安全的阻塞队列
{

private:
    locker m_mutex; // 互斥锁
    cond m_cond;    // 条件变量

    T *m_array;     // 指向循环数组的指针
    int m_size;     // 阻塞队列中当前元素的个数
    int m_max_size; // 阻塞队列中最多存放的元素个数
    int m_front;    // 队首元素的下标
    int m_back;     // 队尾元素的下标

public:
    // 构造函数，初始化私有成员
    block_queue(int max_size = 1000)
    {
        if (max_size <= 0) // 阻塞队列中最多存放max_size个元素
        {
            exit(-1); // 退出程序
        }

        m_max_size = max_size;     // 阻塞队列中最多存放max_size个元素
        m_array = new T[max_size]; // 创建循环数组
        m_size = 0;                // 阻塞队列中当前元素个数
        m_front = -1;              // 队首元素的下标
        m_back = -1;               // 队尾元素的下标
    }

    // 清除循环队列上的元素（不用真的删除，只要将相关索引重新初始化即可）
    void clear()
    {
        m_mutex.lock(); // 先获取互斥锁

        m_size = 0;   // 阻塞队列中当前元素个数
        m_front = -1; // 队首元素的下标
        m_back = -1;  // 队尾元素的下标

        m_mutex.unlock(); // 解锁
    }

    // 销毁循环阻塞队列
    ~block_queue()
    {
        m_mutex.lock(); // 先获取互斥锁

        if (m_array != NULL)  // 如果循环数组不为空
            delete[] m_array; // 释放循环数组

        m_mutex.unlock(); // 解锁
    }

    // 判断队列是否满了
    bool full()
    {
        m_mutex.lock(); // 先获取互斥锁

        if (m_size >= m_max_size) // 队列满了
        {
            m_mutex.unlock(); // 解锁
            return true;      // 返回true
        }

        m_mutex.unlock(); // 解锁

        return false;
    }

    // 判断队列是否为空
    bool empty()
    {
        m_mutex.lock(); // 先获取互斥锁

        if (0 == m_size) // 队列为空
        {
            m_mutex.unlock(); // 解锁
            return true;      // 返回true
        }

        m_mutex.unlock(); // 解锁

        return false;
    }

    // 返回队首元素
    bool front(T &value)
    {
        m_mutex.lock(); // 先获取互斥锁

        if (0 == m_size)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front]; // 返回队首元素
        m_mutex.unlock();         // 解锁

        return true;
    }

    // 返回队尾元素
    bool back(T &value)
    {
        m_mutex.lock(); // 先获取互斥锁

        if (0 == m_size) // 队列为空
        {
            m_mutex.unlock(); // 解锁
            return false;
        }

        value = m_array[m_back]; // 返回队尾元素

        m_mutex.unlock(); // 解锁

        return true;
    }

    int size()
    {
        int tmp = 0; // ！！！！因为不能直接返回m_size，否则互斥锁不好处理

        m_mutex.lock(); // 加锁

        tmp = m_size; // 获取返回队列中元素的个数

        m_mutex.unlock(); // 解锁

        return tmp;
    }

    int max_size()
    {
        int tmp = 0; // ！！！因为不能直接返回m_max_size，否则互斥锁不好处理

        m_mutex.lock(); // 先获取互斥锁

        tmp = m_max_size; // 获取返回队列中最多存放的元素个数

        m_mutex.unlock(); // 解锁

        return tmp;
    }

    // 往队列添加元素，需要将所有使用队列的线程先唤醒
    // 当有元素push进队列,相当于生产者生产了一个元素
    // 若当前没有线程等待条件变量,则唤醒无意义
    bool push(const T &item)
    {
        m_mutex.lock(); // 先获取互斥锁

        if (m_size >= m_max_size) // 队列满了
        {

            m_cond.broadcast(); // 唤醒所有线程
            m_mutex.unlock();   // 解锁
            return false;       // 返回false
        }

        m_back = (m_back + 1) % m_max_size; // 队尾元素的下标
        m_array[m_back] = item;             // 将item赋值给队尾元素

        m_size++; // 队列中当前元素个数加1

        m_cond.broadcast(); // 唤醒所有线程
        m_mutex.unlock();   // 解锁
        return true;
    }

    // pop时，如果当前队列没有元素，将会等待条件变量
    bool pop(T &item)
    {

        m_mutex.lock(); // 先获取互斥锁

        while (m_size <= 0) // 之所以用while，是因为pthread_cond_wait()可能会意外唤醒
        {
            if (!m_cond.wait(m_mutex.get())) // 这里多了一层封装要注意
            {
                // m_cond.wait封装了pthread_cond_wait()，而m_cond.wait的return语句是ret == 0
                // 也就是说pthread_cond_wait()正确返回0时(等到了生产者生产），m_cond.wait返回1，而!1为false，所以会跳出循环，正常pop
                m_mutex.unlock(); 
                return false;     
            }
        }

        m_front = (m_front + 1) % m_max_size; // 队首元素的下标
        item = m_array[m_front];              // 将队首元素赋值给item
        m_size--;                             // 队列中当前元素个数减1

        m_mutex.unlock(); // 解锁

        return true;
    }

    // 增加了超时处理
    bool pop(T &item, int ms_timeout)
    {
        struct timespec t = {0, 0};
        struct timeval now = {0, 0};
        gettimeofday(&now, NULL);

        m_mutex.lock(); // 先获取互斥锁

        if (m_size <= 0)
        {
            t.tv_sec = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000;
            if (!m_cond.timewait(m_mutex.get(), t))
            {
                m_mutex.unlock();
                return false;
            }
        }

        if (m_size <= 0) // 二重if的原因是pthread_cond_timedwait()可能会意外唤醒
        {
            m_mutex.unlock();
            return false;
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;

        m_mutex.unlock();

        return true;
    }
};

#endif
