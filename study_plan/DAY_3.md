# Day 3：精读线程池和并发模型

## 今天的任务定义

今天把“主线程把任务交出去之后发生了什么”读明白。

昨天你已经知道事件从 epoll 里出来之后会进入 `dealwithread()` / `dealwithwrite()`。今天要继续往下跟，看到线程池里到底怎么执行，以及 Reactor 和模拟 Proactor 两条路径到底差在哪。

---

## 今天必须精读的文件

- `threadpool/threadpool.h`
- `webserver.cpp`

建议顺序：

1. 先把 `threadpool.h` 从头读一遍
2. 再回到 `webserver.cpp` 对照 `dealwithread()` / `dealwithwrite()`
3. 最后把两种模型分别画一张调用链图

---

## 第一遍：线程池骨架

先把这些成员和函数过一遍：

- `m_thread_number`
- `m_max_requests`
- `m_threads`
- `m_workqueue`
- `m_queuelocker`
- `m_queuestat`
- `m_connPool`
- `m_actor_model`
- `threadpool(...)`
- `~threadpool()`
- `append`
- `append_p`
- `worker`
- `run`

第一遍结束后，你要知道：

- 线程池如何保存任务
- 线程如何被创建
- 任务如何被唤醒执行

---

## 第二遍：按函数精读

### 板块 1：构造函数

重点函数：

- `threadpool<T>::threadpool`

你要看清楚：

- 线程数和最大请求数的合法性检查
- 线程数组是怎么分配的
- `pthread_create` 创建了什么入口函数
- 为什么线程一创建就 `pthread_detach`

要达到的水平：

- 能解释这个线程池为什么不需要显式 `join`
- 能说明 detached 线程的好处和限制

建议断点位置：

- `pthread_create(...)`

### 板块 2：入队接口

重点函数：

- `threadpool<T>::append`
- `threadpool<T>::append_p`

你要看清楚：

- 两个接口都做了哪些相同动作
- 哪一步开始不同
- `request->m_state = state` 为什么只在 `append()` 里有

要达到的水平：

- 能明确回答：
  - 哪个接口是给 Reactor 用的
  - 哪个接口是给模拟 Proactor 用的

建议盯住：

- `m_workqueue.push_back(request);`
- `m_queuestat.post();`

### 板块 3：工作线程入口

重点函数：

- `threadpool<T>::worker`

你要看清楚：

- 为什么它必须是静态函数
- 为什么里面只是把 `this` 转回线程池对象再调 `run()`

要达到的水平：

- 能解释 pthread 风格线程入口与 C++ 成员函数之间的限制关系

### 板块 4：真正的任务执行循环

重点函数：

- `threadpool<T>::run`

这是今天最核心的函数，必须分两部分读。

#### 先看公共部分

你要看清楚：

- `m_queuestat.wait()` 如何阻塞线程
- 为什么出队前要加锁
- 为什么出队后马上解锁
- 空任务为什么直接跳过

你要达到的水平：

- 能讲清楚线程池并发取任务时为什么必须锁队列

#### 再看 Reactor 分支

条件：

- `if (1 == m_actor_model)`

你要逐行看：

1. `request->m_state` 是读还是写
2. 如果是读事件，工作线程调用 `read_once()`
3. 读成功后获取数据库连接并调用 `process()`
4. 失败时设置 `timer_flag`
5. 如果是写事件，工作线程直接 `write()`

你要达到的水平：

- 能描述 Reactor 模式下，读写动作为什么落在工作线程
- 能解释 `improv` 和 `timer_flag` 是怎么给主线程反馈结果的

建议断点位置：

- `if (1 == m_actor_model)`
- `if (0 == request->m_state)`
- `if (request->read_once())`
- `request->process();`

#### 再看模拟 Proactor 分支

条件：

- `else`

你要看清楚：

- 这里没有再次 `read_once()`
- 线程池主要做的是拿数据库连接然后 `process()`

你要达到的水平：

- 能解释为什么主线程先读完数据后，线程池就只剩业务处理

建议断点位置：

- `connectionRAII mysqlcon(&request->mysql, m_connPool);`
- `request->process();`

---

## 第三遍：回到 `webserver.cpp` 对照模型分叉点

你现在要重新看：

- `WebServer::dealwithread`
- `WebServer::dealwithwrite`

必须对照线程池再看一次。

### Reactor 路径

你要明确：

- 主线程：刷新定时器，投递任务
- 工作线程：真正做读或写
- 主线程：等 `improv` / `timer_flag` 结果

### 模拟 Proactor 路径

你要明确：

- 主线程：先 `read_once()`
- 主线程：读取成功后再投递到线程池
- 工作线程：主要执行 `process()`

---

## 今天必须手写出来的两条调用链

### 1. Reactor 调用链

`epoll_wait -> dealwithread/dealwithwrite -> threadpool::append -> threadpool::run -> read_once/write/process`

### 2. 模拟 Proactor 调用链

`epoll_wait -> dealwithread -> read_once -> threadpool::append_p -> threadpool::run -> process`

---

## 今天适合重点思考的问题

- 为什么 Reactor 模式里工作线程做 `read_once()`，而模拟 Proactor 不是
- 为什么 Reactor 分支里要有 `improv` 和 `timer_flag`
- 为什么线程池里也会碰数据库连接池
- 如果任务队列满了，这个项目现在是怎么处理的

---

## 今天结束时的验收标准

你必须做到：

- 不看代码，讲出线程池从入队到执行的完整流程
- 清楚区分 `append()` 和 `append_p()`
- 画出 Reactor 和模拟 Proactor 两条真实代码路径
- 解释“半同步/半反应堆”在这个项目里不是抽象概念，而是主线程和工作线程的具体分工

---

## 今日输出要求

今天的笔记必须包含：

### 1. 线程池结构图

成员之间的关系写清楚。

### 2. Reactor 执行链

从 epoll 到线程池再到 `http_conn`。

### 3. 模拟 Proactor 执行链

重点写清“主线程先读”。

### 4. 两种模型对比

至少比较：

- 谁读
- 谁写
- 谁做业务
- 主线程负担
- 工作线程负担

