#include "http_conn.h"
#include "../log/log.h"
#include <map>
#include <mysql/mysql.h>
#include <fstream>

/* 定义http响应的一些状态信息 */
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

/* 网站根目录，文件夹内存放请求的资源和跳转的html文件 */
const char *doc_root = "/home/lfc/cpp_project/tiny_webserver/root";

map<string, string> users; /* 存放用户名和密码的map容器 */
locker m_lock; /* 用来干啥的锁？ */

/* 这两组有点傻逼，应该是重复了！ */
//#define connfdET 
#define connfdLT 
//#define listenfdET 
#define listenfdLT 

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

/* 对文件描述符设置非阻塞 */
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

/* 向内核事件表注册fd的相关事件 */
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;

/* 这两个不是重复了？？？？？？？？？ */
#ifdef connfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

#ifdef listenfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef listenfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

/* 从内核事件表删除描述符 */
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

/* 重置EPOLLONESHOT事件，为啥要重置去看下书 */
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

/* 关闭连接，关闭一个连接，客户总量减一 */
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

/* 初始化连接，保存socket文件描述符，socket地址，将本连接加入监听表
最后再初始化一些私有成员变量 */
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    //int reuse=1;
    //setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}

/* 初始化新接受的连接 
注意：check_state默认为分析请求行状态*/
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

/* 从状态机，用于提取出一行内容用于后续分析 */
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp; /* 存放当前分析字节的内容，临时变量 */
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx]; /* 一个字节一个字节的分析其中的内容 */
        
        /* 如果当前字符是'\r'回车符，则说明可能读取到一个完整的行 */
        if (temp == '\r')
        {
            /* 如果'\r'字符碰巧是目前buffer中的最后一个已经被读入的客户数据，那么这次分析没有读取到一个完整的行 */
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN; /* 返回LINE_OPEN以表示还需要继续读取客户数据才能进一步分析 */
            
            /* 如果下一个字符是'\n'，则说明我们成功读取到一个完整的行 */
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0'; /* '\r'替换成'\0' */
                m_read_buf[m_checked_idx++] = '\0'; /* '\n'替换成'\0' */
                return LINE_OK; /* 完整读取一行 */
            }
            return LINE_BAD;
        }

        /* 如果当前字符是'\n'换行符，则也说明可能读取到一个完整的行 */
        else if (temp == '\n')
        {
            /* 前一个字符要是'\r' */
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD; /* 出现语法错误，直接返回错误 */
        }
    }

    /* 如果所有字节分析完毕也没遇到'\r'或者'\n'，则返回LINE_OPEN，表示还需要继续读取客户数据才能进一步分析 */
    return LINE_OPEN;
}

/* 循环读取客户数据，直到无数据可读或出现异常
ET模式循环读取好理解，那LT呢？
我的理解是LT模式的循环是指epoll_wait会不断触发这个事件*/
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

#ifdef connfdLT

    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
    m_read_idx += bytes_read;

    if (bytes_read <= 0)
    {
        return false;
    }

    return true;

#endif

#ifdef connfdET
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;
        }
        else if (bytes_read == 0)
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
#endif
}

