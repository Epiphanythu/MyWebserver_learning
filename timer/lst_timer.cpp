#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()
{
    // 创建头哑节点和尾哑节点
    head = new util_timer();
    tail = new util_timer();
    head->next = tail;
    tail->prev = head;
}
sort_timer_lst::~sort_timer_lst()
{
    // 释放所有定时器节点
    util_timer *tmp = head->next;
    while (tmp != tail)
    {
        util_timer *next = tmp->next;
        delete tmp;
        tmp = next;
    }
    // 释放哑节点
    delete head;
    delete tail;
}

void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    add_timer(timer, head);
}
// timer需要改大时调整
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next; // 当前节点的后一个
    if (tmp == tail || (timer->expire < tmp->expire)) // 尾部 或者已经是最大的了
    {
        return;
    }
    // 从链表中移除定时器
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    // 重新插入到合适位置
    add_timer(timer, head);
}
// 删除节点
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    // 从链表中移除定时器
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}
// 扫描已到期的定时器，执行回调函数
void sort_timer_lst::tick()
{
    time_t cur = time(NULL); // 获取当前时间
    util_timer *tmp = head->next;
    while (tmp != tail)
    {
        if (cur < tmp->expire)
        {
            break;
        }
        tmp->cb_func(tmp->user_data); // 超时回调
        // 保存下一个节点
        util_timer *next = tmp->next;
        // 从链表中移除并释放
        tmp->prev->next = next;
        next->prev = tmp->prev;
        delete tmp;
        tmp = next;
    }
}
// 实际插入
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp != tail)
    {
        if (timer->expire < tmp->expire)
        // 插入到 prev 和tmp 之间
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (tmp == tail) // 走到尾部
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = tail;
        tail->prev = timer;
    }
}
// 初始化时间槽
void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL); // 读取 fd 当前的文件状态标志
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option); // 写回
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        //  避免多线程竞争：一个连接同一时间只被一个线程处理
        //  防止重复触发：处理完前再决定是否重新注册
        //  适合线程池模型：工作线程处理完后主动"归还"连接
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig; // 传递信号值
    send(u_pipefd[1], (char *)&msg, 1, 0); // 将信号值发送到管道写端
    errno = save_errno;
}

//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}
