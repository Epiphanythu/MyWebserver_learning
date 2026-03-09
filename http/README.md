# HTTP 模块详解

## 一、模块概述

HTTP 模块是 Web 服务器的核心，负责：
- **接收** 浏览器发来的 HTTP 请求
- **解析** 请求内容（请求行、头部、正文）
- **处理** 请求（查找文件、验证登录等）
- **生成** HTTP 响应返回给浏览器

---

## 二、源码文件结构

```
http/
├── http_conn.h    # 头文件：类定义、枚举、成员变量
├── http_conn.cpp  # 实现文件：所有函数的具体实现
└── README.md      # 本文档
```

---

## 三、HTTP 请求报文结构

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

## 四、头文件解析（http_conn.h）

### 4.1 四个关键枚举

```cpp
// ① HTTP 请求方法
enum METHOD {
    GET = 0,    // 获取资源（最常用）
    POST,       // 提交数据（登录/注册用）
    HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH
};

// ② 主状态机状态（解析到哪一步了）
enum CHECK_STATE {
    CHECK_STATE_REQUESTLINE = 0,  // 正在解析请求行
    CHECK_STATE_HEADER,           // 正在解析头部
    CHECK_STATE_CONTENT           // 正在解析正文
};

// ③ HTTP 处理结果
enum HTTP_CODE {
    NO_REQUEST,          // 请求不完整，继续读
    GET_REQUEST,         // 获得完整请求
    BAD_REQUEST,         // 请求格式错误
    NO_RESOURCE,         // 资源不存在（404）
    FORBIDDEN_REQUEST,   // 无权限（403）
    FILE_REQUEST,        // 文件请求成功
    INTERNAL_ERROR,      // 服务器内部错误（500）
    CLOSED_CONNECTION
};

// ④ 从状态机状态（一行是否读完）
enum LINE_STATUS {
    LINE_OK = 0,   // 读到完整一行
    LINE_BAD,      // 行格式错误
    LINE_OPEN      // 行还没读完
};
```

### 4.2 核心成员变量详解

```cpp
class http_conn {
private:
    // ===== Socket 相关 =====
    int m_sockfd;                        // 客户端 socket 文件描述符
    sockaddr_in m_address;               // 客户端地址信息

    // ===== 读缓冲区（存储请求）=====
    char m_read_buf[READ_BUFFER_SIZE];   // 读缓冲区（2048字节）
    long m_read_idx;                     // 当前已读取的位置（下次读从这里开始）
    long m_checked_idx;                  // 当前已分析的位置（解析到这里）
    int m_start_line;                    // 当前行的起始位置

    // ===== 写缓冲区（存储响应）=====
    char m_write_buf[WRITE_BUFFER_SIZE]; // 写缓冲区（1024字节）
    int m_write_idx;                     // 当前写入位置

    // ===== 解析状态 =====
    CHECK_STATE m_check_state;           // 主状态机当前状态
    METHOD m_method;                     // 请求方法（GET/POST）

    // ===== 请求信息 =====
    char *m_url;                         // 请求的 URL（如 "/index.html"）
    char *m_version;                     // HTTP 版本（如 "HTTP/1.1"）
    char *m_host;                        // 主机名
    long m_content_length;               // 正文长度（POST 请求才有）
    bool m_linger;                       // 是否保持连接（keep-alive）

    // ===== 文件相关 =====
    char m_real_file[FILENAME_LEN];      // 实际文件路径（如 "./root/index.html"）
    char *m_file_address;                // mmap 映射的内存地址
    struct stat m_file_stat;             // 文件状态信息（大小、权限等）

    // ===== 分散写（writev）=====
    struct iovec m_iv[2];                // 两个缓冲区：[0]响应头 + [1]文件内容
    int m_iv_count;                      // 缓冲区数量（1 或 2）
    int bytes_to_send;                   // 总共要发送的字节数
    int bytes_have_send;                 // 已经发送的字节数

    // ===== 其他 =====
    int cgi;                             // 是否是 POST 请求（CGI 处理）
    char *m_string;                      // POST 请求的正文内容
    char *doc_root;                      // 网站根目录（"./root"）
    int m_TRIGMode;                      // 触发模式（0=LT, 1=ET）
};
```

### 4.3 核心函数一览

