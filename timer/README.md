# 定时器模块

`timer` 模块的任务很明确：处理长时间不活跃的连接，把超时连接及时关闭，避免它们一直占着服务器资源不释放。

对应源码文件：

- [lst_timer.h](D:\MyWebserver_learning\timer\lst_timer.h)
- [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)

---

## 一、模块整体作用

在 Web 服务器里，客户端和服务器建立 TCP 连接之后，不一定会一直持续发送数据。

有些连接可能出现下面几种情况：

- 客户端已经退出，但服务器还没感知到
- 客户端建立了连接，却一直不发请求
- 请求处理完了，但连接一直空闲

如果这些连接长期存在，就会带来问题：

- 占用 `socket` 文件描述符
- 占用服务器保存连接状态的内存
- 连接数越来越多时，会拖慢整个服务器

所以需要一个“定时清理机制”：

1. 每个连接都绑定一个定时器。
2. 定时器里记录这个连接的超时时间。
3. 如果连接一直没有新的活动，到期后就执行回调函数，把它关闭。

这就是 `timer` 模块的核心工作。

---

## 二、整体设计思路

这个模块不是用“每个连接一个线程睡眠等待超时”的方式，而是采用：

- `SIGALRM` 周期性发出信号
- 信号通过管道通知主循环
- 主循环调用 `tick()` 扫描定时器链表
- 把已经超时的连接统一处理掉

可以把它理解成这样一条链：

```text
alarm() 定时触发
    ->
内核发送 SIGALRM
    ->
sig_handler() 捕获信号
    ->
把信号值写入管道
    ->
epoll 监听到管道可读
    ->
主循环调用 timer_handler()
    ->
timer_handler() 调用 tick()
    ->
tick() 处理所有已到期的连接
```

这里最关键的思想叫做“统一事件源”。

也就是说：

- 网络 I/O 事件本来就是由 `epoll` 统一监听
- 信号事件本来不属于普通 I/O
- 但这个项目把信号写进管道，就把“信号事件”也转成了“可读事件”

这样主循环就不用一会儿处理 I/O，一会儿又单独处理信号，而是都交给 `epoll` 来统一调度。

---

## 三、为什么这里用升序双向链表

定时器容器使用的是“按过期时间升序排列的双向链表”。

也就是：

- 链表头部 `head`：最早过期的定时器
- 链表尾部 `tail`：最晚过期的定时器

例如：

```text
head -> [expire=10] <-> [expire=20] <-> [expire=35] <-> [expire=60] <- tail
```

这样设计的好处是：

1. 检查超时时，只需要从表头开始看。
2. 一旦发现某个定时器还没过期，后面的也一定没过期。
3. 因为链表有序，所以 `tick()` 不需要把整个链表都扫完。

这对“超时检测”特别合适。

虽然插入新定时器可能要从前往后找位置，时间复杂度是 `O(n)`，但这个项目里定时器结构简单、实现直观，适合学习。

---

## 四、数据结构详解

### 4.1 `client_data`

代码在 [lst_timer.h](D:\MyWebserver_learning\timer\lst_timer.h)。

```cpp
struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};
```

这个结构体保存的是“一个客户端连接对应的数据”。

每个字段的作用：

- `address`
  记录客户端地址信息，也就是 IP 和端口。
- `sockfd`
  这个客户端连接对应的 socket 文件描述符。
- `timer`
  指向这个连接关联的定时器节点。

为什么这里要互相绑定？

因为：

- 连接需要知道自己的定时器，便于后续更新超时时间
- 定时器超时后，也要能反过来找到对应连接并关闭它

所以 `client_data` 和 `util_timer` 是配套使用的。

---

### 4.2 `util_timer`

代码在 [lst_timer.h](D:\MyWebserver_learning\timer\lst_timer.h)。

```cpp
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;
    void (* cb_func)(client_data *);
    client_data *user_data;
    util_timer *prev;
    util_timer *next;
};
```

这是“单个定时器节点”。

