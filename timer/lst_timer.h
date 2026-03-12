#ifndef LST_TIMER
#define LST_TIMER

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

#include <time.h>
#include "../log/log.h"

class util_timer;

struct client_data 
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};

class util_timer // 定时器节点
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire; //任务的超时时间，这里使用绝对时间
    
    void (* cb_func)(client_data *); //任务回调函数，回调函数处理的对象是一个client_data类型的变量，这个变量包含了被定时器绑定的客户数据
    client_data *user_data; //客户数据
    util_timer *prev; //指向前一个定时器
    util_timer *next;
};

class sort_timer_lst // 定时器链表，升序、双向链表
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer *timer);
    void adjust_timer(util_timer *timer);
    void del_timer(util_timer *timer);
    void tick(); // SIGALRM信号每次被触发就在其信号处理函数中执行一次tick函数，以处理链表上到期的定时器

private:
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head; // 链表头，头结点是链表的第一个定时器
    util_timer *tail; //链表尾，尾结点是链表的最后一个定时器

};

class Utils // 工具类，主要是一些工具函数
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot); // 初始化时间槽

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd; //管道文件描述符，u_pipefd[0]用于监听信号，u_pipefd[1]用于写入数据
    sort_timer_lst m_timer_lst; // 定时器链表
    static int u_epollfd; // epoll文件描述符，通用事件源，后面称为统一事件源，所有socket上的事件都被注册到epoll上，epoll通过u_epollfd标识监听哪个epoll实例
    int m_TIMESLOT; //最小超时单位，单位是秒
};

void cb_func(client_data *user_data);

#endif