```cpp
public:
    // ===== 对外接口 =====
    void init(int sockfd, ...);    // 初始化连接（外部调用）
    void close_conn(bool real_close = true);  // 关闭连接
    void process();                // 主入口：解析 + 处理 + 生成响应
    bool read_once();              // 读取请求数据
    bool write();                  // 发送响应数据

private:
    // ===== 内部函数 =====
    void init();                   // 初始化内部状态

    // 解析相关
    HTTP_CODE process_read();              // 主状态机
    LINE_STATUS parse_line();              // 从状态机（读一行）
    HTTP_CODE parse_request_line(char *text);  // 解析请求行
    HTTP_CODE parse_headers(char *text);       // 解析头部
    HTTP_CODE parse_content(char *text);       // 解析正文
    HTTP_CODE do_request();              // 处理请求
    char *get_line() { return m_read_buf + m_start_line; }

    // 响应相关
    bool process_write(HTTP_CODE ret);    // 生成响应
    void unmap();                         // 解除 mmap 映射
    bool add_response(const char *format, ...);  // 添加响应内容
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
    bool add_content(const char *content);
```

---

## 五、数据流向总览

```
┌─────────────────────────────────────────────────────────────────┐
│                    HTTP 数据流向                                 │
└─────────────────────────────────────────────────────────────────┘

浏览器发送请求
      │
      ▼
┌─────────────┐
│ read_once() │  读取数据到 m_read_buf
│  LT: 读一次  │
│  ET: 循环读  │
└──────┬──────┘
       │
       ▼
┌──────────────┐
│   process()  │  入口函数
└──────┬───────┘
       │
       ├─► process_read() 主状态机
       │       │
       │       ├─► parse_line()         从状态机，读一行
       │       ├─► parse_request_line() 解析 "GET / HTTP/1.1"
       │       ├─► parse_headers()      解析 "Host: xxx"
       │       └─► parse_content()      解析 POST 正文
       │
       ├─► do_request() 处理请求
       │       ├─► URL 路由
       │       ├─► 登录/注册验证
       │       └─► mmap 映射文件
       │
       └─► process_write() 生成响应
               ├─► add_status_line()
               ├─► add_headers()
               └─► 设置 iovec
       │
       ▼
┌───────────────┐
│    write()    │  发送响应（writev 分散写）
└──────┬────────┘
       │
       ▼
浏览器收到响应
```

---

## 六、源码逐函数解析

### 6.1 入口函数 process()

**位置**：http_conn.cpp 第 688 行

```cpp
void http_conn::process()
{
    // 1. 解析请求
    HTTP_CODE read_ret = process_read();

    // 2. 如果请求不完整，继续等待数据
    if (read_ret == NO_REQUEST)
    {
        // 注册 EPOLLIN 事件，等待更多数据
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }

    // 3. 生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();  // 生成失败，关闭连接
    }

    // 4. 注册 EPOLLOUT 事件，准备发送响应
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
```

**流程说明：**

```
process_read() 返回值：
├── NO_REQUEST     → 请求不完整，继续等待
├── GET_REQUEST    → 请求完整，处理
├── BAD_REQUEST    → 请求错误，返回 404
├── NO_RESOURCE    → 资源不存在，返回 404
├── FORBIDDEN_REQUEST → 无权限，返回 403
├── FILE_REQUEST   → 文件请求成功，返回 200
└── INTERNAL_ERROR → 服务器错误，返回 500
```

---

### 6.2 读取数据 read_once()

**位置**：http_conn.cpp 第 198 行

```cpp
//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    // 缓冲区已满检查
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }

    int bytes_read = 0;

    // ===== LT 模式（水平触发）=====
    if (0 == m_TRIGMode)
    {
        // 读一次就行，没读完下次 epoll 还会通知
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                          READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;  // 读失败或对方关闭连接
        }

        return true;
    }
    // ===== ET 模式（边缘触发）=====
    else
    {
        // 必须循环读完！因为 ET 只通知一次
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                              READ_BUFFER_SIZE - m_read_idx, 0);

            if (bytes_read == -1)
            {
                // EAGAIN/EWOULDBLOCK = 没数据了，读完了（正常情况）
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;  // 真正的错误
            }
            else if (bytes_read == 0)
            {
                return false;  // 对方关闭连接
            }

            m_read_idx += bytes_read;  // 更新已读位置
        }
        return true;
    }
}
```

