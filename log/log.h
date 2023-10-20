#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

// 日志类
class Log
{

private:
    char dir_name[128];               // 路径名
    char log_name[128];               // log文件名
    int m_split_lines;                // 日志最大行数
    int m_log_buf_size;               // 日志缓冲区大小
    long long m_count;                // 日志行数记录
    int m_today;                      // 因为按天分类,记录当前时间是那一天
    FILE *m_fp;                       // 打开log的文件指针
    char *m_buf;                      // 写缓冲区
    block_queue<string> *m_log_queue; // 阻塞队列
    bool m_is_async;                  // 是否同步标志位
    locker m_mutex;                   // 互斥锁，用来保护日志缓冲区

private:
    Log();                  // 单例模式，构造函数私有化，防止外部new
    virtual ~Log();         // 析构函数私有化，防止外部delete
    void *async_write_log() // 异步写日志方法
    {
        string single_log; // 单条日志

        while (m_log_queue->pop(single_log)) // 从阻塞队列中取出一条日志
        {
            m_mutex.lock(); // 加锁

            fputs(single_log.c_str(), m_fp); // 写入文件

            m_mutex.unlock(); // 解锁
        }
    }

public:
    // 对外接口

    // 获取单例对象（C++11以后，使用局部变量懒汉不用加锁）
    static Log *get_instance()
    {
        static Log instance; // C++11以后，局部静态变量线程安全
        return &instance;    // 返回单例对象的地址
    }

    // 异步写日志公有方法，调用私有方法async_write_log
    static void *flush_log_thread(void *args)
    {
        Log::get_instance()->async_write_log();
    }

    // 可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    // 将输出内容按照标准格式整理
    // 写入日志内容的函数，可变参数，最后一个参数为可变参数的个数
    void write_log(int level, const char *format, ...);

    // 强制刷新缓冲区
    void flush(void);
};

// 日志类中的方法都不会被其他程序直接调用（所以定义成public有啥用*.*)
// 下面四个可变参数宏提供了其他程序的调用方法，用于不同类型的日志输出
#define LOG_DEBUG(format, ...) Log::get_instance()->write_log(0, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) Log::get_instance()->write_log(1, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) Log::get_instance()->write_log(2, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) Log::get_instance()->write_log(3, format, ##__VA_ARGS__)

#endif
