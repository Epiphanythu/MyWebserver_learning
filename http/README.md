# HTTP 模块详解

## 一、模块概述

HTTP 模块是 Web 服务器的核心，负责：
- **接收** 浏览器发来的 HTTP 请求
- **解析** 请求内容（请求行、头部、正文）
- **处理** 请求（查找文件、验证登录等）
- **生成** HTTP 响应返回给浏览器

---

## 二、HTTP 请求报文结构

浏览器发送的请求格式如下：

```
POST /2log.html HTTP/1.1          ← 请求行
Host: 192.168.1.1:9006            ← 请求头部
Connection: keep-alive
Content-Length: 25
                                  ← 空行（分隔头部和正文）
user=admin&passwd=123456          ← 请求正文（POST才有）
```

**三部分说明：**

| 部分 | 内容 | 说明 |
|------|------|------|
| 请求行 | `POST /2log.html HTTP/1.1` | 方法、URL、版本 |
| 请求头部 | `Host: xxx` 等键值对 | 附加信息 |
| 请求正文 | `user=admin&passwd=123456` | POST 请求的数据 |

---

## 三、核心类设计

### 3.1 四个关键枚举

```cpp
// 1. HTTP 请求方法
enum METHOD {
    GET = 0,     // 获取资源
    POST,        // 提交数据（登录/注册）
    // ... 其他方法
};

// 2. 主状态机的状态（解析到哪一步了）
enum CHECK_STATE {
    CHECK_STATE_REQUESTLINE = 0,  // 正在解析请求行
    CHECK_STATE_HEADER,           // 正在解析头部
    CHECK_STATE_CONTENT           // 正在解析正文
};

// 3. HTTP 处理结果
enum HTTP_CODE {
    NO_REQUEST,          // 请求不完整，需要继续读取
    GET_REQUEST,         // 获得了完整的请求
    BAD_REQUEST,         // 请求格式错误
    NO_RESOURCE,         // 资源不存在
    FORBIDDEN_REQUEST,   // 没有权限
    FILE_REQUEST,        // 文件请求，可以发送
    INTERNAL_ERROR,      // 服务器内部错误
};

// 4. 从状态机状态（分析一行内容）
enum LINE_STATUS {
    LINE_OK = 0,   // 读到一个完整的行
    LINE_BAD,      // 行格式错误
    LINE_OPEN      // 行未读完
};
```

### 3.2 核心成员变量

```cpp
class http_conn {
private:
    int m_sockfd;                    // 客户端 socket 描述符
    char m_read_buf[READ_BUFFER_SIZE];  // 读缓冲区（存储请求）
    long m_read_idx;                 // 缓冲区已读位置
    long m_checked_idx;              // 当前分析位置
    int m_start_line;                // 当前行的起始位置

    char m_write_buf[WRITE_BUFFER_SIZE]; // 写缓冲区（存储响应）

    CHECK_STATE m_check_state;       // 主状态机当前状态
    METHOD m_method;                 // 请求方法（GET/POST）
    char *m_url;                     // 请求的 URL
    char *m_file_address;            // 文件映射到内存的地址

    bool m_linger;                   // 是否保持连接
    long m_content_length;           // 正文长度
};
```

---

## 四、状态机解析（核心！）

### 4.1 双状态机模型

```
┌─────────────────────────────────────────────────────────────┐
│                      主状态机                                │
│  (决定当前解析的是 请求行 / 头部 / 正文)                       │
└─────────────────────────────────────────────────────────────┘
                              │
                              │ 每次处理一行
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      从状态机                                │
│  (负责从缓冲区中读出一行完整内容)                              │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 从状态机 - parse_line()

**作用**：从缓冲区中读出一行（以 `\r\n` 结尾）

```cpp
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];

        if (temp == '\r')  // 遇到回车
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;  // 数据不完整，还没读到 \n
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                // 把 \r\n 替换成 \0\0（字符串结束符）
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;  // 读到完整一行
            }
            return LINE_BAD;
        }
        else if (temp == '\n')  // 遇到换行（处理只有 \n 的情况）
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;  // 还没读到完整一行
}
```

**图解：**

```
缓冲区内容：
┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
│ G │ E │ T │   │ / │   │ H │ T │ T │ P │ \r│ \n│ ...
└───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
                                      ↑
                                m_checked_idx

找到 \r\n 后，替换为 \0\0：
┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
│ G │ E │ T │   │ / │   │ H │ T │ T │ P │ \0│ \0│ ...
└───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
                                      ↑
                            现在这一行是一个完整字符串