**图解 LT vs ET：**

```
LT 模式：
┌────────────────────────────────────────┐
│ 缓冲区: [数据1][数据2][数据3]            │
│                                         │
│ 第1次 recv → 读数据1                    │
│ epoll 还会通知，继续读                   │
│ 第2次 recv → 读数据2                    │
│ epoll 还会通知，继续读                   │
│ 第3次 recv → 读数据3                    │
└────────────────────────────────────────┘

ET 模式：
┌────────────────────────────────────────┐
│ 缓冲区: [数据1][数据2][数据3]            │
│                                         │
│ epoll 只通知一次！                       │
│ 必须用 while 循环一次性读完：            │
│   while(1) {                            │
│     recv() → 读数据1                    │
│     recv() → 读数据2                    │
│     recv() → 读数据3                    │
│     recv() → EAGAIN（没数据了，退出）    │
│   }                                     │
└────────────────────────────────────────┘
```

**关键变量说明：**

| 变量 | 含义 |
|------|------|
| `m_read_buf` | 读缓冲区，存储原始请求数据 |
| `m_read_idx` | 已读取的位置，下次读取从这里开始 |
| `READ_BUFFER_SIZE` | 缓冲区大小，2048 字节 |

---

### 6.3 从状态机 parse_line()

**位置**：http_conn.cpp 第 164 行

**作用**：从缓冲区读出一行（以 `\r\n` 结尾），并将其替换为 `\0\0`

```cpp
//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    // 遍历缓冲区，从 m_checked_idx 到 m_read_idx
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];

        // ===== 情况1：遇到 \r =====
        if (temp == '\r')
        {
            // 如果 \r 是最后一个字符，说明数据不完整
            if ((m_checked_idx + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            // 如果下一个是 \n，说明是一行
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                // 把 \r\n 替换成 \0\0，变成字符串结束符
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;  // 读到完整一行！
            }
            return LINE_BAD;  // \r 后面不是 \n，格式错误
        }

        // ===== 情况2：遇到 \n（处理只有 \n 的情况）=====
        else if (temp == '\n')
        {
            // 检查前一个字符是否是 \r
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;  // 单独的 \n，格式错误
        }
    }
    return LINE_OPEN;  // 还没遇到换行符，数据不完整
}
```

**图解：**

```
读之前：
m_read_buf:
┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
│ G │ E │ T │   │ / │   │ H │ T │ T │ P │ \r│ \n│ H │ ...
└───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
      ↑                                       ↑
  m_start_line                          m_checked_idx

读之后：
m_read_buf:
┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
│ G │ E │ T │   │ / │   │ H │ T │ T │ P │ \0│ \0│ H │ ...
└───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
                                          ↑
                                  现在这里是字符串结束符

此时 get_line() 返回 "GET / HTTP"（一个完整的 C 字符串）
```

**为什么要替换成 \0？**
> 这样每一行就变成了一个独立的 C 字符串，可以直接用字符串函数（如 strcasecmp、strpbrk）处理。

**返回值说明：**

| 返回值 | 含义 | 下一步 |
|--------|------|--------|
| `LINE_OK` | 读到完整一行 | 交给主状态机处理 |
| `LINE_OPEN` | 行还没读完 | 继续读取数据 |
| `LINE_BAD` | 行格式错误 | 返回 BAD_REQUEST |

---

### 6.4 主状态机 process_read()

**位置**：http_conn.cpp 第 342 行

**作用**：协调整个解析过程，根据当前状态调用不同的解析函数

```cpp
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    // 循环条件：
    // 1. 如果在解析正文状态，且行状态是 OK
    // 2. 或者能从缓冲区读出一行
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK)
           || ((line_status = parse_line()) == LINE_OK))
    {
        // 获取当前行的起始位置
        text = get_line();  // = m_read_buf + m_start_line

        // 记录下一行的起始位置
        m_start_line = m_checked_idx;

        LOG_INFO("%s", text);  // 打印日志（可选）

        // 根据当前状态，调用不同的解析函数
        switch (m_check_state)
        {
        // ===== 状态1：解析请求行 =====
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            // 解析成功后，状态会变成 CHECK_STATE_HEADER
            break;
        }

        // ===== 状态2：解析头部 =====
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)  // 头部解析完了，没有正文
            {
                return do_request();  // 直接处理请求
            }
            break;
        }

        // ===== 状态3：解析正文 =====
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();  // 正文解析完了
            line_status = LINE_OPEN;   // 继续读取
            break;
        }

        default:
            return INTERNAL_ERROR;
        }
    }

    return NO_REQUEST;  // 请求不完整，需要继续读取
}
```

