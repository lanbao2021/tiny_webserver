#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

// 构造函数
connection_pool::connection_pool()
{
	this->CurConn = 0;	// 当前已使用的连接数
	this->FreeConn = 0; // 当前空闲的连接数
}

// 单例模式，获取数据库连接池对象
connection_pool *connection_pool::GetInstance()
{
	// 静态局部变量的特性：只会初始化一次，且只能在函数内部使用
	// 静态局部变量的生存周期：程序运行期间
	// 静态局部变量的线程安全性：线程安全
	static connection_pool connPool;
	return &connPool;
}

// 构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, unsigned int MaxConn)
{
	this->url = url;
	this->Port = Port;
	this->User = User;
	this->PassWord = PassWord;
	this->DatabaseName = DBName;

	lock.lock(); // 加锁。这里我觉得没必要加锁，因为这时候没有客户连接不是吗？但加了也无关紧要就是

	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;	   // 指向一个数据库连接的指针
		con = mysql_init(con); // 初始化数据库连接

		if (con == NULL) // 初始化失败
		{
			cout << "Error:" << mysql_error(con); // mysql_error()用于返回上一个函数调用的错误代码
			exit(1);							  // 退出程序
		}
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0); // 连接数据库

		if (con == NULL) // 连接失败
		{
			cout << "Error: " << mysql_error(con); // mysql_error()用于返回上一个函数调用的错误代码
			exit(1);							   // 退出程序
		}
		connList.push_back(con); // 将创建好的数据库连接放入list容器中
		++FreeConn;				 // 空闲连接数+1
	}

	reserve = sem(FreeConn);  // 初始化信号量为可用连接个数
	this->MaxConn = FreeConn; // 初始化最大连接个数

	lock.unlock(); // 解锁
}

// 当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL; // 指向一个数据库连接的指针

	if (0 == connList.size()) // 连接池中没有连接（这个或许加锁更好？）
		return NULL;		  // 返回空指针

	// 如果信号量的值为0，就阻塞等待，直到信号量的值大于0
	// 如果信号量的值大于0，就将信号量的值减1，然后立即返回
	reserve.wait();

	lock.lock(); // 访问数据库连接时要先拿到互斥锁

	con = connList.front(); // 取出一个连接
	connList.pop_front();	// list容器中删除一个连接

	--FreeConn; // 空闲连接数-1
	++CurConn;	// 当前使用连接数+1

	lock.unlock(); // 解锁

	return con; // 返回一个可用连接
}

// 释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)  // 传入的参数为空指针
		return false; // 返回false

	lock.lock(); // 访问数据库连接池时要先拿到互斥锁

	connList.push_back(con); // 放回list容器
	++FreeConn;
	--CurConn;

	lock.unlock(); // 解锁

	reserve.post(); // 如果有线程阻塞在信号量上，就唤醒它；否则，信号量的值加1

	return true;
}

// 销毁数据库连接池
void connection_pool::DestroyPool()
{

	lock.lock(); // 修改数据库连接时要先拿到互斥锁

	if (connList.size() > 0) // 连接池中有连接
	{
		list<MYSQL *>::iterator it; // 迭代器
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it; // 取出一个连接
			mysql_close(con); // 关闭连接
		}
		CurConn = 0;	  // 当前使用的连接数
		FreeConn = 0;	  // 空闲连接数
		connList.clear(); // 清空list

		lock.unlock(); // 连接池中有连接，解锁
	}

	lock.unlock(); // 连接池中没有连接，解锁
}

// 获取当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->FreeConn;
}

// 析构函数
connection_pool::~connection_pool()
{
	DestroyPool();
}

// connectionRAII类的构造函数
// 将数据库连接的获取与释放通过RAII机制封装，避免手动释放。
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool)
{
	// 这里需要注意的是，在获取连接时，通过有参构造对传入的参数进行修改。
	// 其中数据库连接本身是指针类型，所以参数需要通过双指针才能对其进行修改
	*SQL = connPool->GetConnection(); // 从连接池中取一个连接
	conRAII = *SQL;					  // 指向一个数据库连接的指针，用于析构时归还给连接池
	poolRAII = connPool;			  // 指向整个数据库连接池的指针
}

connectionRAII::~connectionRAII()
{
	poolRAII->ReleaseConnection(conRAII);
}