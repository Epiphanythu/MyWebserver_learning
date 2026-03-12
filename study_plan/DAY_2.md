# Day 2：精读 WebServer，建立事件循环主干

## 今天的任务定义

今天只做一件事：把 `webserver.cpp` 这条主线读顺。

你今天不需要把所有模块读透，但必须搞清楚服务器启动后，主线程到底在做什么，事件来了以后怎么分流，读写事件又是怎么交给后续模块的。

---

## 今天必须精读的文件

- `webserver.h`
- `webserver.cpp`

建议顺序：

1. 先读 `webserver.h`
2. 再通读 `webserver.cpp`
3. 第二遍只盯关键函数和关键分支

---

## 第一遍：建立函数地图

第一遍不要卡细节，先把这些函数全部过一遍，知道它们各自负责什么：

- `WebServer::WebServer`
- `WebServer::~WebServer`
- `WebServer::init`
- `WebServer::trig_mode`
- `WebServer::log_write`
- `WebServer::sql_pool`
- `WebServer::thread_pool`
- `WebServer::eventListen`
- `WebServer::timer`
- `WebServer::adjust_timer`
- `WebServer::deal_timer`
- `WebServer::dealclientdata`
- `WebServer::dealwithsignal`
- `WebServer::dealwithread`
- `WebServer::dealwithwrite`
- `WebServer::eventLoop`

第一遍结束后，你至少要知道：

- 哪些函数是启动阶段调用的
- 哪些函数是事件循环里调用的
- 哪些函数和定时器有关

---

## 第二遍：按主链路精读

### 板块 1：构造函数和资源布局

重点函数：

- `WebServer::WebServer`
- `WebServer::~WebServer`

你要看清楚：

- `users` 为什么按 `MAX_FD` 分配
- `m_root` 是怎么拼出来的
- `users_timer` 为什么也是按 `MAX_FD` 分配
- 析构时释放了哪些资源

要达到的水平：

- 能解释为什么这个项目大量用“fd 下标直接索引对象数组”
- 能说出这类设计的优点和代价

建议思考：

- 如果连接数很少，但 `MAX_FD` 很大，这种设计的内存代价是什么

### 板块 2：配置落地

重点函数：

- `WebServer::init`
- `WebServer::trig_mode`

你要看清楚：

- `init()` 只是保存配置，没有做系统调用
- `trig_mode()` 如何把一个整数配置映射成两个触发模式字段

要达到的水平：

- 能明确区分“配置保存”和“资源初始化”
- 能脱离代码说出 `TRIGMode=0/1/2/3` 分别代表什么

### 板块 3：启动监听

重点函数：

- `WebServer::eventListen`

这是今天最核心的函数，必须拆开看。

你要逐段读：

1. 创建监听 socket
2. 配置 `linger`
3. 填充 `sockaddr_in`
4. 设置 `SO_REUSEADDR`
5. `bind`
6. `listen`
7. 初始化工具类
8. 创建 epoll 实例
9. 将 `listenfd` 加入 epoll
10. 建立 `socketpair`
11. 将管道读端加入 epoll
12. 注册信号处理
13. 启动 `alarm`
14. 把关键 fd 保存到 `Utils` 静态成员

你要达到的水平：

- 每一步都知道作用
- 能讲清为什么信号也要通过 fd 进入 epoll 主循环

建议重点盯的变量：

- `m_listenfd`
- `m_epollfd`
- `m_pipefd`
- `m_LISTENTrigmode`

建议断点位置：

- `eventListen()` 开头
- `bind()` 后
- `utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);`
- `socketpair(...)` 后

### 板块 4：新连接接入

重点函数：

- `WebServer::dealclientdata`
- `WebServer::timer`

你要看清楚：

- LT 模式下只 `accept` 一次
- ET 模式下循环 `accept`
- 新连接建立后，不只是 `accept` 完就结束了，还要初始化 `http_conn` 和定时器

你要达到的水平：

- 能讲清“接入一个新连接”在这个项目里包括哪些动作

建议重点盯的变量：

- `connfd`
- `client_address`
- `users[connfd]`
- `users_timer[connfd]`

建议断点位置：

- `int connfd = accept(...)`
- `timer(connfd, client_address);`

### 板块 5：事件循环总调度

重点函数：

- `WebServer::eventLoop`

你要把这个函数当成主循环调度器来读。

必须逐个分支看：

1. `sockfd == m_listenfd`
2. `EPOLLRDHUP | EPOLLHUP | EPOLLERR`
3. `sockfd == m_pipefd[0] && EPOLLIN`
4. `EPOLLIN`
5. `EPOLLOUT`

你要达到的水平：

- 看到任意一个分支，能说出它对应什么场景
- 知道这个分支会把流程推进到哪个函数

建议断点位置：

- `int number = epoll_wait(...)`
- `if (sockfd == m_listenfd)`
- `else if (events[i].events & EPOLLIN)`
- `else if (events[i].events & EPOLLOUT)`

### 板块 6：读写事件的分流入口

重点函数：

- `WebServer::dealwithread`
- `WebServer::dealwithwrite`

你今天先不要求把线程池完全读透，但必须看懂：

- 这两个函数是 Reactor 和模拟 Proactor 的分流入口
- 它们和定时器刷新有直接关系

你要达到的水平：

- 能说出主线程在这两个函数里实际做了什么
- 能看出来并发模型的分叉点就在这里

建议重点盯的变量：

- `m_actormodel`
- `timer`
- `users[sockfd].improv`
- `users[sockfd].timer_flag`

---

## 今天必须手写出来的调用链

你今天必须自己写出这 3 条调用链：

1. 启动链

`main -> WebServer::init -> WebServer::eventListen -> WebServer::eventLoop`

2. 新连接链

`eventLoop -> dealclientdata -> accept -> timer -> http_conn::init`

3. 读写事件分流链

`eventLoop -> dealwithread / dealwithwrite -> 后续线程池或直接读写`

---

## 今天结束时的验收标准

如果你学到位了，你应该能做到：

- 不看代码，描述 `eventListen()` 做了哪些初始化
- 不看代码，描述 `eventLoop()` 怎么区分 5 类事件
- 能讲出新连接接入后除了 `accept` 还做了什么
- 能说出 Reactor 和模拟 Proactor 在 `dealwithread/dealwithwrite` 这里如何分叉

如果你还做不到，就不要往后赶。

---

## 今日输出要求

今天的笔记不要写成泛泛总结，必须包含：

### 1. `eventListen()` 分步解释

每一步对应一个作用。

### 2. `eventLoop()` 分支图

把 5 类事件分支画出来。

### 3. 新连接处理图

从 `accept` 到定时器建立。

### 4. 你自己的疑问

比如：

- 为什么信号不用别的方式处理
- 为什么 `users` 是大数组
- 为什么 `listenfd` 和 `connfd` 的触发模式分开配置