**状态转换图：**

```
开始（初始状态：CHECK_STATE_REQUESTLINE）
  │
  ▼
┌─────────────────────────────────────┐
│ CHECK_STATE_REQUESTLINE             │
│ 调用 parse_request_line()           │
│ 输入: "GET /index.html HTTP/1.1"    │
│ 处理: 提取 method, url, version     │
│ 完成后: m_check_state = HEADER      │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│ CHECK_STATE_HEADER                  │
│ 调用 parse_headers()                │
│ 输入: "Host: localhost:9006"        │
│ 输入: "Connection: keep-alive"      │
│ 输入: ""（空行，表示头部结束）        │
│ 处理: 提取各种头部字段               │
└──────────────┬──────────────────────┘
               │ 遇到空行
               ▼
        ┌──────┴──────┐
        │ m_content_  │
        │ length > 0 ?│
        └──────┬──────┘
          是│       │否
            ▼       ▼
┌────────────────┐  直接
│ CHECK_STATE_   │  do_request()
│ CONTENT        │
│ 调用 parse_    │
│ content()      │
│ 输入: "user=   │
│ admin&passwd=  │
│ 123"           │
└────────┬───────┘
         │ 解析完成
         ▼
   do_request()
```

---

### 6.5 解析请求行 parse_request_line()

**位置**：http_conn.cpp 第 242 行

**输入示例**：`"GET /index.html HTTP/1.1"`

```cpp
//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // ===== 1. 提取方法（GET/POST）=====
    // strpbrk: 找到第一个空格或制表符的位置
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;  // 格式错误
    }

    // 在空格处截断，text 就变成了方法名
    *m_url++ = '\0';  // 先把空格改成 \0，然后 m_url 指针后移
    char *method = text;  // 现在 text = "GET"

    // 判断是 GET 还是 POST
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;  // 标记需要 CGI 处理（登录/注册）
    }
    else
        return BAD_REQUEST;  // 不支持其他方法

    // ===== 2. 提取 URL =====
    // strspn: 跳过空格和制表符
    m_url += strspn(m_url, " \t");

    // ===== 3. 提取版本号 =====
    // 找到下一个空格
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;

    *m_version++ = '\0';  // 截断，现在 m_url = "/index.html"
    m_version += strspn(m_version, " \t");  // 跳过空格

    // 检查版本号
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    // ===== 4. 处理完整 URL =====
    // 如果是 "http://localhost/index.html" 格式，提取 "/index.html"
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');  // 找到第一个 /
    }
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    // ===== 5. 检查 URL 合法性 =====
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;

    // 如果只访问 "/"，显示首页 judge.html
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");

    // ===== 6. 状态转换 =====
    m_check_state = CHECK_STATE_HEADER;  // 接下来解析头部
    return NO_REQUEST;  // 请求还没处理完
}
```

**图解解析过程：**

```
原始字符串: "GET /index.html HTTP/1.1"
              │    │             │
              │    │             └── m_version（最终）
              │    └── m_url（最终）
              └── method（最终）

步骤1: strpbrk 找到第一个空格
       "GET /index.html HTTP/1.1"
           ↑
       m_url 指向这里

步骤2: 截断
       "GET\0/index.html HTTP/1.1"
            ↑
       text = "GET"（方法）
       m_url 指向 "/index.html HTTP/1.1"

步骤3: strspn 跳过空格
       m_url 指向 "/index.html HTTP/1.1"

步骤4: strpbrk 找下一个空格
       "/index.html HTTP/1.1"
                   ↑
              m_version 指向这里

步骤5: 截断
       最终结果:
       method = "GET"
       m_url = "/index.html"
       m_version = "HTTP/1.1"
```

---

### 6.6 解析头部 parse_headers()

**位置**：http_conn.cpp 第 290 行

**输入示例**：`"Host: localhost:9006"` 或 `"Connection: keep-alive"`