```

### 4.3 主状态机 - process_read()

**作用**：根据当前状态，调用不同的解析函数

```cpp
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK)
           || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();        // 获取当前行
        m_start_line = m_checked_idx;

        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:  // 解析请求行
            ret = parse_request_line(text);
            break;

        case CHECK_STATE_HEADER:       // 解析头部
            ret = parse_headers(text);
            break;

        case CHECK_STATE_CONTENT:      // 解析正文
            ret = parse_content(text);
            break;
        }
    }
    return NO_REQUEST;
}
```

**状态转换图：**

```
                    开始
                      │
                      ▼
        ┌─────────────────────────────┐
        │  CHECK_STATE_REQUESTLINE    │
        │  解析请求行                   │
        │  "GET /index.html HTTP/1.1" │
        └──────────────┬──────────────┘
                       │ 解析成功
                       ▼
        ┌─────────────────────────────┐
        │  CHECK_STATE_HEADER         │
        │  解析请求头部                 │
        │  "Host: xxx" "Connection:.."│
        └──────────────┬──────────────┘
                       │ 遇到空行
                       ▼
              ┌────────┴────────┐
              │ 有正文？          │
              └────────┬────────┘
               是 │         │ 否
                  ▼         ▼
    ┌──────────────────┐   直接处理
    │ CHECK_STATE_CONTENT│
    │ 解析正文            │
    │ "user=admin&..."   │
    └────────────────────┘
```

---

## 五、解析各部分详解

### 5.1 解析请求行 - parse_request_line()

```cpp
// 输入: "GET /index.html HTTP/1.1"
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // 1. 找到第一个空格或制表符，分割出方法
    m_url = strpbrk(text, " \t");
    *m_url++ = '\0';  // 在空格处截断

    char *method = text;  // 现在text就是 "GET"

    // 2. 判断请求方法
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;  // 标记需要 CGI 处理
    }

    // 3. 提取 URL
    m_url += strspn(m_url, " \t");  // 跳过空格

    // 4. 提取版本
    m_version = strpbrk(m_url, " \t");
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    // 5. 处理 URL，默认访问 judge.html
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");

    // 6. 状态转换：接下来解析头部
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}
```

**图解：**

```
原始: "GET /index.html HTTP/1.1"
       │    │            │
       │    │            └── m_version
       │    └── m_url
       └── method

第一步: 找空格，截断
       "GET\0/index.html HTTP/1.1"
            ↑
          m_url 指向这里

第二步: 提取 URL
       "GET\0/index.html\0HTTP/1.1"
                      ↑
                   m_version
```

### 5.2 解析头部 - parse_headers()

```cpp
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 1. 空行表示头部结束
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            // 有正文，继续解析
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 没有正文，请求完整了
        return GET_REQUEST;
    }
    // 2. 解析 Connection 字段
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        if (strcasecmp(text, "keep-alive") == 0)
            m_linger = true;  // 保持连接
    }
    // 3. 解析 Content-length 字段
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        m_content_length = atol(text);
    }
    // 4. 解析 Host 字段
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        m_host = text;
    }

    return NO_REQUEST;
}
```

### 5.3 解析正文 - parse_content()

```cpp
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    // 检查正文是否读取完整
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        m_string = text;  // 保存正文（用户名密码等）
        return GET_REQUEST;
    }
    return NO_REQUEST;
}
```

---

## 六、处理请求 - do_request()

解析完成后，调用 `do_request()` 处理请求：

```cpp
http_conn::HTTP_CODE http_conn::do_request()
{
    // 1. 拼接文件路径
    strcpy(m_real_file, doc_root);  // doc_root = "./root"
    int len = strlen(doc_root);

    // 2. 根据URL处理不同请求
    const char *p = strrchr(m_url, '/');

    // 处理登录/注册（POST请求）
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        // 提取用户名和密码
        // "user=admin&passwd=123456"
        char name[100], password[100];
        // ... 解析代码

        if (*(p + 1) == '3')  // 注册
        {
            // 插入数据库
            // INSERT INTO user...
        }
        else if (*(p + 1) == '2')  // 登录
        {
            // 验证用户名密码
            if (users[name] == password)
                // 返回欢迎页面
            else
                // 返回错误页面
        }
    }

    // 3. 打开文件，映射到内存
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size,
                                   PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    return FILE_REQUEST;
}
```

**URL 路由表：**

| URL | 功能 |
|-----|------|
| `/0` | 注册页面 |
| `/1` | 登录页面 |
| `/2log.html` | 登录验证（POST） |
| `/3register.html` | 注册验证（POST） |
| `/5` | 图片页面 |
| `/6` | 视频页面 |

---

## 七、生成响应 - process_write()

### 7.1 HTTP 响应格式

```
HTTP/1.1 200 OK                  ← 状态行
Content-Length: 1234             ← 响应头部
Connection: keep-alive
                                 ← 空行
