// 升序定时器链表
#ifndef LST_TIMER
#define LST_TIMER

#include <time.h>
#include "../log/log.h"

class util_timer; // 提前声明一下定时器链表上的结点类

// 定时器链表上的结点中会存放用户数据的数据结构
struct client_data
{
    sockaddr_in address; // 客户端socket地址
    int sockfd;          // socket文件描述符
    util_timer *timer;   // 指向定时器链表上的结点
};

// 定时器链表上的结点
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;                  // 任务超时时间（绝对时间）
    void (*cb_func)(client_data *); // 任务回调函数
    client_data *user_data;         // 回调函数处理的客户数据，由定时器执行者传递给回调函数
    util_timer *prev;               // 指向前一个定时器
    util_timer *next;               // 指向后一个定时器
};

// 升序定时器链表
class sort_timer_lst
{

private:
    util_timer *head; // 指向升序定时链表的头结点
    util_timer *tail; // 指向升序定时链表的尾结点

    // 一个重载的辅助函数，它被公有的add_timer函数和adjust_timer函数调用
    // 该函数表示将目标定时器timer添加到结点lst_head之后的部分链表
    void add_timer(util_timer *timer, util_timer *lst_head)
    {

        util_timer *prev = lst_head;  // 辅助变量，指向待插入结点的前一个结点
        util_timer *tmp = prev->next; // 辅助变量，指向待插入结点的后一个结点

        while (tmp)
        {
            if (timer->expire < tmp->expire)
            {
                // 寻找第一个超时时间大于待插入结点的定时器，将其插入到该定时器之前
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }

        if (!tmp)
        {
            // 遍历整个链表后都没找到，说明要将它插入到尾部
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }

public:
    // 构造函数初始化头结点和尾结点为NULL
    sort_timer_lst() : head(NULL), tail(NULL) {}

    // 析构函数删除所有定时器
    ~sort_timer_lst()
    {
        util_timer *tmp = head;
        while (tmp)
        {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }

    // 将定时器加入到链表中
    void add_timer(util_timer *timer)
    {
        // 定时器为NULL说明有问题，直接返回
        if (!timer)
        {
            return;
        }

        // 升序定时器链表中一个定时器都还没有
        if (!head)
        {
            head = tail = timer;
            return;
        }

        // 跟头结点的超时时间对比，比头结点小那么就成为新的头结点，返回
        if (timer->expire < head->expire)
        {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }

        // 其余情况，执行add_timer的另一个重载函数来插入定时器
        add_timer(timer, head);
    }

    // 当某个定时任务发生变化时，调整对应的定时器在链表中的位置
    // 注意：这个函数只考虑被调整的定时器的超时时间延长的情况，即待定时器需要往链表尾部移动
    void adjust_timer(util_timer *timer)
    {

        if (!timer)
        {
            return;
        }

        // 获取待调整定时器所指向的下一个定时器
        util_timer *tmp = timer->next;

        // 如果待调整定时器所指向的下一个定时器为NULL，或调整后的超时时间还是小于后面的那个定时器，则不用调整
        if (!tmp || (timer->expire < tmp->expire))
        {
            return;
        }

        // 如果待调整定时器是头结点，则将该定时器从链表中取出并重新插入链表
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer, head);
        }

        // 其余情况，将该定时器取出，插入到原来所在位置之后
        else
        {
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }

    // 将目标定时器timer从升序定时器链表中删除
    void del_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }

        // 目标定时器是升序定时器链表中的最后一个结点
        if ((timer == head) && (timer == tail))
        {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }

        // 目标定时器是头结点
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }

        // 目标定时器是尾结点
        if (timer == tail)
        {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }

        // 目标定时器是中间结点
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    // SIGALRM信号每次触发就在其信号处理函数中执行一次tick函数，以处理链表上到期的任务
    // 如果使用统一事件源，这个信号处理函数就是主函数
    void tick()
    {
        if (!head)
        {
            return;
        }

        // 每次执行tick()都写入日志
        LOG_INFO("%s", "timer tick");
        Log::get_instance()->flush();

        time_t cur = time(NULL); // 获取系统当前时间

        // 从头结点开始依次处理每个定时器，直到遇见一个尚未到期的定时器，这就是定时器的核心逻辑
        util_timer *tmp = head;
        while (tmp)
        {
            // 遇见一个尚未到期的定时器
            // 因为每个定时器都使用绝对时间作为超时值，所以我们可以把定时器的超时值和系统当前时间作比较，以此来判断定时器是否到期
            if (cur < tmp->expire)
            {
                break;
            }

            // 调用定时器的回调函数，以执行定时任务
            tmp->cb_func(tmp->user_data);

            // 执行完定时器中的定时任务后就将它从链表中删除，并重置头结点
            head = tmp->next;
            if (head)
            {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }
};

#endif