可以把它理解成链表中的一个节点，每个节点都代表“一项超时任务”。

各个成员含义如下。

#### `expire`

```cpp
time_t expire;
```

它表示这个定时器的到期时间。

注意这里不是“还剩几秒”，而是一个绝对时间点。

例如：

```cpp
timer->expire = time(NULL) + 3 * TIMESLOT;
```

含义是：

- `time(NULL)` 取当前时间
- 再往后加若干秒
- 得到一个真正的“到期时刻”

后面判断超时的时候，只要比较：

```cpp
cur >= timer->expire
```

就能知道是否过期。

#### `cb_func`

```cpp
void (* cb_func)(client_data *);
```

这是回调函数指针。

意思是：当定时器超时以后，不是定时器自己决定怎么处理，而是去调用这个函数。

这个函数接收一个 `client_data *` 参数，所以它能知道：

- 这个超时任务对应的是哪个客户端
- 该关闭哪个 `sockfd`

你可以把它理解成：

“时间到了之后，要执行的处理动作”。

#### `user_data`

```cpp
client_data *user_data;
```

这是定时器绑定的用户数据。

为什么回调函数需要它？

因为回调函数要知道自己在处理谁，而定时器节点本身只保存超时信息，不够用，所以还要关联对应连接的数据。

#### `prev` 和 `next`

```cpp
util_timer *prev;
util_timer *next;
```

这两个指针用于把所有定时器串成一个双向链表。

双向链表的好处是：

- 删除节点时更方便
- 调整节点位置时也更方便

构造函数里把它们初始化为 `NULL`：

```cpp
util_timer() : prev(NULL), next(NULL) {}
```

意思是刚创建出来时，它还没有接到链表里。

---

### 4.3 `sort_timer_lst`

代码在 [lst_timer.h](D:\MyWebserver_learning\timer\lst_timer.h)。

```cpp
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer *timer);
    void adjust_timer(util_timer *timer);
    void del_timer(util_timer *timer);
    void tick();

private:
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;
    util_timer *tail;
};
```

这个类就是“升序定时器链表”的管理者。

它负责四件事：

1. 添加新定时器
2. 调整已有定时器的位置
3. 删除某个定时器
4. 扫描并处理到期定时器

成员变量：

- `head`
  指向链表头，也就是最早过期的节点。
- `tail`
  指向链表尾，也就是最晚过期的节点。

因为链表始终按 `expire` 升序排列，所以：

- 越靠前，越早超时
- 越靠后，越晚超时

---

### 4.4 `Utils`

代码在 [lst_timer.h](D:\MyWebserver_learning\timer\lst_timer.h)。

```cpp
class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);
    int setnonblocking(int fd);
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);
    static void sig_handler(int sig);
    void addsig(int sig, void(handler)(int), bool restart = true);
    void timer_handler();
    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;
};
```

`Utils` 是一个工具类，它把和定时器配套的辅助功能也包进来了。

它除了管理定时器链表 `m_timer_lst`，还负责：

- 设置非阻塞
- 把 fd 加入 `epoll`
- 注册信号处理函数
- 处理定时任务入口

这里尤其要注意三个成员：

#### `u_pipefd`

```cpp
static int *u_pipefd;
```

这是管道描述符数组的指针，供信号处理函数使用。

信号处理函数是静态函数，不能直接访问普通成员变量，所以这里用静态成员来共享管道。

#### `m_timer_lst`

```cpp
sort_timer_lst m_timer_lst;
```

这是真正存放所有定时器的链表对象。

#### `u_epollfd`

```cpp
static int u_epollfd;
```

在超时回调里，要把连接从 `epoll` 中删除，所以也需要一个全局可访问的 `epoll` 文件描述符。

#### `m_TIMESLOT`

```cpp
int m_TIMESLOT;
```

表示定时器每次触发的时间间隔。

比如设成 `5`，就意味着每隔 5 秒触发一次 `SIGALRM`。

---

## 五、函数逐个详解