/* 解析http请求行，获得请求方法，目标url及http版本号
解析完成后主状态机的状态变为CHECK_STATE_HEADER */
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    /* 在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔
    请求行中最先含有空格' '和'\t'任一字符的位置并返回，如果没有空格或\t，则报文格式有误*/
    m_url = strpbrk(text, " \t");
    if (!m_url){ return BAD_REQUEST; } 

    *m_url++ = '\0'; /* 将该位置改为'\0'，用于将前面数据取出 */
    
    char *method = text; /* 取出请求方法，并通过与GET和POST比较，以确定请求方式 */
    if (strcasecmp(method, "GET") == 0){ m_method = GET;}
    else if (strcasecmp(method, "POST") == 0){ m_method = POST; cgi = 1; }
    else{ return BAD_REQUEST; }

    /* size_t strspn(const char *str1, const char *str2)
    该函数返回 str1 中第一个不在字符串 str2 中出现的字符下标。 
    m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有，所以调用strspn函数将m_url向后偏移，
    即通过查找，跳过空格和\t字符，指向请求资源的第一个字符*/
    m_url += strspn(m_url, " \t");

    /* 使用与前面判断「请求方式」的相同逻辑，判断HTTP版本号 */
    m_version = strpbrk(m_url, " \t");
    if (!m_version) return BAD_REQUEST;
    
    *m_version++ = '\0'; /* 将该位置改为'\0'，用于将前面数据取出 */

    /* 假设GET请求为GET /562f25980001b1b106000338.jpg HTTP/1.1
    此时m_url已经取出中间部分的/562f25980001b1b106000338.jpg了 */

    m_version += strspn(m_version, " \t"); /* 此时m_version已经取出HTTP/1.1了 */

    /* 仅支持HTTP/1.1 */
    if (strcasecmp(m_version, "HTTP/1.1") != 0) return BAD_REQUEST;
    
    /* 对请求资源前7个字符进行判断
    这里主要是有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理 */
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
        /* char *strchr(const char *str, int c)
        如果在字符串 str 中找到字符 c，则函数返回指向该字符的指针，如果未找到该字符则返回 NULL。 */
    }

    /* 类似地，增加https情况 */
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
        /* char *strchr(const char *str, int c)
        如果在字符串 str 中找到字符 c，则函数返回指向该字符的指针，如果未找到该字符则返回 NULL。 */
    }

    /* 异常情况处理 */
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;

    /* 当url为/时，显示欢迎界面 */
    if (strlen(m_url) == 1) 
    {
        /* char *strcat(char *dest, const char *src)
        dest -- 指向目标数组，该数组包含了一个 C 字符串，且足够容纳追加后的字符串。
        src -- 指向要追加的字符串，该字符串不会覆盖目标字符串。
        该函数返回一个指向最终的目标字符串 dest 的指针 */
        strcat(m_url, "judge.html");
    }

    /* 解析完请求行后，主状态机继续分析请求头 */
    m_check_state = CHECK_STATE_HEADER;

    return NO_REQUEST;
}

/* 请求头和空行的处理函数 */
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    /* 请求头和空行的处理使用的同一个函数，这里通过判断当前的text首位是不是\0字符，
    若是，则表示当前处理的是空行，若不是，则表示当前处理的是请求头。 */
    if (text[0] == '\0')
    {
        /* 若是空行，进而判断content-length是否为0，如果不是0，表明是POST请求，则状态转移到CHECK_STATE_CONTENT，
        否则说明是GET请求，则报文解析结束。 */
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }

    /* 若解析的是请求头部字段，则主要分析connection字段，content-length字段，
    其他字段可以直接跳过，各位也可以根据需求继续分析。 */


    /* connection字段判断是keep-alive还是close，决定是长连接还是短连接
    strncasecmp判断字符串指定长度是否相等的函数，忽略大小写 */
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {

        text += 11;
        text += strspn(text, " \t"); /* 跳过空格和\t字符 */
        
        /* strncasecmp判断字符串是否相等的函数，忽略大小写 */
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }

    /* content-length字段，这里用于读取post请求的消息体长度 */
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }

    /* 解析请求头部HOST字段 */
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }

    /* 忽略其它头部字段 */
    else
    {
        //printf("oop!unknow header: %s\n",text);
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }

    return NO_REQUEST;
}


/* GET和POST请求报文的区别之一是有无消息体部分，GET请求没有消息体，当解析完空行之后，便完成了报文的解析。

但后续的登录和注册功能，为了避免将用户名和密码直接暴露在URL中，
我们在项目中改用了POST请求，将用户名和密码添加在报文中作为消息体进行了封装。 */
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    /* 判断buffer中是否读取了消息体 */
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0'; /* 让消息体字符串以'\0'结尾 */
        m_string = text; // ？？POST请求中最后为输入的用户名和密码
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