```cpp
//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // ===== 1. 空行表示头部结束 =====
    if (text[0] == '\0')
    {
        // 如果有 Content-Length，说明有正文
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;  // 切换到解析正文状态
            return NO_REQUEST;  // 继续读取
        }
        return GET_REQUEST;  // 没有正文，请求完整了
    }

    // ===== 2. 解析 Connection 字段 =====
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;  // 跳过 "Connection:"
        text += strspn(text, " \t");  // 跳过空格
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;  // 标记保持连接
        }
    }

    // ===== 3. 解析 Content-Length 字段 =====
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);  // 记录正文长度
    }

    // ===== 4. 解析 Host 字段 =====
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }

    // ===== 5. 其他头部忽略 =====
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }

    return NO_REQUEST;  // 继续读取下一行
}
```

**解析的头部字段：**

| 头部 | 作用 | 示例值 | 存储变量 |
|------|------|--------|----------|
| `Connection` | 是否保持连接 | `keep-alive` | `m_linger` |
| `Content-Length` | 正文长度 | `25` | `m_content_length` |
| `Host` | 主机名 | `localhost:9006` | `m_host` |

---

### 6.7 解析正文 parse_content()

**位置**：http_conn.cpp 第 330 行

```cpp
//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    // 检查正文是否读取完整
    // m_read_idx: 已读取的总字节数
    // m_checked_idx: 已分析的字节数
    // m_content_length: 正文长度
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';  // 添加字符串结束符
        m_string = text;  // 保存正文内容（如 "user=admin&passwd=123"）
        return GET_REQUEST;  // 请求完整了
    }
    return NO_REQUEST;  // 还没读完
}
```

**正文内容示例（POST 请求）：**

```
m_string = "user=admin&passwd=123456"

后续在 do_request() 中解析：
- 用户名: admin
- 密码: 123456
```

---

### 6.8 处理请求 do_request()

**位置**：http_conn.cpp 第 388 行

**作用**：根据解析结果处理请求，返回文件或验证登录

```cpp
http_conn::HTTP_CODE http_conn::do_request()
{
    // ===== 1. 拼接文件路径 =====
    strcpy(m_real_file, doc_root);  // doc_root = "./root"
    int len = strlen(doc_root);

    // 找到 URL 中最后一个 '/'
    const char *p = strrchr(m_url, '/');

    // ===== 2. 处理登录/注册（POST 请求）=====
    // URL 格式: /2log.html（登录）或 /3register.html（注册）
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        // 提取用户名和密码
        // m_string 格式: "user=admin&passwd=123456"
        char name[100], password[100];
        int i;

        // 提取用户名（从第5个字符开始，到 '&' 结束）
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        // 提取密码（从 "&passwd=" 之后开始）
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        // 注册（URL 第二个字符是 '3'）
        if (*(p + 1) == '3')
        {
            char sql_insert[200];
            sprintf(sql_insert,
                "INSERT INTO user(username, passwd) VALUES('%s', '%s')",
                name, password);

            // 检查用户名是否已存在
            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);  // 插入数据库
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");  // 注册成功，跳转登录页
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");  // 用户名已存在
        }
        // 登录（URL 第二个字符是 '2'）
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");  // 登录成功
            else
                strcpy(m_url, "/logError.html");  // 登录失败
        }
    }

    // ===== 3. URL 路由 =====
    // 根据URL第二个字符决定返回哪个页面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else
    {
        // 普通静态文件
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    // ===== 4. 检查文件 =====
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;  // 文件不存在

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;  // 无读取权限

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;  // 是目录，不是文件

    // ===== 5. mmap 映射文件到内存 =====
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size,
                                   PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    return FILE_REQUEST;
}
```

**URL 路由表：**

| 请求 URL | 实际文件 | 功能 |
|----------|----------|------|
| `/` | `/judge.html` | 首页 |
| `/0xxx` | `/register.html` | 注册页面 |
| `/1xxx` | `/log.html` | 登录页面 |
| `/2xxx` (POST) | 验证登录 | 登录验证 |
| `/3xxx` (POST) | 注册用户 | 注册验证 |
| `/5xxx` | `/picture.html` | 图片页面 |
| `/6xxx` | `/video.html` | 视频页面 |
| `/xxx` | `/xxx` | 静态文件 |

