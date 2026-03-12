# Day 5：精读数据库连接池和登录注册逻辑

## 今天的任务定义

今天把数据库这一层和业务这一层串起来。

你要真正回答这句话：浏览器发一个登录 POST 请求，代码到底怎么一步步走到 MySQL，又怎么决定返回哪个页面。

---

## 今天必须精读的文件

- `CGImysql/sql_connection_pool.h`
- `CGImysql/sql_connection_pool.cpp`
- `http/http_conn.cpp`

建议顺序：

1. 先读连接池
2. 再读 RAII
3. 最后回到 `http_conn.cpp` 顺登录注册链路

---

## 第一遍：连接池骨架

先把这些成员和接口过一遍：

- `connection_pool::GetInstance`
- `connection_pool::init`
- `connection_pool::GetConnection`
- `connection_pool::ReleaseConnection`
- `connection_pool::DestroyPool`
- `connection_pool::GetFreeConn`
- `connectionRAII`

第一遍结束后，你要知道：

- 连接池是单例
- 连接池里存的不是配置，而是真实 MySQL 连接对象
- 连接获取和释放是成对出现的

---

## 第二遍：按函数精读连接池

### 板块 1：连接池初始化

重点函数：

- `connection_pool::init`

你要看清楚：

- 初始化时一次性创建多少连接
- 这些连接保存在哪里
- 连接数、空闲连接数、已使用连接数如何维护
- 为什么这里需要锁和信号量

要达到的水平：

- 能解释为什么连接池适合高并发 Web 场景

建议断点位置：

- 创建 MySQL 连接的循环处

### 板块 2：获取与释放连接

重点函数：

- `connection_pool::GetConnection`
- `connection_pool::ReleaseConnection`

你要看清楚：

- 没有空闲连接时会怎样
- 连接取出后统计数据怎么变化
- 连接归还后统计数据怎么变化

要达到的水平：

- 能说出连接池通过什么机制限制同时访问数据库的连接数量

### 板块 3：RAII 封装

重点函数：

- `connectionRAII::connectionRAII`
- `connectionRAII::~connectionRAII`

你要看清楚：

- 构造时拿连接
- 析构时还连接

要达到的水平：

- 能解释它如何避免“忘记归还连接”

---

## 第三遍：回到 `http_conn.cpp` 跟登录注册主链路

今天你必须在 `http_conn.cpp` 里把下面这些点定位出来：

- 用户表数据如何初始化
- 登录注册 POST 数据如何进入解析
- 用户名和密码如何从请求体里拆出来
- 注册逻辑如何判断用户名是否已存在
- 登录逻辑如何校验用户名密码
- 成功和失败分别跳转到哪个资源

---

## 重点函数和板块

### 板块 1：用户数据初始化

重点函数：

- `http_conn::initmysql_result`

你要看清楚：

- 启动阶段怎么读取用户表
- 数据为什么会放进 `map<string, string>`

要达到的水平：

- 能解释“数据库 + 内存缓存”这层设计

### 板块 2：POST 请求体进入业务逻辑

重点函数：

- `http_conn::parse_content`
- `http_conn::do_request`

你要看清楚：

- 请求体完整后，数据保存在哪里
- `do_request()` 是怎么根据 URL 和表单内容决定后续逻辑的

要达到的水平：

- 能说出登录注册逻辑并不是在 `parse_content()` 里完成的，而是在后续业务决策阶段完成的

### 板块 3：注册逻辑

你要在 `do_request()` 里定位出注册分支，重点看：

- 如何从请求体解析用户名和密码
- 如何判断用户是否已存在
- SQL 插入语句在哪里构造
- 插入成功后返回哪个页面
- 插入失败后返回哪个页面

要达到的水平：

- 能完整讲清注册流程的每一步

### 板块 4：登录逻辑

你要在 `do_request()` 里定位出登录分支，重点看：

- 用户名密码如何和 `m_users` 对比
- 成功时返回哪个页面
- 失败时返回哪个页面

要达到的水平：

- 能完整讲清登录流程的每一步

建议断点位置：

- `initmysql_result()`
- `do_request()` 中登录/注册分支判断处

---

## 今天必须手写出来的调用链

### 1. 数据库初始化链

`WebServer::sql_pool -> connection_pool::init -> http_conn::initmysql_result`

### 2. 登录请求链

`POST 请求 -> read_once -> process_read -> parse_content -> do_request -> 用户校验 -> 选择页面 -> process_write`

### 3. 注册请求链

`POST 请求 -> parse_content -> do_request -> SQL 插入 -> 更新用户表/缓存 -> 选择页面 -> process_write`

---

## 今天适合重点思考的问题

- 为什么项目既要查数据库，又要维护一个内存 `map`
- 为什么登录注册逻辑没有单独拆成 service 层
- 如果用户数据很多，当前这种做法会不会有问题
- 如果数据库不可用，这个项目现在会怎样表现

---

## 今天结束时的验收标准

你必须做到：

- 能讲清连接池初始化、获取、释放的完整流程
- 能解释 RAII 在这里解决了什么问题
- 能完整描述登录和注册两条业务路径
- 能区分“业务决定返回哪个页面”和“HTTP 负责把页面发回去”

---

## 今日输出要求

今天的笔记必须包含：

### 1. 连接池结构图

成员和操作写清楚。

### 2. 登录链路

从请求体到页面返回。

### 3. 注册链路

从请求体到 SQL 再到页面返回。

### 4. 设计评价

至少写 2 个优点和 2 个潜在问题。