/* process_read通过while循环，将主从状态机进行封装，对报文的每一行进行循环处理 */
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    /* m_check_state的默认值是CHECK_STATE_REQUESTLINE，见init()函数
    所以第一次进入循环取决于(line_status = parse_line()) == LINE_OK */
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
     ((line_status = parse_line()) == LINE_OK)) 
    {
        text = get_line(); /* 因为parse_line()中把'\r'和'\n'替换成了'\0'，所以这里得到的text就是一行内容 */
        m_start_line = m_checked_idx; /* 重置m_start_line的位置，下一次就是下一行的起点了 */
        
        LOG_INFO("%s", text); /* 写日志 */
        Log::get_instance()->flush();

        switch (m_check_state)
        {
            /* 第1次循环会先进入这里，解析请求行 */
            case CHECK_STATE_REQUESTLINE:
            {
                /* 只要没出现解析异常就可以申请继续读取数据进一步分析了 */
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }

            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                else if (ret == GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }

            /* (line_status = parse_line()) == LINE_OK)不会执行，因为左边已经是true了，短路求值
            
            此时消息体内容还是用text = get_line()来获取，不用担心会不会只返回半截消息体，因为
            get_line函数是return m_read_buf + m_start_line，也就是说它是返回空行后面的所有内容*/
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if (ret == GET_REQUEST)
                    return do_request();
                line_status = LINE_OPEN; /* 完成消息体解析后，将line_status变量更改为LINE_OPEN，此时可以跳出循环，完成报文解析任务。 */
                break;
            }
            
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

/* 回应客户端的请求 */
http_conn::HTTP_CODE http_conn::do_request()
{
    /* 将初始化的m_real_file赋值为网站根目录 */
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    
    /* 找到m_url中/的位置 */
    const char *p = strrchr(m_url, '/'); 

    //处理cgi, 据/后的第一个字符判断是登录还是注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        /* 

        /2CGISQL.cgi POST请求，进行登录校验
        /3CGISQL.cgi POST请求，进行注册校验
        
        */

        /* 下面这段没有起到实际作用 */
        /* 下面这段没有起到实际作用 */
        char flag = m_url[1]; /* 后面没用到这个变量... */

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2); /* m_url为啥+2，是指/2xxx或/3xxx，第3个字符开始才是请求文件名的意思吗？ */
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);
        /* 上面这段没有起到实际作用 */
        /* 上面这段没有起到实际作用 */
        

        /* 将用户名和密码提取出来
        user=123&passwd=123 */
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        /* 用户注册 */
        if (*(p + 1) == '3')
        {
            /* 准备好mysql语句 */
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            /* 先检测数据库中是否有重名的
            表中没有，进行注册，将数据写入到数据库中 */
            if (users.find(name) == users.end())
            {
                /* 向数据库中插入数据时，需要通过锁来同步数据 */
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password)); /* 同时更新users那个map容器 */
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }

        /* 用户登录
        若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0 */
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    /* 跳转注册页面，GET */
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    /* 跳转登录页面，GET */
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    /* 显示图片页面，POST */
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    /* 显示视频页面，POST */
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    /* 显示关注页面，POST */
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    /* 否则发送url实际请求的文件
    对应前面if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))的解析内容
    
    注：两元社上的解析不太对，如果以上均不符合，即不是登录和注册，也不是其余情况，那么直接将url与网站目录拼接
    这时是welcome界面？？？请求服务器上的一个图片？？我觉得不太对 */
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    /* 通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    失败返回NO_RESOURCE状态，表示资源不存在 */
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    /* 判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态 */
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    /* 判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误 */
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    /* 以只读方式获取文件描述符，通过mmap将该文件映射到内存中 */
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    return FILE_REQUEST; /* 表示请求文件存在，且可以访问 */
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

