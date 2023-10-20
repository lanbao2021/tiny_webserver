#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"

using namespace std;

class connection_pool
{
private:
	unsigned int MaxConn;  // 最大连接数
	unsigned int CurConn;  // 当前已使用的连接数
	unsigned int FreeConn; // 当前空闲的连接数

	locker lock;			// 多线程操作连接池会造成竞争，这里使用互斥锁进行同步
	list<MYSQL *> connList; // 连接池
	sem reserve;			// 使用信号量实现多线程争夺连接的同步机制

	string url;			 // 主机地址
	string Port;		 // 数据库端口号
	string User;		 // 登陆数据库用户名
	string PassWord;	 // 登陆数据库密码
	string DatabaseName; // 使用数据库名

private:
	connection_pool();	// 构造数据库连接池
	~connection_pool(); // 析构数据库连接池

public:
	// 外部接口
	static connection_pool *GetInstance(); // 单例模式，获取数据库连接池对象
	MYSQL *GetConnection();				   // 获取数据库连接
	bool ReleaseConnection(MYSQL *conn);   // 释放连接
	int GetFreeConn();					   // 获取连接
	void DestroyPool();					   // 销毁所有连接

	void init(string url, string User, string PassWord, string DataBaseName, int Port, unsigned int MaxConn); // 初始化数据库连接池
};

// 将数据库连接的获取与释放通过RAII机制封装，避免手动释放。
class connectionRAII
{

public:
	connectionRAII(MYSQL **con, connection_pool *connPool); // 从连接池中取一个连接
	~connectionRAII();										// 析构时，归还给连接池

private:
	MYSQL *conRAII;			   // 指向一个数据库连接的指针
	connection_pool *poolRAII; // 指向整个数据库连接池的指针
};

#endif
