#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;       // 设置读取文件的名称m_real_file大小
    static const int READ_BUFFER_SIZE = 2048;  // 设置读缓冲区m_read_buf大小
    static const int WRITE_BUFFER_SIZE = 1024; // 设置写缓冲区m_write_buf大小

    enum METHOD // 报文的请求方法，本项目只用到GET和POST
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };

    enum HTTP_CODE // 报文解析的结果
    {
        NO_REQUEST,  // 请求不完整，需要继续读取请求报文数据
        GET_REQUEST, // 获得了完整的HTTP请求
        BAD_REQUEST, // HTTP请求报文有语法错误
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR, // 服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
        CLOSED_CONNECTION
    };

    enum CHECK_STATE // 主状态机的状态
    {
        CHECK_STATE_REQUESTLINE = 0, // 解析请求行
        CHECK_STATE_HEADER,          // 解析请求头
        CHECK_STATE_CONTENT          // 解析消息体，仅用于解析POST请求
    };

    enum LINE_STATUS // 从状态机的状态
    {
        LINE_OK = 0, // 完整读取一行
        LINE_BAD,    // 报文语法有误
        LINE_OPEN    // 读取的行不完整
    };

public:
    static int m_epollfd;    // 存放的就是主线程里的那个epollfd
    static int m_user_count; // 计算http连接用户数量
    MYSQL *mysql;            // 存放分配给当前http_conn对象的mysql连接

private:
    int m_sockfd;          // 存放当前连接的socket文件描述符
    sockaddr_in m_address; // 存放当前socket的地址信息

    char m_read_buf[READ_BUFFER_SIZE]; // 存储读取的请求报文数据的缓冲区
    int m_read_idx;                    // 缓冲区中m_read_buf中数据的最后一个字节的下一个位置，一开始0
    int m_checked_idx;                 // m_read_buf当前读取的位置m_checked_idx
    int m_start_line;                  // m_read_buf中下一行数据的起点

    char m_write_buf[WRITE_BUFFER_SIZE]; // 存储发出的响应报文数据
    int m_write_idx;                     // 指示m_write_buf中有效数据的长度

    CHECK_STATE m_check_state; // 存放主状态机的状态
    METHOD m_method;           // 存放请求方法

    // 以下为解析请求报文中对应的6个变量
    char m_real_file[FILENAME_LEN]; // 存储读取文件的名称
    char *m_url;                    // 存储请求报文中的url
    char *m_version;                // 存储请求报文中的版本号
    char *m_host;                   // 存储请求报文中的主机名
    int m_content_length;           // 存储请求报文中的消息体的长度
    bool m_linger;                  // 判断是否要保持连接

    char *m_file_address;    // 读取服务器上的文件地址
    struct stat m_file_stat; // 存储读取文件的状态
    struct iovec m_iv[2];    // io向量机制iovec
    int m_iv_count;          // 表示被写内存块的数量
    int cgi;                 // 是否启用的POST
    char *m_string;          // 存储请求头数据
    int bytes_to_send;       // 剩余发送字节数
    int bytes_have_send;     // 已发送字节数

public:
    http_conn() {}
    ~http_conn() {}

    void init(int sockfd, const sockaddr_in &addr);   // 初始化新接受的连接
    sockaddr_in *get_address() { return &m_address; } // 获取当前连接的socket地址
    void close_conn(bool real_close = true);          // 关闭连接

    void process();   // 处理客户请求
    bool read_once(); // 非阻塞读操作
    bool write();     // 非阻塞写操作

    void initmysql_result(connection_pool *connPool); // 初始化数据库读取表

private:
    void init(); // 初始化新接受的连接后，再对一些private成员进行初始化

    HTTP_CODE process_read();          // 解析HTTP请求
    bool process_write(HTTP_CODE ret); // 填充HTTP应答

    HTTP_CODE parse_request_line(char *text); // 解析请求行
    HTTP_CODE parse_headers(char *text);      // 解析请求头
    HTTP_CODE parse_content(char *text);      // 解析消息体
    HTTP_CODE do_request();                   // 处理请求

    // 获取一行数据
    char *get_line()
    {
        return m_read_buf + m_start_line;
    };

    LINE_STATUS parse_line(); // 从状态机读取一行，分析是请求报文的哪一部分

    void unmap(); // 释放资源

    bool add_response(const char *format, ...);          // 向m_write_buf中写入待发送的数据
    bool add_content(const char *content);               // 向m_write_buf中写入响应报文的内容
    bool add_status_line(int status, const char *title); // 向m_write_buf中写入状态行
    bool add_headers(int content_length);                // 向m_write_buf中写入响应报文的头部
    bool add_content_type();                             // 向m_write_buf中写入响应报文的Content-Type
    bool add_content_length(int content_length);         // 向m_write_buf中写入响应报文的Content-Length
    bool add_linger();                                   // 向m_write_buf中写入响应报文的Connection
    bool add_blank_line();                               // 向m_write_buf中写入空行
};

#endif