/* 服务器子线程调用process_write完成响应报文，随后注册epollout事件（这个注册在process()函数中进行）。
服务器主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器端。 */
bool http_conn::write()
{
    int temp = 0;

    /* 若要发送的数据长度为0, 表示响应报文为空，一般不会出现这种情况 */
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }


    while (1)
    {
        /* 将响应报文的状态行、消息头、空行和响应正文发送给浏览器端 */
        temp = writev(m_sockfd, m_iv, m_iv_count);

        
        if (temp < 0)
        {
            if (errno == EAGAIN) /* 判断缓冲区是否满了 */
            {
                /* 重新注册写事件，等待重发 */
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }

            /* 其它情况属于异常，取消内存映射，准备关闭连接 */
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp; /* 第一次调用其实bytes_to_send大小不变，因为temp=0 */

        if (bytes_have_send >= m_iv[0].iov_len) /* 第一个iovec头部信息的数据已发送完，发送第二个iovec数据 */
        {
            m_iv[0].iov_len = 0; /* 不再继续发送头部信息 */
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else /* 继续发送第一个iovec头部信息的数据 */
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        /* 判断条件，数据已全部发送完 */
        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN); /* 在epoll树上重置EPOLLONESHOT事件 */

            if (m_linger) /* 浏览器的请求为长连接 */
            {
                init(); /* 重新初始化HTTP对象 */
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

/* 下面几个函数，均是内部调用add_response函数更新m_write_idx指针和缓冲区m_write_buf中的内容 */
bool http_conn::add_response(const char *format, ...)
{
    /* 如果写入内容超出m_write_buf大小则报错 */
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    
    va_list arg_list; /* 定义可变参数列表 */
    va_start(arg_list, format); /* 将变量arg_list初始化为传入参数 */

    /* 将数据format从可变参数列表写入缓冲区写，返回写入数据的长度 */
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    
    /* 如果写入的数据长度超过缓冲区剩余空间，则报错 */
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }

    m_write_idx += len; /* 更新m_write_idx位置 */

    va_end(arg_list); /* 清空可变参列表 */

    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();

    return true;
}

/* 添加状态行：http/1.1 状态码 状态消息 */
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

/* 添加消息报头，内部调用add_content_length和add_linger函数
content-length记录响应报文长度，用于浏览器端判断服务器是否发送完数据
connection记录连接状态，用于告诉浏览器端保持长连接 */
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

/* 添加Content-Length，表示响应报文的长度 */
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

/* 添加文本类型，这里是html
好像没用上？ */
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

/* 添加连接状态，通知浏览器端是保持连接还是关闭 */
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

/* 添加空行 */
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

/* 添加文本content */
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

/* 处理写入数据 */
bool http_conn::process_write(HTTP_CODE ret)
{
    /* 响应报文分为两种
    一种是请求文件的存在，通过io向量机制iovec声明两个iovec，分别指向m_write_buf，和mmap的地址m_file_address（要传输的文件，会放到消息体里？）
    另一种是请求出错，这时候只申请一个iovec，指向m_write_buf。 */
    switch (ret)
    {
        /* 内部错误，500 */
        case INTERNAL_ERROR:
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
                return false;
            break;
        }

        /* 报文语法有误，404 */
        case BAD_REQUEST:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }

        /* 资源没有访问权限，403 */
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }
        
        /* 文件存在，200 */
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);

            /* 如果请求的资源存在 */
            if (m_file_stat.st_size != 0)
            {
                add_headers(m_file_stat.st_size);

                /* 第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx */
                m_iv[0].iov_base = m_write_buf; /* iov_base指向一个缓冲区，这个缓冲区是存放的是writev将要发送的数据。 */
                m_iv[0].iov_len = m_write_idx; /* iov_len表示实际写入的长度 */
                
                /* 第二个iovec指针指向mmap返回的文件指针，长度指向文件大小 */
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;

                /* 发送的全部数据为响应报文头部信息和文件大小 */
                bytes_to_send = m_write_idx + m_file_stat.st_size; 
                
                return true;
            }

            /* 如果请求的资源大小为0，则返回空白html文件 */
            else
            {
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }

        default:
            return false;
    }

    /* 除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区 */
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;

    bytes_to_send = m_write_idx;

    return true;
}


/* 浏览器发出http连接请求，服务器端主线程创建http对象接收请求并将所有数据读入对应buffer，将该对象插入任务队列后，工作线程从任务队列中取出一个任务进行处理。

各子线程通过process函数对任务进行处理，调用process_read函数和process_write函数分别完成报文解析与报文响应两个任务。 */
void http_conn::process()
{
    /* 接收请求数据 */
    HTTP_CODE read_ret = process_read();
    
    /* NO_REQUEST，表示请求不完整，需要继续接收请求数据 */
    if (read_ret == NO_REQUEST)
    {
        /* 注册并监听读事件 */
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    /* 调用process_write完成报文响应 */
    bool write_ret = process_write(read_ret);
    if (!write_ret){ close_conn(); }

    /* 注册并监听写事件 */
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