<html>...</html>                 ← 响应正文
```

### 7.2 构建响应

```cpp
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case FILE_REQUEST:  // 文件请求成功
    {
        // 1. 添加状态行
        add_status_line(200, ok_200_title);

        // 2. 添加响应头
        add_headers(m_file_stat.st_size);

        // 3. 设置 iovec（分散写）
        m_iv[0].iov_base = m_write_buf;      // 响应头
        m_iv[0].iov_len = m_write_idx;
        m_iv[1].iov_base = m_file_address;   // 文件内容
        m_iv[1].iov_len = m_file_stat.st_size;
        m_iv_count = 2;

        return true;
    }
    case BAD_REQUEST:  // 404 错误
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        add_content(error_404_form);
        break;
    }
}
```

### 7.3 添加响应内容的辅助函数

```cpp
// 添加状态行: "HTTP/1.1 200 OK\r\n"
bool add_status_line(int status, const char *title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 添加头部字段: "Content-Length: 1234\r\n"
bool add_content_length(int content_len) {
    return add_response("Content-Length:%d\r\n", content_len);
}

// 添加连接状态: "Connection: keep-alive\r\n"
bool add_linger() {
    return add_response("Connection:%s\r\n",
                        m_linger ? "keep-alive" : "close");
}

// 添加空行
bool add_blank_line() {
    return add_response("%s", "\r\n");
}
```

---

## 八、发送响应 - write()

使用 `writev` **分散写**一次性发送响应头和文件内容：

```cpp
bool http_conn::write()
{
    while (1)
    {
        // writev 可以一次写多个缓冲区
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)  // 缓冲区满了，等下次再写
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_to_send <= 0)  // 发送完成
        {
            unmap();  // 解除内存映射
            if (m_linger)
                init();  // 保持连接，重置状态
            else
                return false;  // 关闭连接
        }
    }
}
```

**分散写图解：**

```
m_iv[0]: ┌─────────────────────────┐
         │ HTTP/1.1 200 OK\r\n     │ ← 响应头
         │ Content-Length: 1234\r\n│
         │ \r\n                    │
         └─────────────────────────┘

m_iv[1]: ┌─────────────────────────┐
         │ <html>...</html>        │ ← 文件内容（内存映射）
         └─────────────────────────┘

writev 一次性把两个缓冲区都发送出去
```

---

## 九、完整处理流程

```
┌────────────────────────────────────────────────────────────────┐
│                       HTTP 处理流程                             │
└────────────────────────────────────────────────────────────────┘

1. 浏览器发送请求
        │
        ▼
2. read_once() ── 读取请求数据到 m_read_buf
        │
        ▼
3. process_read() ── 解析请求
   ┌─────────────────────────────────────┐
   │  主状态机循环:                        │
   │  ├── parse_line() 从状态机取一行      │
   │  ├── parse_request_line() 解析请求行  │
   │  ├── parse_headers() 解析头部         │
   │  └── parse_content() 解析正文         │
   └─────────────────────────────────────┘
        │
        ▼
4. do_request() ── 处理请求
   ├── 静态请求: 打开文件，mmap 映射到内存
   └── 动态请求: 登录/注册验证
        │
        ▼
5. process_write() ── 生成响应
   ├── 添加状态行
   ├── 添加响应头
   └── 设置 iovec
        │
        ▼
6. write() ── 发送响应
   └── writev 分散写发送数据
        │
        ▼
7. 浏览器收到响应，显示页面
```

---

## 十、关键技术点

### 10.1 mmap 内存映射

```cpp
// 把文件映射到内存，直接像操作内存一样操作文件
m_file_address = (char *)mmap(0, m_file_stat.st_size,
                               PROT_READ, MAP_PRIVATE, fd, 0);

// 用完后解除映射
munmap(m_file_address, m_file_stat.st_size);
```

**优点**：避免文件读写的数据拷贝，提高效率。

### 10.2 writev 分散写

```cpp
struct iovec {
    void  *iov_base;  // 缓冲区起始地址
    size_t iov_len;   // 缓冲区长度
};

