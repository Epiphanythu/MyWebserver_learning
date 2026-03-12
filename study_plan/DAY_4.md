# Day 4：精读 HTTP 请求解析和响应生成

## 今天的任务定义

今天只盯 `http_conn`。

你的目标不是“把这个文件读完”，而是把一次 HTTP 请求从进入读缓冲区，到被解析，到决定返回什么资源，再到响应发出去，这整条链路读通。

---

## 今天必须精读的文件

- `http/http_conn.h`
- `http/http_conn.cpp`

建议顺序：

1. 先看 `http_conn.h` 里的枚举、成员、接口
2. 再看 `http_conn.cpp` 的整体结构
3. 第二遍按请求生命周期顺序精读函数

---

## 第一遍：建立类型和状态认知

先把这些内容过一遍：

- `METHOD`
- `CHECK_STATE`
- `HTTP_CODE`
- `LINE_STATUS`
- 读缓冲区成员
- 写缓冲区成员
- 请求相关成员
- 文件映射相关成员
- `m_state`
- `m_check_state`

第一遍结束后，你必须知道：

- 哪些枚举是“请求解析阶段”的状态
- 哪些枚举是“服务器处理结果”的状态
- 读缓冲区和写缓冲区分别干什么

---

## 第二遍：按请求生命周期精读

### 板块 1：连接初始化

重点函数：

- `http_conn::init(int sockfd, ...)`
- `http_conn::init()`

你要看清楚：

- 新连接接入时初始化了哪些字段
- 解析状态、缓冲区、连接属性如何重置

要达到的水平：

- 能解释为什么 `http_conn` 需要既有“带参数 init”又有“无参数 init”

建议断点位置：

- `http_conn::init(int sockfd, ...)`

### 板块 2：读取请求数据

重点函数：

- `http_conn::read_once`

你要逐段看：

1. 当前已读位置怎么算
2. LT 模式怎么读
3. ET 模式怎么循环读
4. 遇到 `EAGAIN` 怎么处理
5. 什么情况下返回 false

要达到的水平：

- 能解释 ET 为什么必须一次性把内核缓冲区尽量读空
- 能说出 `m_read_idx` 的作用

建议断点位置：

- `recv(...)`
- ET 循环处

### 板块 3：总处理入口

重点函数：

- `http_conn::process`

你要看清楚：

- 它是怎么调用 `process_read()`
- 什么时候会继续生成响应
- 什么时候需要重新监听读事件

要达到的水平：

- 能把 `process()` 看成 HTTP 层总调度函数，而不是普通包装函数

### 板块 4：请求解析主线

重点函数：

- `http_conn::process_read`
- `http_conn::parse_line`
- `http_conn::parse_request_line`
- `http_conn::parse_headers`
- `http_conn::parse_content`

这是今天最重要的一段。

你要分 3 层看：

#### 第一层：`process_read()`

重点看：

- 主状态机如何推进
- 什么时候处理请求行
- 什么时候处理请求头
- 什么时候处理请求体

#### 第二层：`parse_line()`

重点看：

- 怎么从读缓冲区里按 `\r\n` 切行
- 为什么这一步属于从状态机

#### 第三层：3 个 parse 函数

重点看：

- 请求方法、URL、HTTP 版本怎么解析
- `Host`、`Connection`、`Content-Length` 怎么处理
- 请求体完整性怎么判断

要达到的水平：

- 能真正讲清“主状态机 + 从状态机”在代码里的配合关系
- 能说出 GET 和 POST 的分流点大致落在哪

建议断点位置：

- `process_read()` 主循环
- `parse_request_line(...)`
- `parse_headers(...)`
- `parse_content(...)`

### 板块 5：业务路由和资源定位

重点函数：

- `http_conn::do_request`

你要重点看：

- URL 如何映射到磁盘文件
- 什么时候进入登录注册逻辑
- 什么时候只是普通静态文件返回
- 文件不存在、无权限、请求目录时分别返回什么状态

要达到的水平：

- 能区分“HTTP 解析完成”和“业务/资源决策开始”这两个阶段

建议重点盯的变量：

- `m_url`
- `m_real_file`
- `cgi`
- `m_string`

### 板块 6：响应构造

重点函数：

- `http_conn::process_write`
- `http_conn::add_status_line`
- `http_conn::add_headers`
- `http_conn::add_content_type`
- `http_conn::add_content_length`
- `http_conn::add_linger`
- `http_conn::add_blank_line`
- `http_conn::add_content`

你要看清楚：

- 响应报文头部如何一点点拼出来
- 不同 `HTTP_CODE` 如何映射到不同响应

要达到的水平：

- 能说出 HTTP 响应至少由哪几部分组成

### 板块 7：真正发送响应

重点函数：

- `http_conn::write`
- `http_conn::unmap`

你要看清楚：

- `iovec` 怎么组织头部和文件内容
- `writev` 怎么发送
- 文件发送完成后为什么要 `unmap`
- 长连接和短连接后续流程有什么区别

要达到的水平：

- 能解释为什么这个项目适合 `mmap + writev`

建议断点位置：

- `writev(...)`
- `unmap()`

---

## 今天必须手写出来的调用链

### 1. 请求处理链

`read_once -> process -> process_read -> parse_xxx -> do_request -> process_write -> write`

### 2. GET 资源返回链

`parse_request_line -> do_request -> 文件映射 -> process_write -> writev`

### 3. POST 登录/注册链

`parse_content -> do_request -> 业务分支 -> 选择返回页面 -> process_write`

---

## 今天适合重点思考的问题

- 为什么请求解析要拆成主状态机和从状态机
- 为什么 `do_request()` 不只是“打开文件”
- 为什么响应头和响应体分开组织
- 为什么发文件不直接反复 `send`

---

## 今天结束时的验收标准

你必须做到：

- 能描述一个请求从进入缓冲区到发出响应的完整函数路径
- 能解释读缓冲区、写缓冲区、文件映射三者的角色
- 能说清 GET 和 POST 在哪里开始出现处理差异
- 能讲明白 `process_read()`、`do_request()`、`process_write()` 三个阶段分别负责什么

---

## 今日输出要求

今天的笔记必须包含：

### 1. `http_conn` 关键成员说明

至少写出状态、缓冲区、文件映射相关成员。

### 2. HTTP 解析流程图

把状态推进画出来。

### 3. 响应生成流程图

写清头部和正文是怎么拼的。

### 4. GET 和 POST 对比

重点写处理链路的区别，不要只写概念。