下面按照 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp) 里的源码顺序来讲。

---

### 5.1 `sort_timer_lst::sort_timer_lst()`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
```

这是构造函数，用来初始化链表。

刚创建链表时，里面没有任何节点，所以：

- `head = NULL`
- `tail = NULL`

表示当前链表为空。

如果不初始化，`head` 和 `tail` 里面可能是随机值，后面一访问就会出问题。

---

### 5.2 `sort_timer_lst::~sort_timer_lst()`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}
```

这是析构函数，用来在链表对象销毁时，把所有定时器节点释放掉。

执行过程如下：

1. 用 `tmp` 指向当前头节点。
2. 记录下下一个节点：`head = tmp->next`。
3. 删除当前节点：`delete tmp`。
4. 让 `tmp = head`，继续处理下一个。
5. 一直删到链表为空。

为什么这里从头开始删？

因为链表节点之间已经串起来了，从头往后删最自然，也最安全。

---

### 5.3 `sort_timer_lst::add_timer(util_timer *timer)`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);
}
```

这个函数负责把一个新定时器插入到链表中，并保持链表仍然是按过期时间升序排列。

我们按分支来看。

#### 第一种情况：`timer` 为空

```cpp
if (!timer)
{
    return;
}
```

如果传进来的指针本身就是空，说明没有可插入的定时器，直接返回。

这是最基本的防御式编程。

#### 第二种情况：当前链表为空

```cpp
if (!head)
{
    head = tail = timer;
    return;
}
```

如果当前链表一个节点都没有，那么这个新节点既是头，也是尾。

#### 第三种情况：新节点比头节点更早过期

```cpp
if (timer->expire < head->expire)
{
    timer->next = head;
    head->prev = timer;
    head = timer;
    return;
}
```

这说明新节点应该插到最前面。

执行后链表变成：

```text
timer <-> old_head <-> ...
```

这里要注意三步：

1. `timer->next = head`
   让新节点指向旧头节点。
2. `head->prev = timer`
   让旧头节点反过来指向新节点。
3. `head = timer`
   更新链表头指针。

#### 第四种情况：插入到中间或尾部

```cpp
add_timer(timer, head);
```

如果前面都不满足，说明它不该插到最前面，那就调用私有重载版本，从 `head` 开始往后找合适位置。

---

### 5.4 `sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)
    {
        if (timer->expire < tmp->expire)
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
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}
```

这个函数是真正做“查找插入位置”的地方。

它的思路很简单：

1. 从某个起点 `lst_head` 开始往后走。
2. 找到第一个“过期时间比新节点更晚”的节点。
3. 把新节点插到它前面。
4. 如果一直走到结尾都没找到，就说明新节点应该插到尾部。

下面按代码一步步看。

#### 初始化两个指针

```cpp
util_timer *prev = lst_head;
util_timer *tmp = prev->next;
```

这里：

- `prev` 表示当前考察位置的前一个节点
- `tmp` 表示当前正在比较的节点

这是一种很常见的链表插入写法，因为插入操作需要同时修改前后两个节点的指针。

#### 在链表中向后查找

```cpp
while (tmp)
{
    if (timer->expire < tmp->expire)
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
```

只要 `tmp` 还不为空，就继续往后找。

如果发现：

```cpp
timer->expire < tmp->expire
```

就说明：

- 新节点应该放在 `prev` 和 `tmp` 之间

于是把四个指针关系补好：

```cpp
prev->next = timer;
timer->next = tmp;
tmp->prev = timer;
timer->prev = prev;
```

这四句非常重要，缺任何一句，链表关系都会断掉。

#### 如果走到了链表尾部

```cpp
if (!tmp)
{
    prev->next = timer;
    timer->prev = prev;
    timer->next = NULL;
    tail = timer;
}
```

如果 `tmp == NULL`，说明已经走到最后都没找到比它更晚过期的节点。

那就说明这个新节点应该挂在尾部。

这时需要：

1. 让原尾部的 `next` 指向它
2. 它的 `prev` 指向原尾部
3. 它的 `next` 设为 `NULL`
4. 更新 `tail`

---

### 5.5 `sort_timer_lst::adjust_timer(util_timer *timer)`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}
```

这个函数很重要，也很容易看懵。

它的作用不是“修改超时时间”，而是：

当别的地方已经把 `timer->expire` 改大之后，这个函数负责把它重新放回链表中的正确位置。

为什么只考虑“改大”？

因为在这个项目里，连接一旦有新活动，通常会延长超时时间，也就是把 `expire` 往后推。

一旦 `expire` 变大，这个节点就可能应该往链表后面移动。

#### 先判空

```cpp
if (!timer)
{
    return;
}
```

还是一样，防止空指针。

#### 先看是否真的需要移动

```cpp
util_timer *tmp = timer->next;
if (!tmp || (timer->expire < tmp->expire))
{
    return;
}
```

这里的逻辑特别关键。

`tmp = timer->next` 表示当前节点的下一个节点。

然后判断：

1. `!tmp`
   说明它本来就在尾部，后面没有节点了，自然不需要往后移。
2. `timer->expire < tmp->expire`
   说明即使 `expire` 改过了，它还是比后一个节点更早过期，位置依然正确。

所以这两种情况都直接返回。

换句话说，只有当：

```cpp
timer->expire >= timer->next->expire
```

时，才说明当前位置已经不合适了，需要调整。

#### 情况一：当前节点就是头节点

```cpp
if (timer == head)
{
    head = head->next;
    head->prev = NULL;
    timer->next = NULL;
    add_timer(timer, head);
}
```

如果要移动的节点正好是头节点，那么先把它从头部摘下来。

执行过程：

1. `head = head->next`
   链表头往后挪一个。
2. `head->prev = NULL`
   新头节点前面没有节点了。
3. `timer->next = NULL`
   把原头节点从旧位置断开。
4. `add_timer(timer, head)`
   从新的头节点开始重新找位置。

这里为什么不用改 `timer->prev`？

因为它本来就是头节点，`prev` 本来就应该是 `NULL`。

#### 情况二：当前节点在中间

```cpp
else
{
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    add_timer(timer, timer->next);
}
```

如果它不是头节点，那就说明在中间或者更后面。

先把它从原链表中摘掉：

1. `timer->prev->next = timer->next`
2. `timer->next->prev = timer->prev`

这样它前后两个节点就直接连上了。

然后：

```cpp
add_timer(timer, timer->next);
```

从它原来的后继位置开始往后找插入点。

为什么可以从 `timer->next` 开始，而不是从头开始？

因为这里调整的是“过期时间变大”的情况。

既然只会往后移，那就没必要从头重新找，直接从原位置后面继续找就行，更高效。

---

### 5.6 `sort_timer_lst::del_timer(util_timer *timer)`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}
```

这个函数负责把某个定时器从链表中删除并释放内存。

删除链表节点时，最常见的难点就是：不同位置的节点处理方式不一样。

所以这里分成了四种情况。

#### 第一种：链表里只有这一个节点

```cpp
if ((timer == head) && (timer == tail))
```

说明当前节点既是头又是尾，也就是链表长度为 1。

删除后链表直接变空：

```cpp
head = NULL;
tail = NULL;
```

#### 第二种：删除头节点

```cpp
if (timer == head)
{
    head = head->next;
    head->prev = NULL;
    delete timer;
    return;
}
```

删除头节点之后：

- 让下一个节点成为新的 `head`
- 新头节点的 `prev` 要设为 `NULL`

#### 第三种：删除尾节点

```cpp
if (timer == tail)
{
    tail = tail->prev;
    tail->next = NULL;
    delete timer;
    return;
}
```

删除尾节点之后：

- 让前一个节点成为新的 `tail`
- 新尾节点的 `next` 要设为 `NULL`

#### 第四种：删除中间节点

```cpp
timer->prev->next = timer->next;
timer->next->prev = timer->prev;
delete timer;
```

最普通的情况就是把前后两个节点直接连起来。

---

### 5.7 `sort_timer_lst::tick()`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }
    
    time_t cur = time(NULL);
    util_timer *tmp = head;
    while (tmp)
    {
        if (cur < tmp->expire)
        {
            break;
        }
        tmp->cb_func(tmp->user_data);
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}
```

这是整个定时器模块最核心的函数。

它的作用是：

扫描链表中所有已经到期的定时器，并执行它们的回调函数。

#### 第一步：如果链表为空，直接返回

```cpp
if (!head)
{
    return;
}
```

没有任何定时器，就没必要处理。

#### 第二步：获取当前时间

```cpp
time_t cur = time(NULL);
```

`cur` 表示“现在这一刻的绝对时间”。

后面会拿它和每个节点的 `expire` 比较。

#### 第三步：从头节点开始扫描

```cpp
util_timer *tmp = head;
while (tmp)
```

因为链表是升序的，所以最先检查的永远是最早过期的那个。

#### 第四步：如果当前节点还没过期，就立刻停止

```cpp
if (cur < tmp->expire)
{
    break;
}
```

这一句体现了“升序链表”的价值。

因为一旦当前这个节点都还没过期，那么后面的节点只会更晚过期，所以整个扫描可以立即结束。

这比无序链表高效得多。

#### 第五步：执行超时回调

```cpp
tmp->cb_func(tmp->user_data);
```

这一步才是真正的“处理超时连接”。

也就是说：

- `tick()` 只负责发现超时
- 真正怎么处理，由回调函数 `cb_func` 决定

在当前项目里，回调函数的行为是关闭连接、从 `epoll` 中移除该 fd，并减少连接计数。

#### 第六步：把已处理节点从链表头删除

```cpp
head = tmp->next;
if (head)
{
    head->prev = NULL;
}
delete tmp;
tmp = head;
```

因为当前节点已经处理完了，所以直接从链表里删掉。

然后继续检查新的头节点是否也超时。

所以 `tick()` 的处理方式是：

- 只从头部连续删除已过期节点
- 遇到第一个未过期节点就停止

这个实现非常符合有序链表的特点。

---

### 5.8 `Utils::init(int timeslot)`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}
```

这个函数作用很简单：初始化时间槽大小。

也就是把外部传入的定时间隔保存到成员变量 `m_TIMESLOT` 里。

这个值后面会被 `alarm(m_TIMESLOT)` 使用。

例如：

- 如果 `m_TIMESLOT = 5`
- 就表示每隔 5 秒触发一次定时信号

---

### 5.9 `Utils::setnonblocking(int fd)`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}
```

这个函数用于把文件描述符设置为非阻塞。

为什么要非阻塞？

因为这个项目使用 `epoll` 配合高并发事件循环，如果一个 fd 是阻塞的，那么一次读写卡住，就会把整个线程拖住。

#### 第一句：获取原有标志

```cpp
int old_option = fcntl(fd, F_GETFL);
```

取出当前 fd 已经设置好的标志位。

#### 第二句：加上 `O_NONBLOCK`

```cpp
int new_option = old_option | O_NONBLOCK;
```

这里不是直接覆盖，而是在原有基础上“按位或”进去。

这样不会把原来的其他标志弄丢。

#### 第三句：写回新的标志

```cpp
fcntl(fd, F_SETFL, new_option);
```

此时这个 fd 就变成非阻塞了。

#### 返回原来的标志

```cpp
return old_option;
```

这样做是为了将来如果需要恢复原状态，还能知道原本是什么标志。

---

### 5.10 `Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}
```

这个函数用于把某个 fd 注册到 `epoll` 中监听。

它虽然在 `timer` 模块里，但实际上是服务于整个服务器的工具函数。

#### 创建 `epoll_event`

```cpp
epoll_event event;
event.data.fd = fd;
```

`event.data.fd` 表示：当这个事件就绪时，`epoll` 会把这个 fd 返回给你。

#### 根据触发模式设置监听事件

```cpp
if (1 == TRIGMode)
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
else
    event.events = EPOLLIN | EPOLLRDHUP;
```

这里监听的是读事件 `EPOLLIN`。

如果 `TRIGMode == 1`，就加上 `EPOLLET`，表示边沿触发。

不然就是默认的 LT 水平触发。

同时都加上了：

```cpp
EPOLLRDHUP
```

表示对端关闭连接时也能收到通知。

#### 根据需要开启 `EPOLLONESHOT`

```cpp
if (one_shot)
    event.events |= EPOLLONESHOT;
```

它的意思是：一个事件触发后，只让一个线程处理一次，处理完成前不再重复投递。

这样能避免同一个连接被多个线程同时操作。

#### 加入 `epoll`

```cpp
epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
```

把这个 fd 真正注册到内核的 `epoll` 事件表中。

#### 设为非阻塞

```cpp
setnonblocking(fd);
```

这是事件驱动服务器里的常规操作，尤其是 ET 模式下，非阻塞几乎是必须的。

---

### 5.11 `Utils::sig_handler(int sig)`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
void Utils::sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}
```

这个函数是信号处理函数。

它的作用不是直接处理业务，而是把信号值写入管道，通知主循环“有信号来了”。

这一段一定要重点理解。

#### 为什么信号处理函数里不能直接做复杂工作

因为信号处理函数是在异步环境里执行的。

在这里面很多函数都不安全，比如：

- 不能随便操作复杂数据结构
- 不能放心地调用很多库函数
- 否则容易出现重入问题

所以这里采用一种非常经典的写法：

- 信号处理函数只做一件很轻量的事情
- 那就是把信号值写到管道里
- 真正复杂的逻辑交给主循环处理

#### 保存 `errno`

```cpp
int save_errno = errno;
```

信号处理函数可能打断当前正常执行的代码。

如果在这里改坏了 `errno`，外部代码可能会读到错误的错误码。

所以先保存起来，结束前再恢复。

#### 把信号编号写进管道

```cpp
int msg = sig;
send(u_pipefd[1], (char *)&msg, 1, 0);
```

这里：

- `u_pipefd[1]` 是管道写端
- `sig` 是收到的信号值，比如 `SIGALRM`

写进管道后，管道读端就会变成“可读”，于是 `epoll_wait()` 就能感知到。

这里虽然 `msg` 是 `int`，但只发送了 1 个字节：

```cpp
send(..., 1, 0);
```

因为信号值本身通常很小，这里只取低字节就够用了。

#### 恢复 `errno`

```cpp
errno = save_errno;
```

这样信号处理对外部上下文影响更小。

---

### 5.12 `Utils::addsig(int sig, void(handler)(int), bool restart)`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
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
```

这个函数用于注册信号处理方式。

也就是告诉系统：

- 某个信号来了之后
- 应该由哪个函数去处理

#### 清空 `sigaction`

```cpp
struct sigaction sa;
memset(&sa, '\0', sizeof(sa));
```

先把结构体清零，避免里面带有随机垃圾值。

#### 指定处理函数

```cpp
sa.sa_handler = handler;
```

例如后面如果注册：

```cpp
addsig(SIGALRM, sig_handler);
```

那就表示 `SIGALRM` 到来时由 `sig_handler` 处理。

#### 设置 `SA_RESTART`

```cpp
if (restart)
    sa.sa_flags |= SA_RESTART;
```

这表示：

如果某些系统调用执行到一半被信号打断，内核会自动帮你重启它们。

这样可以减少因为信号中断导致的异常情况。

#### 屏蔽所有信号

```cpp
sigfillset(&sa.sa_mask);
```

意思是：在执行当前信号处理函数期间，把所有信号先屏蔽掉。

这样做的目的是避免信号处理过程再次被别的信号打断，降低重入风险。

#### 调用 `sigaction`

```cpp
assert(sigaction(sig, &sa, NULL) != -1);
```

真正把设置提交给内核。

如果失败，就触发断言。

---

### 5.13 `Utils::timer_handler()`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}
```

这个函数可以理解成“定时任务总入口”。

每当主循环发现管道里收到了 `SIGALRM` 对应的通知时，就会调用它。

它做两件事。

#### 第一件事：处理所有到期定时器

```cpp
m_timer_lst.tick();
```

也就是去链表里扫描并处理超时连接。

#### 第二件事：重新启动下一轮定时

```cpp
alarm(m_TIMESLOT);
```

`alarm()` 只会触发一次，不会自动循环。

所以每次收到 `SIGALRM` 并处理完后，都要重新调用一次 `alarm()`，这样服务器才能持续不断地周期检测超时连接。

这个细节非常重要。

如果不重新 `alarm()`，那整个超时机制只会运行一次。

---

### 5.14 `Utils::show_error(int connfd, const char *info)`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}
```

这个函数用于向客户端发送错误信息，然后关闭连接。

逻辑很直接：

1. `send()` 发送提示字符串
2. `close()` 关闭连接

它不是定时器本身的核心逻辑，但属于配套工具函数。

---

### 5.15 `int *Utils::u_pipefd = 0`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
int *Utils::u_pipefd = 0;
```

这是静态成员变量的定义。

它不是声明，而是“在 cpp 中真正分配存储位置”。

作用是给 `sig_handler()` 提供可访问的管道描述符。

因为静态函数不能直接访问对象的普通成员，所以这里必须使用静态成员。

---

### 5.16 `int Utils::u_epollfd = 0`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
int Utils::u_epollfd = 0;
```

这同样是静态成员变量定义。

它保存 `epoll` 的文件描述符，供超时回调 `cb_func()` 使用。

因为超时回调里需要执行：

```cpp
epoll_ctl(..., EPOLL_CTL_DEL, ...)
```

所以必须能拿到 `epollfd`。

---

### 5.17 `cb_func(client_data *user_data)`

代码在 [lst_timer.cpp](D:\MyWebserver_learning\timer\lst_timer.cpp)。

```cpp
void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}
```

这就是前面提到的“定时器超时后的回调函数”。

它的任务是：清理超时连接。

逐句来看。

#### 从 `epoll` 中移除该连接

```cpp
epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
```

如果一个连接已经超时并准备关闭，就不能继续留在 `epoll` 监听表里了。

所以先把它删除。

这里的三个关键信息是：

- `Utils::u_epollfd`
  当前服务器使用的 `epoll` 实例
- `EPOLL_CTL_DEL`
  表示删除事件
- `user_data->sockfd`
  要删除的那个连接

#### 断言 `user_data` 有效

```cpp
assert(user_data);
```

这个断言说明：回调函数必须拿到合法的连接数据。

不过从代码顺序上看，这句放在 `epoll_ctl` 后面，其实不算最严谨。

因为在进入 `epoll_ctl` 时已经先使用了 `user_data->sockfd`。

也就是说，如果 `user_data` 真是空指针，程序会先出错，断言来不及保护。

所以从“防御式写法”角度讲，更合理的顺序通常应当是先判断或断言，再使用。

不过当前这份源码就是这样写的，文档讲解以源码为准。

#### 关闭连接

```cpp
close(user_data->sockfd);
```

真正释放这个 socket 对应的系统资源。

#### 在线用户数减一

```cpp
http_conn::m_user_count--;
```

这个项目里维护了一个静态连接计数。

连接关闭之后，需要把这个计数减掉，保证统计正确。

---

## 六、模块运行流程

把整个 `timer` 模块串起来，你就能更容易理解它为什么这样写。

### 6.1 服务器启动阶段

服务器初始化时，一般会做这些事：

1. 创建管道 `pipefd`
2. 把管道读端加入 `epoll`
3. 注册信号处理函数 `addsig(SIGALRM, sig_handler)`
4. 设置时间槽 `init(timeslot)`
5. 调用 `alarm(timeslot)` 启动第一次定时

这样之后，每隔一段时间就会收到一次 `SIGALRM`。

---

### 6.2 新连接到来时

当有新的客户端连接到来时，会：

1. 为该连接创建 `client_data`
2. 创建一个新的 `util_timer`
3. 设置它的回调函数 `cb_func`
4. 设置它的过期时间 `expire`
5. 让 `client_data.timer = 这个定时器`
6. 把定时器加入 `m_timer_lst`

于是这个连接就被纳入了超时管理。

---

### 6.3 连接有活动时

如果某个连接收到了新的请求，或者发生了读写活动，说明它仍然活跃。

这时通常会：

1. 修改它对应定时器的 `expire`
2. 把超时时间往后延长
3. 调用 `adjust_timer()` 重新调整它在链表中的位置

为什么要调位置？

因为超时时间变大以后，它往往不应该继续待在原来的位置上，而应该向链表尾部移动。

---

### 6.4 定时信号到来时

当 `alarm()` 到时间后：

1. 内核发送 `SIGALRM`
2. `sig_handler()` 被调用
3. `sig_handler()` 把信号值写入管道
4. 管道读端变为可读
5. `epoll_wait()` 返回这个事件
6. 主循环调用 `timer_handler()`
7. `timer_handler()` 调用 `tick()`
8. `tick()` 处理所有已超时节点
9. `timer_handler()` 再次调用 `alarm(m_TIMESLOT)`

这就形成了一个周期循环。

---

### 6.5 连接超时时

如果 `tick()` 扫描到某个节点已经到期：

1. 调用 `cb_func(user_data)`
2. 从 `epoll` 中删除该连接
3. 关闭 `sockfd`
4. 连接计数减一
5. 删除这个定时器节点

到这里，这个超时连接就被彻底清理掉了。

---

## 七、初学者最容易卡住的点

### 7.1 为什么信号不直接在 `sig_handler()` 里处理

因为信号处理函数运行在异步上下文中，里面不能放心做复杂逻辑。

所以这里只做最轻的一步：

- 把信号写到管道里

然后让主循环在正常上下文中处理真正业务。

这就是“统一事件源”的核心。

---

### 7.2 为什么 `tick()` 只从头开始检查

因为链表按过期时间升序排列。

只要发现头部某个节点还没过期，后面所有节点也都不会过期。

所以 `tick()` 不需要全表扫描。

---

### 7.3 为什么 `adjust_timer()` 只往后调整

因为这个项目里常见场景是“连接有活动，所以延长超时时间”。

延长超时意味着 `expire` 变大。

`expire` 变大后，这个节点只可能往后移动，不可能往前移动。

所以它从原位置后面继续找插入点就行。

---

### 7.4 为什么这里不用最小堆、时间轮

不是不能用，而是当前项目为了教学和实现直观，选了更容易理解的升序链表。

它的优点是：

- 结构清晰
- 指针关系直观
- 便于学习“定时器 + 回调 + 统一事件源”的整体机制

---

## 八、这个模块你应该抓住什么

如果你是初学者，学这个模块时最应该抓住下面四点：

1. `util_timer` 不是线程，也不是系统定时器，它只是一个“记录超时任务的节点”。
2. `sort_timer_lst` 维护的是“按过期时间升序”的双向链表。
3. `tick()` 的职责是“发现谁超时了”，`cb_func()` 的职责是“超时后怎么处理”。
4. `SIGALRM + 管道 + epoll` 这套组合，是为了把信号事件也并入主循环统一处理。

只要这四点清楚了，整个 `timer` 模块就通了。

---

## 九、源码文件对应关系

```text
timer/
|- lst_timer.h
|  定义数据结构和类接口
|
|- lst_timer.cpp
|  实现链表操作、信号处理和超时回调
|
|- README.md
|  当前这份 timer 模块讲解文档
```