// 一次性写入多个不连续的缓冲区
writev(fd, iov, iovcnt);
```

**优点**：减少系统调用次数，提高效率。

### 10.3 非阻塞 I/O

```cpp
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}
```

---

## 十一、函数总结

| 函数 | 作用 |
|------|------|
| `read_once()` | 读取请求数据 |
| `process_read()` | 主状态机，协调整个解析过程 |
| `parse_line()` | 从状态机，读出一行 |
| `parse_request_line()` | 解析请求行 |
| `parse_headers()` | 解析请求头部 |
| `parse_content()` | 解析请求正文 |
| `do_request()` | 处理请求（查文件、验证登录） |
| `process_write()` | 生成响应报文 |
| `write()` | 发送响应数据 |

**核心思想**：
1. **状态机模式**：将复杂的解析过程分解为多个状态
2. **零拷贝**：mmap + writev 减少数据拷贝
3. **非阻塞 I/O**：配合 epoll 实现高并发

---

---

# ET/LT 模式详解

## 1. LT 模式（Level-Triggered，水平触发，默认模式）

### 行为特点：
- 只要 socket 的接收缓冲区**还有数据未读完**，每次调用 `epoll_wait()` 都会返回该 fd。
- 即使你只读了一部分数据，下次 `epoll_wait()` 仍会通知你"可读"。

### 类比：
> 像门铃：只要有人在门口（缓冲区有数据），门铃就会一直响，直到你把人请进来（读完数据）。

### 优点：
- 编程简单，容错性强
- 适合初学者

### 缺点：
- 可能重复通知，效率略低（但通常可接受）

### TinyWebServer 中的使用：
```cpp
// LT 模式（默认）
event.events = EPOLLIN | EPOLLRDHUP;
```

---

## 2. ET 模式（Edge-Triggered，边缘触发）

### 行为特点：
- **仅在状态变化时通知一次**。
  例如：socket 接收缓冲区**从空 → 非空**时，`epoll_wait()` 返回一次。
- 如果你没有一次性读完所有数据，**即使缓冲区还有数据，也不会再次通知！**

### 类比：
> 像拍肩膀：只拍你一下，提醒"有新数据来了"。如果你没理，就不会再拍。

### 优点：
- 减少 `epoll_wait()` 的调用次数，**性能更高**
- 适合高并发场景（如 Nginx 默认使用 ET）

### 缺点：
- **必须一次性读完所有数据**，否则会丢数据！
- 必须配合 **非阻塞 socket（O_NONBLOCK）** 使用

### TinyWebServer 中的使用：
```cpp
// ET 模式
event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
setnonblocking(fd); // 必须设为非阻塞！
```

并在 `read_once()` 中循环读取：
```cpp
// ET 模式下必须循环 recv 直到 EAGAIN
while (true) {
    bytes_read = recv(fd, buf, size, 0);
    if (bytes_read == -1) {
        if (errno == EAGAIN) break; // 数据读完了
        return false;
    }
}
```

---

## 3. 对比总结

| 特性 | LT（水平触发） | ET（边缘触发） |
|------|----------------|----------------|
| 通知频率 | 只要有数据就通知 | 仅状态变化时通知一次 |
| 是否需循环读 | 否（可分多次读） | **是（必须一次读完）** |
| 是否需非阻塞 | 否（可用阻塞） | **是（必须非阻塞）** |
| 编程难度 | 简单 | 较难 |
| 性能 | 略低 | 更高 |
| 典型应用 | 初学者项目、简单服务器 | Nginx、Redis、高性能服务 |

---

## 4. 在 TinyWebServer 中如何选择？

启动时通过 `-m` 参数控制：
```bash
./server -m 0   # LT + LT（默认）
./server -m 3   # ET + ET（高性能）
```

- `listenfd`（监听 socket）和 `connfd`（连接 socket）可分别设置模式
- **ET 模式下，`read_once()` 必须用 while 循环读完**，否则请求会卡住！

---

## 5. 建议

- **初学者先用 LT 模式**：逻辑简单，不易出错
- **理解后再尝试 ET 模式**：体会高性能服务器的设计精髓
- **永远记住**：ET + 非阻塞 + 循环读 = 正确姿势

> 面试常问：
> "为什么 ET 模式要搭配非阻塞 socket？"
> 答：因为 `recv` 在无数据时会阻塞，而 ET 不会再次通知，导致线程卡死。