---

### 6.9 生成响应 process_write()

**位置**：http_conn.cpp 第 629 行

```cpp
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    // ===== 错误 500 =====
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        add_content(error_500_form);
        break;
    }

    // ===== 错误 404 =====
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        add_content(error_404_form);
        break;
    }

    // ===== 错误 403 =====
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        add_content(error_403_form);
        break;
    }

    // ===== 成功 200 =====
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);  // "HTTP/1.1 200 OK\r\n"

        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);

            // 设置分散写结构（两个缓冲区）
            m_iv[0].iov_base = m_write_buf;       // 缓冲区1：响应头
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;    // 缓冲区2：文件内容（mmap）
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else  // 空文件
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            add_content(ok_string);
        }
    }

    default:
        return false;
    }

    // 错误情况：只有一个缓冲区（响应头）
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
```

**生成的响应示例（成功）：**

```
m_write_buf (响应头):
┌─────────────────────────────────────┐
│ HTTP/1.1 200 OK\r\n                 │
│ Content-Length: 1234\r\n            │
│ Connection: keep-alive\r\n          │
│ \r\n                                │
└─────────────────────────────────────┘

m_file_address (文件内容，mmap):
┌─────────────────────────────────────┐
│ <html>                              │
│   <head>...</head>                  │
│   <body>Hello World</body>          │
│ </html>                             │
└─────────────────────────────────────┘
```

---

### 6.10 发送响应 write()

**位置**：http_conn.cpp 第 524 行

```cpp
bool http_conn::write()
{
    int temp = 0;

    // 没有数据要发送
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();  // 重置状态
        return true;
    }

    while (1)
    {
        // writev: 分散写，一次发送多个缓冲区
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)  // 发送缓冲区满了
            {
                // 注册 EPOLLOUT，等下次再发
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;  // 真正的错误
        }

        // 更新已发送字节数
        bytes_have_send += temp;
        bytes_to_send -= temp;

        // 调整 iovec 指针（处理部分发送的情况）
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            // 响应头发完了，调整文件指针
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            // 响应头还没发完，调整指针
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 发送完成
        if (bytes_to_send <= 0)
        {
            unmap();  // 解除 mmap 映射

            if (m_linger)  // 保持连接
            {
                init();  // 重置状态，等待下一个请求
                modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
                return true;
            }
            else
            {
                return false;  // 关闭连接
            }
        }
    }
}
```

**writev 分散写图解：**

```
发送前：
m_iv[0]: ┌─────────────────────────────┐
         │ HTTP/1.1 200 OK\r\n         │  100 字节
         │ Content-Length: 1234\r\n    │
         │ \r\n                        │
         └─────────────────────────────┘

m_iv[1]: ┌─────────────────────────────┐
         │ <html>...</html>            │  1234 字节（mmap）
         └─────────────────────────────┘

writev(m_sockfd, m_iv, 2) 一次性发送两个缓冲区

假设只发送了 50 字节：
m_iv[0]: ┌─────────────────────────────┐
         │ Content-Length: 1234\r\n    │  剩余 50 字节
         │ \r\n                        │
         └─────────────────────────────┘
         ↑ 起始位置后移 50 字节

下次调用 writev 时，从新位置继续发送
```

---

## 七、关键技术点

### 7.1 mmap 内存映射

```cpp
// 把文件映射到内存，直接像操作内存一样操作文件
int fd = open(filename, O_RDONLY);
m_file_address = (char *)mmap(0, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
close(fd);

// 用完后解除映射
munmap(m_file_address, file_size);
```

**优点**：
- 避免用户态和内核态之间的数据拷贝
- 不需要 read/write 系统调用
- 文件内容直接在内存中，可以像数组一样访问

**原理图：**

```
传统方式：
文件 ──read()──► 内核缓冲区 ──拷贝──► 用户缓冲区 ──write()──► socket

mmap 方式：
文件 ──mmap──► 内存映射区域（直接访问）──writev──► socket
                    ↓
              零拷贝！
```

### 7.2 writev 分散写

```cpp
struct iovec {
    void  *iov_base;  // 缓冲区起始地址
    size_t iov_len;   // 缓冲区长度
};

// 一次性写入多个不连续的缓冲区
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
```

**优点**：
- 减少系统调用次数（1 次 writev 代替 2 次 write）
- 适合发送"头部 + 正文"这种结构

**示例：**

```cpp
struct iovec iov[2];
iov[0].iov_base = header;   // 响应头
iov[0].iov_len = header_len;
iov[1].iov_base = body;     // 响应正文
iov[1].iov_len = body_len;

writev(sockfd, iov, 2);  // 一次发送两个缓冲区
```

### 7.3 非阻塞 I/O

```cpp
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}
```

**为什么需要非阻塞？**
- 配合 epoll ET 模式使用
- 避免在 read/write 时阻塞整个线程
- EAGAIN 表示"暂时没有数据"，不是错误

---

## 八、完整处理流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                    HTTP 请求处理完整流程                         │
└─────────────────────────────────────────────────────────────────┘

1. epoll 监听到读事件（EPOLLIN）
       │
       ▼
2. read_once() ──────────────────────────────────────────────────
       │  从 socket 读取数据到 m_read_buf
       │  LT: 读一次  /  ET: while 循环读完
       ▼
3. process() 入口函数 ───────────────────────────────────────────
       │
       ├─► process_read() 主状态机
       │       │
       │       ├─► parse_line() 从状态机，读一行
       │       │       找到 \r\n，替换为 \0\0
       │       │
       │       ├─► parse_request_line() 解析 "GET / HTTP/1.1"
       │       │       → 提取 m_method, m_url, m_version
       │       │       → 状态切换到 CHECK_STATE_HEADER
       │       │
       │       ├─► parse_headers() 解析头部
       │       │       → 提取 Connection, Content-Length, Host
       │       │       → 遇到空行，头部结束
       │       │
       │       └─► parse_content() 解析正文（POST）
       │               → 保存用户名密码到 m_string
       │
       ├─► do_request() 处理请求
       │       │
       │       ├─► URL 路由（/0, /1, /2, /3, /5, /6...）
       │       │
       │       ├─► 登录/注册验证（POST 请求）
       │       │       → 提取用户名密码
       │       │       → 查询/插入数据库
       │       │
       │       └─► mmap 映射文件到内存
       │               → m_file_address 指向文件内容
       │
       └─► process_write() 生成响应
               │
               ├─► add_status_line()  "HTTP/1.1 200 OK\r\n"
               │
               ├─► add_headers()      "Content-Length: xxx\r\n"
               │                      "Connection: xxx\r\n"
               │                      "\r\n"
               │
               └─► 设置 iovec（响应头 + 文件内容）
                       m_iv[0] = 响应头
                       m_iv[1] = 文件内容（mmap）
       │
       ▼
4. 注册 EPOLLOUT 事件
       │
       ▼
5. epoll 监听到写事件（EPOLLOUT）
       │
       ▼
6. write() 发送响应 ─────────────────────────────────────────────
       │  writev(m_sockfd, m_iv, m_iv_count)
       │  处理 EAGAIN（缓冲区满）
       │  发送完成后 unmap()
       ▼
7. 如果 keep-alive，重置状态，等待下一个请求
   否则，关闭连接
```

---

## 九、函数总结

| 函数 | 作用 | 关键点 |
|------|------|--------|
| `read_once()` | 读取请求数据 | LT/ET 模式差异 |
| `process()` | 入口函数 | 协调解析和响应 |
| `parse_line()` | 从状态机，读一行 | 把 \r\n 替换为 \0\0 |
| `process_read()` | 主状态机 | 状态切换逻辑 |
| `parse_request_line()` | 解析请求行 | strpbrk 分割字符串 |
| `parse_headers()` | 解析头部 | 空行表示结束 |
| `parse_content()` | 解析正文 | POST 数据 |
| `do_request()` | 处理请求 | URL 路由 + mmap |
| `process_write()` | 生成响应 | 设置 iovec |
| `write()` | 发送响应 | writev 分散写 |

---

## 十、核心思想总结

1. **状态机模式**：将复杂的 HTTP 解析过程分解为多个状态（请求行 → 头部 → 正文）

2. **零拷贝**：
   - mmap：文件直接映射到内存
   - writev：一次发送多个缓冲区

3. **非阻塞 I/O**：配合 epoll 实现高并发

4. **Reactor/Proactor 模式**：epoll 事件驱动

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
