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

**HTTP 请求行格式：**
```
GET /index.html HTTP/1.1
│       │          │
│       │          └── HTTP版本
│       └── URL
└── 请求方法
```

#### 6.5.1 涉及的 C 字符串函数

在解析之前，先了解代码中使用的字符串函数：

| 函数 | 作用 | 示例 |
|------|------|------|
| `strpbrk(s, chars)` | 找到第一个在 chars 中的字符 | `strpbrk("hello", "aeiou")` → 指向 'e' |
| `strspn(s, chars)` | 计算开头有多少字符在 chars 中 | `strspn("   abc", " ")` → 返回 3 |
| `strcasecmp(a, b)` | 忽略大小写比较字符串 | `strcasecmp("GET", "get")` → 返回 0（相等） |
| `strncasecmp(a, b, n)` | 忽略大小写比较前 n 个字符 | `strncasecmp("http://", "HTTP", 4)` → 返回 0 |
| `strchr(s, c)` | 找到字符 c 第一次出现的位置 | `strchr("/index", '/')` → 指向第一个 '/' |
| `strcat(dst, src)` | 拼接字符串 | `strcat(s, ".html")` → s 变成 "s.html" |
| `strlen(s)` | 字符串长度 | `strlen("abc")` → 返回 3 |

**strpbrk 详解：**

```cpp
char *strpbrk(const char *str, const char *accept);
// 作用：在 str 中找到第一个出现在 accept 中的字符
// 返回：找到则返回指向该字符的指针，没找到返回 NULL

// 示例
char text[] = "GET /index.html HTTP/1.1";
char *p = strpbrk(text, " \t");  // 找第一个空格或制表符
// p 指向 "GET" 后面的空格
```

**strspn 详解：**

```cpp
size_t strspn(const char *str, const char *accept);
// 作用：计算 str 开头有多少个字符在 accept 中
// 返回：匹配的字符数量

// 示例
strspn("   hello", " \t");  // 返回 3（开头有3个空格）
strspn("abc123", "abc");    // 返回 3（abc都在"abc"中）
```

#### 6.5.2 源码逐行详解

```cpp
//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // ========================================
    // 第1步：提取请求方法（GET/POST）
    // ========================================

    // strpbrk: 找到第一个空格或制表符的位置
    // 输入: "GET /index.html HTTP/1.1"
    //                ↑
    //           m_url 指向这里
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;  // 没有空格，格式错误
    }

    // 这一行做了两件事：
    // 1. *m_url = '\0' - 把空格改成字符串结束符
    // 2. m_url++ - 指针后移一位
    *m_url++ = '\0';

    // 现在：
    // text = "GET"（因为空格被改成了 \0）
    // m_url 指向 "/index.html HTTP/1.1"

    char *method = text;  // method = "GET"

    // strcasecmp: 忽略大小写比较字符串
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;  // cgi标志位，表示需要解析请求体
    }
    else
        return BAD_REQUEST;  // 仅支持GET和POST方法

    // ========================================
    // 第2步：提取 URL
    // ========================================

    // strspn: 跳过空格和制表符
    // 输入: "   /index.html HTTP/1.1"
    //       ^^^
    //       有3个空格，strspn 返回 3
    // m_url += 3 后指向 "/index.html HTTP/1.1"
    m_url += strspn(m_url, " \t");

    // ========================================
    // 第3步：提取 HTTP 版本号
    // ========================================

    // 再次用 strpbrk 找下一个空格
    // 输入: "/index.html HTTP/1.1"
    //                   ↑
    //              m_version 指向这里
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;

    // 同样的技巧：截断并后移
    *m_version++ = '\0';

    // 现在：
    // m_url = "/index.html"
    // m_version 指向 " HTTP/1.1"

    // 跳过空格
    m_version += strspn(m_version, " \t");

    // 检查版本号（仅支持 HTTP/1.1）
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    // ========================================
    // 第4步：处理完整 URL（带协议前缀）
    // ========================================

    // 如果 URL 是完整格式 "http://localhost/index.html"
    // 需要提取出 "/index.html"

    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;  // 跳过 "http://"
        // 现在 m_url = "localhost/index.html"
        m_url = strchr(m_url, '/');  // 找到第一个 '/'
        // 现在 m_url = "/index.html"
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    // ========================================
    // 第5步：检查 URL 合法性
    // ========================================

    // URL 必须以 '/' 开头
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;

    // 当 url 为 "/" 时，显示首页 judge.html
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    // 现在 m_url = "/judge.html"

    // ========================================
    // 第6步：状态转换
    // ========================================

    m_check_state = CHECK_STATE_HEADER;  // 接下来解析头部
    return NO_REQUEST;  // 请求还没处理完，继续读取
}
```

#### 6.5.3 完整流程图

```
输入: "GET /index.html HTTP/1.1"
         │
         ▼
┌─────────────────────────────────────────┐
│ 第1步: strpbrk 找第一个空格              │
│                                         │
│ "GET /index.html HTTP/1.1"              │
│     ↑                                   │
│   m_url                                 │
└─────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────┐
│ 第2步: *m_url++ = '\0' 截断              │
│                                         │
│ "GET\0/index.html HTTP/1.1"             │
│       ↑                                 │
│     m_url (后移了)                       │
│                                         │
│ text = "GET"                            │
│ m_url 指向 "/index.html HTTP/1.1"       │
└─────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────┐
│ 第3步: 判断请求方法                       │
│                                         │
│ strcasecmp(text, "GET") == 0            │
│ m_method = GET                          │
└─────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────┐
│ 第4步: strspn 跳过空格                    │
│                                         │
│ m_url += strspn(m_url, " \t");          │
│ m_url 指向 "/index.html HTTP/1.1"       │
└─────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────┐
│ 第5步: strpbrk 找下一个空格               │
│                                         │
│ "/index.html HTTP/1.1"                  │
│             ↑                           │
│         m_version                       │
└─────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────┐
│ 第6步: *m_version++ = '\0' 截断          │
│                                         │
│ m_url = "/index.html"                   │
│ m_version = "HTTP/1.1"                  │
└─────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────┐
│ 最终结果:                                │
│                                         │
│ m_method  = GET                         │
│ m_url     = "/index.html"               │
│ m_version = "HTTP/1.1"                  │
│                                         │
│ m_check_state = CHECK_STATE_HEADER      │
└─────────────────────────────────────────┘
```

#### 6.5.4 内存变化图解

```
原始缓冲区（text 指向这里）:
┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
│ G │ E │ T │   │ / │ i │ n │ d │ e │ x │ . │ h │ t │ m │ l │   │ H │ T │...
└───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16  17

第一次截断后（*m_url++ = '\0'）:
┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
│ G │ E │ T │ \0│ / │ i │ n │ d │ e │ x │ . │ h │ t │ m │ l │   │ H │ T │...
└───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
              ↑   ↑
           \0  m_url 指向这里（位置4）

此时:
- text = "GET"（因为位置3是 \0）
- m_url 指向 "/index.html HTTP/1.1"

第二次截断后:
┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
│ G │ E │ T │ \0│ / │ i │ n │ d │ e │ x │ . │ h │ t │ m │ l │ \0│ H │ T │...
└───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
                                                          ↑
                                                    m_version 指向这里

最终:
- text (method) = "GET"
- m_url = "/index.html"
- m_version = "HTTP/1.1"
```

#### 6.5.5 关键技巧总结

**技巧1：用 \0 截断字符串**

```cpp
*m_url++ = '\0';
// 等价于：
*m_url = '\0';
m_url = m_url + 1;
```

这样可以把一个长字符串"切分"成多个独立的 C 字符串。

**技巧2：strspn 跳过空白字符**

```cpp
m_url += strspn(m_url, " \t");
```

跳过开头的所有空格和制表符。

**技巧3：strpbrk 找分隔符**

```cpp
char *p = strpbrk(text, " \t");
```

找到第一个空格或制表符，用于分割字段。

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

**位置**：http_conn.cpp 第 411 行

**作用**：这是 HTTP 请求处理的**核心函数**，负责：
1. URL 路由（根据 URL 决定返回什么内容）
2. 登录/注册验证（POST 请求）
3. 文件查找和权限检查
4. mmap 映射文件到内存

#### 6.8.1 函数整体流程

```
do_request()
    │
    ├─► 1. 拼接文件路径（doc_root + URL）
    │
    ├─► 2. 判断是否是 POST 请求（cgi == 1）
    │       │
    │       ├─► 登录验证（URL 以 /2 开头）
    │       │
    │       └─► 注册验证（URL 以 /3 开头）
    │
    ├─► 3. URL 路由（根据 URL 第二个字符）
    │       │
    │       ├─► /0xxx → 注册页面
    │       ├─► /1xxx → 登录页面
    │       ├─► /5xxx → 图片页面
    │       ├─► /6xxx → 视频页面
    │       └─► 其他 → 静态文件
    │
    ├─► 4. 检查文件（是否存在、权限、是否是目录）
    │
    └─► 5. mmap 映射文件到内存
```

#### 6.8.2 涉及的 C 函数

| 函数 | 作用 | 示例 |
|------|------|------|
| `strcpy(dst, src)` | 复制字符串 | `strcpy(buf, "hello")` |
| `strcat(dst, src)` | 拼接字符串 | `strcat(buf, ".html")` |
| `strncpy(dst, src, n)` | 复制前 n 个字符 | `strncpy(buf, src, 10)` |
| `strrchr(s, c)` | 从后往前找字符 | `strrchr("/a/b/c", '/')` → 指向 "/c" |
| `stat(path, &st)` | 获取文件信息 | 检查文件是否存在、大小等 |
| `open(path, flags)` | 打开文件 | `open("a.txt", O_RDONLY)` |
| `mmap()` | 内存映射 | 将文件映射到内存 |
| `mysql_query()` | 执行 SQL 语句 | 查询/插入数据库 |

#### 6.8.3 源码逐行详解

```cpp
http_conn::HTTP_CODE http_conn::do_request()
{
    // ========================================
    // 第1步：拼接文件路径
    // ========================================

    // doc_root = "./root"（网站根目录）
    // m_real_file 将存储完整的文件路径
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // 现在 m_real_file = "./root"

    // 找到 URL 中最后一个 '/'
    // 例如：m_url = "/2log.html"
    // strrchr 返回指向最后一个 '/' 的指针
    const char *p = strrchr(m_url, '/');
    // p 指向 "/2log.html" 中的 '/'
    // p + 1 指向 "2log.html"

    // ========================================
    // 第2步：处理登录/注册（POST 请求）
    // ========================================

    // cgi == 1 表示是 POST 请求
    // *(p + 1) == '2' 表示登录
    // *(p + 1) == '3' 表示注册
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        // 提取实际要访问的文件名
        // m_url = "/2log.html"
        // m_url + 2 = "log.html"
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");        // m_url_real = "/"
        strcat(m_url_real, m_url + 2);  // m_url_real = "/log.html"
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        // m_real_file = "./root/log.html"
        free(m_url_real);

        // ----- 提取用户名和密码 -----
        // m_string 是 POST 请求的正文
        // 格式: "user=admin&passwd=123456"
        //       0123456789...
        //       user= 是5个字符
        //       &passwd= 是9个字符

        char name[100], password[100];
        int i;

        // 提取用户名
        // 从 m_string[5] 开始（跳过 "user="）
        // 到 '&' 结束
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';
        // 假设 m_string = "user=admin&passwd=123456"
        // i 从 5 开始，m_string[5] = 'a'
        // 循环到 m_string[10] = '&' 结束
        // name = "admin"

        // 提取密码
        // i 现在指向 '&'，i + 10 跳过 "&passwd="
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';
        // password = "123456"

        // ----- 注册处理 -----
        if (*(p + 1) == '3')  // URL 以 '/3' 开头
        {
            // 构建 SQL 插入语句
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            // sql_insert = "INSERT INTO user(username, passwd) VALUES('admin', '123456')"

            // 检查用户名是否已存在
            // users 是一个 map<string, string>，存储所有用户
            if (users.find(name) == users.end())  // 用户名不存在
            {
                m_lock.lock();  // 加锁（多线程安全）
                int res = mysql_query(mysql, sql_insert);  // 执行 SQL
                users.insert(pair<string, string>(name, password));  // 更新内存
                m_lock.unlock();  // 解锁

                if (!res)  // res == 0 表示成功
                    strcpy(m_url, "/log.html");  // 注册成功，跳转登录页
                else
                    strcpy(m_url, "/registerError.html");  // 数据库错误
            }
            else
                strcpy(m_url, "/registerError.html");  // 用户名已存在

            free(sql_insert);
        }
        // ----- 登录处理 -----
        else if (*(p + 1) == '2')  // URL 以 '/2' 开头
        {
            // 验证用户名和密码
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");  // 登录成功
            else
                strcpy(m_url, "/logError.html");  // 登录失败
        }
    }

    // ========================================
    // 第3步：URL 路由
    // ========================================

    // 根据 URL 第二个字符决定返回哪个页面
    // *(p + 1) 是 URL 中 '/' 后面的第一个字符

    if (*(p + 1) == '0')  // /0xxx → 注册页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
        // m_real_file = "./root/register.html"
    }
    else if (*(p + 1) == '1')  // /1xxx → 登录页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
        // m_real_file = "./root/log.html"
    }
    else if (*(p + 1) == '5')  // /5xxx → 图片页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '6')  // /6xxx → 视频页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '7')  // /7xxx → 粉丝页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else  // 其他 → 直接访问静态文件
    {
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
        // m_url = "/index.html"
        // m_real_file = "./root/index.html"
    }

    // ========================================
    // 第4步：检查文件
    // ========================================

    // stat() 获取文件信息，失败返回 -1
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;  // 文件不存在（404）

    // 检查文件权限
    // S_IROTH: 其他用户是否有读权限
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;  // 无权限（403）

    // 检查是否是目录
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;  // 是目录，不是文件（400）

    // ========================================
    // 第5步：mmap 映射文件到内存
    // ========================================

    // 以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);

    // mmap: 将文件映射到内存
    // 参数:
    //   NULL (0): 让内核选择映射地址
    //   m_file_stat.st_size: 映射的大小（文件大小）
    //   PROT_READ: 只读权限
    //   MAP_PRIVATE: 私有映射（修改不会影响原文件）
    //   fd: 文件描述符
    //   0: 偏移量（从文件开头开始）
    m_file_address = (char *)mmap(0, m_file_stat.st_size,
                                   PROT_READ, MAP_PRIVATE, fd, 0);

    close(fd);  // 映射后可以关闭文件描述符

    return FILE_REQUEST;  // 返回成功，准备发送文件
}
```

#### 6.8.4 URL 路由详解

**路由原理：**

URL 格式为 `/Xxxx`，其中 `X` 是第一个字符（决定了路由）：

```
URL: /2log.html
      ↑
      这是第二个字符（p + 1 指向这里）
```

**路由表：**

| 请求 URL | URL 第二字符 | 实际文件 | 功能 |
|----------|-------------|----------|------|
| `/` | 无 | `/judge.html` | 首页（在 parse_request_line 中处理） |
| `/0xxx` | `'0'` | `/register.html` | 注册页面 |
| `/1xxx` | `'1'` | `/log.html` | 登录页面 |
| `/2xxx` (POST) | `'2'` | 登录验证 | 验证用户名密码 |
| `/3xxx` (POST) | `'3'` | 注册验证 | 插入数据库 |
| `/5xxx` | `'5'` | `/picture.html` | 图片页面 |
| `/6xxx` | `'6'` | `/video.html` | 视频页面 |
| `/7xxx` | `'7'` | `/fans.html` | 粉丝页面 |
| `/xxx` | 其他 | `/xxx` | 直接访问静态文件 |

**路由流程图：**

```
请求 URL: "/2log.html"
         │
         ▼
┌─────────────────────────────────────┐
│ strrchr(m_url, '/') 找到最后一个 '/' │
│ p 指向 "/2log.html" 中的 '/'         │
│ p + 1 指向 "2log.html"               │
│ *(p + 1) = '2'                       │
└─────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────┐
│ 判断 *(p + 1) 的值                   │
│                                     │
│ '0' → 注册页面                       │
│ '1' → 登录页面                       │
│ '2' + POST → 登录验证                │
│ '3' + POST → 注册验证                │
│ '5' → 图片页面                       │
│ '6' → 视频页面                       │
│ 其他 → 静态文件                      │
└─────────────────────────────────────┘
```

#### 6.8.5 登录/注册流程详解

**POST 请求正文格式：**

```
m_string = "user=admin&passwd=123456"
           ├─────┤ ├──────┤
           5个字符  9个字符（包括&）

解析用户名：
位置: 0123456789...
内容: user=admin&passwd=123456
           ↑    ↑
          [5]  [&]
      从这里开始  到这里结束

解析密码：
位置: 01234567890123456789...
内容: user=admin&passwd=123456
                    ↑
              [i+10]（跳过&passwd=）
```

**登录验证流程：**

```
用户提交登录表单
         │
         ▼
┌─────────────────────────────────────┐
│ POST /2log.html                     │
│ Body: user=admin&passwd=123456      │
└─────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────┐
│ do_request() 解析                    │
│ name = "admin"                      │
│ password = "123456"                 │
└─────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────┐
│ 查询 users map                       │
│ users.find("admin") != end?         │
│ users["admin"] == "123456"?         │
└─────────────────────────────────────┘
         │
    ┌────┴────┐
    │         │
   成功       失败
    │         │
    ▼         ▼
/welcome.html  /logError.html
```

**注册验证流程：**

```
用户提交注册表单
         │
         ▼
┌─────────────────────────────────────┐
│ POST /3register.html                │
│ Body: user=newuser&passwd=123456    │
└─────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────┐
│ do_request() 解析                    │
│ name = "newuser"                    │
│ password = "123456"                 │
└─────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────┐
│ 检查用户名是否存在                    │
│ users.find("newuser") == end?       │
└─────────────────────────────────────┘
         │
    ┌────┴────┐
    │         │
  不存在     已存在
    │         │
    ▼         ▼
┌─────────┐  /registerError.html
│ 插入数据库│
│ 更新 map │
└────┬────┘
     │
     ▼
/log.html（跳转登录页）
```

#### 6.8.6 mmap 内存映射详解

**为什么用 mmap？**

```
传统方式读取文件并发送：
┌─────────┐    read()    ┌─────────┐    write()   ┌─────────┐
│  文件   │ ──────────► │ 用户缓冲区 │ ──────────► │  socket │
└─────────┘             └─────────┘             └─────────┘
                          拷贝1次

mmap 方式：
┌─────────┐    mmap()   ┌─────────┐   writev()  ┌─────────┐
│  文件   │ ──────────► │ 内存映射  │ ──────────► │  socket │
└─────────┘   (零拷贝)   └─────────┘             └─────────┘
```

**mmap 函数参数：**

```cpp
void *mmap(
    void *addr,      // 映射起始地址（NULL 表示让系统选择）
    size_t length,   // 映射长度（文件大小）
    int prot,        // 内存保护标志
    int flags,       // 映射类型
    int fd,          // 文件描述符
    off_t offset     // 文件偏移量
);

// 本项目中的调用：
m_file_address = (char *)mmap(
    0,                      // 让系统选择地址
    m_file_stat.st_size,    // 文件大小
    PROT_READ,              // 只读
    MAP_PRIVATE,            // 私有映射（修改不影响原文件）
    fd,                     // 文件描述符
    0                       // 从文件开头开始
);
```

**使用 mmap 后的发送流程：**

```
mmap 映射后:
┌─────────────────────────────────────────────┐
│              虚拟内存空间                     │
│  ┌───────────────────────────────────────┐  │
│  │ m_file_address                        │  │
│  │   ↓                                   │  │
│  │ <html><body>Hello</body></html>       │  │
│  │ (文件内容直接映射到内存)                 │  │
│  └───────────────────────────────────────┘  │
└─────────────────────────────────────────────┘
                    │
                    │ writev()
                    ▼
              发送到 socket
```

#### 6.8.7 文件检查流程

```cpp
// 1. 检查文件是否存在
if (stat(m_real_file, &m_file_stat) < 0)
    return NO_RESOURCE;  // 404

// 2. 检查文件权限
if (!(m_file_stat.st_mode & S_IROTH))
    return FORBIDDEN_REQUEST;  // 403

// 3. 检查是否是目录
if (S_ISDIR(m_file_stat.st_mode))
    return BAD_REQUEST;  // 400
```

**返回值说明：**

| 返回值 | HTTP 状态码 | 含义 |
|--------|-------------|------|
| `FILE_REQUEST` | 200 | 文件请求成功 |
| `NO_RESOURCE` | 404 | 文件不存在 |
| `FORBIDDEN_REQUEST` | 403 | 无权限 |
| `BAD_REQUEST` | 400 | 请求格式错误 |

#### 6.8.8 完整流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                    do_request() 完整流程                         │
└─────────────────────────────────────────────────────────────────┘

输入: m_url = "/2log.html", m_string = "user=admin&passwd=123"
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 第1步: 拼接文件路径                                              │
│ strcpy(m_real_file, doc_root);  // "./root"                     │
│ p = strrchr(m_url, '/');        // 指向 "/"                     │
└─────────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 第2步: 判断是否 POST 请求                                        │
│ cgi == 1 && (*(p+1) == '2' || *(p+1) == '3')                    │
│                                                                 │
│ 如果是:                                                          │
│   - 提取用户名和密码                                             │
│   - '2' → 登录验证                                               │
│   - '3' → 注册验证                                               │
│   - 更新 m_url 为结果页面                                        │
└─────────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 第3步: URL 路由                                                  │
│ 根据 *(p+1) 的值决定实际文件                                     │
│                                                                 │
│ '0' → /register.html                                            │
│ '1' → /log.html                                                 │
│ '5' → /picture.html                                             │
│ '6' → /video.html                                               │
│ 其他 → 直接使用 m_url                                            │
└─────────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 第4步: 检查文件                                                  │
│ stat() → 文件是否存在?                                           │
│ 权限检查 → 是否可读?                                             │
│ 类型检查 → 是否是目录?                                           │
└─────────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 第5步: mmap 映射                                                 │
│ fd = open(m_real_file, O_RDONLY);                               │
│ m_file_address = mmap(...);                                     │
│ close(fd);                                                      │
│                                                                 │
│ 返回 FILE_REQUEST                                               │
└─────────────────────────────────────────────────────────────────┘
         │
         ▼
    返回给 process_write() 生成响应
```

#### 6.8.9 关键点总结

1. **URL 路由**：通过 URL 第二个字符决定返回内容
2. **CGI 标志**：`cgi == 1` 表示需要处理 POST 正文
3. **用户验证**：使用 map 存储用户信息，快速查找
4. **线程安全**：数据库操作需要加锁
5. **mmap**：零拷贝技术，提高文件传输效率

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

### 6.11 初始化函数 init()（无参版本）

**位置**：http_conn.cpp 第 136 行

**作用**：重置 http_conn 对象的所有内部状态，为处理新的 HTTP 请求做准备。

```cpp
//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;  // 初始状态：解析请求行
    m_linger = false;                         // 默认不保持连接
    m_method = GET;                           // 默认 GET 方法
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    // 清空缓冲区
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}
```

**变量初始化说明表：**

| 变量 | 初始值 | 含义 |
|------|--------|------|
| `m_check_state` | `CHECK_STATE_REQUESTLINE` | 主状态机初始状态 |
| `m_linger` | `false` | 默认短连接 |
| `m_method` | `GET` | 默认 GET 请求 |
| `m_read_idx` | `0` | 读缓冲区已用位置 |
| `m_checked_idx` | `0` | 已分析位置 |
| `m_write_idx` | `0` | 写缓冲区已用位置 |
| `m_content_length` | `0` | 正文长度 |
| `cgi` | `0` | 非 POST 请求 |

**何时调用？**

```
┌─────────────────────────────────────────────────────────────────┐
│                    init() 调用时机                               │
└─────────────────────────────────────────────────────────────────┘

1. 新连接到来时
   init(sockfd, addr, ...)  →  内部调用 init()

2. keep-alive 连接处理完一个请求后
   write() 发送完成  →  if (m_linger) { init(); }

3. 响应发送完毕，准备接收下一个请求时
   write()  →  bytes_to_send == 0  →  init()
```

---

### 6.12 初始化函数 init()（有参版本）

**位置**：http_conn.cpp 第 113 行

**作用**：当新连接到来时，由外部调用此函数初始化连接。

```cpp
//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;            // 保存 socket 文件描述符
    m_address = addr;             // 保存客户端地址

    addfd(m_epollfd, sockfd, true, m_TRIGMode);  // 注册到 epoll
    m_user_count++;               // 用户数 +1

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;              // 网站根目录（如 "./root"）
    m_TRIGMode = TRIGMode;        // 触发模式（0=LT, 1=ET）
    m_close_log = close_log;      // 是否关闭日志

    strcpy(sql_user, user.c_str());      // 数据库用户名
    strcpy(sql_passwd, passwd.c_str());  // 数据库密码
    strcpy(sql_name, sqlname.c_str());   // 数据库名

    init();  // 调用无参版本，重置内部状态
}
```

**参数详解：**

| 参数 | 类型 | 说明 | 示例值 |
|------|------|------|--------|
| `sockfd` | `int` | 客户端 socket 文件描述符 | `accept()` 返回值 |
| `addr` | `sockaddr_in&` | 客户端地址结构体 | 包含 IP、端口 |
| `root` | `char*` | 网站根目录 | `"./root"` |
| `TRIGMode` | `int` | 触发模式 | `0`=LT, `1`=ET |
| `close_log` | `int` | 是否关闭日志 | `0`=开启, `1`=关闭 |
| `user` | `string` | 数据库用户名 | `"root"` |
| `passwd` | `string` | 数据库密码 | `"root"` |
| `sqlname` | `string` | 数据库名 | `"yourdb"` |

**调用流程图：**

```
main.cpp 中的流程：
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ accept() 接受新连接                                              │
│ 返回 connfd                                                      │
└─────────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ users[connfd].init(connfd, client_address, root, mode, ...)     │
│                                                                 │
│ 做了什么：                                                        │
│ 1. 保存 socket 和地址                                            │
│ 2. 注册到 epoll（addfd）                                          │
│ 3. 用户计数 +1                                                   │
│ 4. 保存配置信息（根目录、触发模式、数据库信息）                      │
│ 5. 调用 init() 重置内部状态                                       │
└─────────────────────────────────────────────────────────────────┘
```

---

### 6.13 关闭连接 close_conn()

**位置**：http_conn.cpp 第 101 行

**作用**：关闭客户端连接，清理资源。

```cpp
//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);  // 从 epoll 移除
        m_sockfd = -1;                   // 标记为无效
        m_user_count--;                  // 用户数 -1
    }
}
```

**参数说明：**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `real_close` | `true` | 是否真正关闭（可以用于延迟关闭） |

**关闭流程：**

```
close_conn() 调用
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 检查 real_close && m_sockfd != -1                               │
│                                                                 │
│ 如果条件满足：                                                    │
│   1. removefd() - 从 epoll 删除，并 close socket                 │
│   2. m_sockfd = -1 - 标记为已关闭                                │
│   3. m_user_count-- - 用户计数减一                               │
└─────────────────────────────────────────────────────────────────┘
```

**何时调用？**

```cpp
// 1. 对方关闭连接时（在 WebServer.cpp 中）
if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
    users[sockfd].close_conn();
}

// 2. 处理出错时
if (!write_ret) {
    close_conn();
}

// 3. 非 keep-alive 连接发送完成后
if (!m_linger) {
    return false;  // 在 write() 返回后，外部会调用 close_conn()
}
```

---

### 6.14 设置非阻塞 setnonblocking()

**位置**：http_conn.cpp 第 51 行

**作用**：将文件描述符设置为非阻塞模式。

```cpp
//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);      // 获取当前标志
    int new_option = old_option | O_NONBLOCK; // 添加非阻塞标志
    fcntl(fd, F_SETFL, new_option);           // 设置新标志
    return old_option;                         // 返回旧标志（方便恢复）
}
```

**fcntl 函数详解：**

```cpp
#include <fcntl.h>

// fcntl - 对文件描述符进行各种控制操作
int fcntl(int fd, int cmd, ... /* arg */ );

// 常用命令：
// F_GETFL - 获取文件状态标志
// F_SETFL - 设置文件状态标志

// 文件状态标志：
// O_NONBLOCK - 非阻塞模式
// O_APPEND   - 追加模式
// O_ASYNC    - 异步 I/O
```

**阻塞 vs 非阻塞对比：**

```
阻塞模式：
┌─────────────────────────────────────────────────────────────────┐
│ recv(fd, buf, size, 0);                                          │
│                                                                 │
│ 如果没有数据：                                                    │
│   → 线程被挂起，等待数据到来                                       │
│   → 线程什么都做不了                                               │
│   → 其他连接也被阻塞                                               │
└─────────────────────────────────────────────────────────────────┘

非阻塞模式：
┌─────────────────────────────────────────────────────────────────┐
│ recv(fd, buf, size, 0);                                          │
│                                                                 │
│ 如果没有数据：                                                    │
│   → 立即返回 -1                                                   │
│   → errno = EAGAIN 或 EWOULDBLOCK                                │
│   → 线程可以处理其他事情                                           │
│   → 配合 epoll 使用，效率更高                                      │
└─────────────────────────────────────────────────────────────────┘
```

**为什么 ET 模式必须用非阻塞？**

```
ET 模式 + 阻塞 socket = 死锁！

场景：
1. epoll 通知可读（ET 只通知一次）
2. 循环读取数据...
3. 数据读完，调用 recv()
4. recv() 阻塞等待（因为 socket 是阻塞的）
5. epoll 不会再通知（ET 只通知一次）
6. 线程永远卡在 recv()！

ET 模式 + 非阻塞 socket = 正确！

场景：
1. epoll 通知可读
2. 循环读取数据...
3. 数据读完，调用 recv()
4. recv() 返回 -1，errno = EAGAIN
5. 检测到 EAGAIN，跳出循环
6. 完美！
```

---

### 6.15 注册 epoll 事件 addfd()

**位置**：http_conn.cpp 第 60 行

**作用**：将文件描述符添加到 epoll 实例中，注册读事件。

```cpp
//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;  // ET 模式
    else
        event.events = EPOLLIN | EPOLLRDHUP;            // LT 模式

    if (one_shot)
        event.events |= EPOLLONESHOT;

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);  // 添加到 epoll
    setnonblocking(fd);                              // 设置非阻塞
}
```

**epoll 事件类型详解：**

| 事件 | 说明 | 用途 |
|------|------|------|
| `EPOLLIN` | 可读事件 | socket 有数据可读 |
| `EPOLLOUT` | 可写事件 | socket 可以发送数据 |
| `EPOLLET` | 边缘触发模式 | 只在状态变化时通知 |
| `EPOLLRDHUP` | 对方关闭连接 | 检测客户端断开 |
| `EPOLLONESHOT` | 一次性事件 | 防止多线程竞争 |

**EPOLLONESHOT 详解：**

```
没有 EPOLLONESHOT：
┌─────────────────────────────────────────────────────────────────┐
│ 线程1 正在处理 socket A 的请求                                    │
│                                                                 │
│ 此时 socket A 又来了新数据                                        │
│ epoll 再次通知 socket A 可读                                      │
│ 线程2 也开始处理 socket A                                         │
│                                                                 │
│ → 两个线程同时处理一个连接！                                       │
│ → 数据竞争！混乱！                                                │
└─────────────────────────────────────────────────────────────────┘

使用 EPOLLONESHOT：
┌─────────────────────────────────────────────────────────────────┐
│ 线程1 正在处理 socket A 的请求                                    │
│                                                                 │
│ 即使 socket A 又来了新数据                                        │
│ epoll 也不会再次通知                                              │
│                                                                 │
│ 线程1 处理完后，调用 modfd() 重置事件                              │
│ epoll 才会再次通知                                                │
│                                                                 │
│ → 同一时刻只有一个线程处理一个连接                                 │
│ → 安全！                                                         │
└─────────────────────────────────────────────────────────────────┘
```

---

### 6.16 修改 epoll 事件 modfd()

**位置**：http_conn.cpp 第 84 行

**作用**：修改已注册的 epoll 事件（如从读事件改为写事件）。

```cpp
//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);  // 修改事件
}
```

**常见使用场景：**

```cpp
// 1. 请求不完整，继续等待数据
modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

// 2. 响应生成完毕，准备发送
modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);

// 3. 发送缓冲区满，等待可写
if (errno == EAGAIN) {
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
```

**状态转换图：**

```
┌─────────────────────────────────────────────────────────────────┐
│                    epoll 事件状态转换                            │
└─────────────────────────────────────────────────────────────────┘

                    ┌──────────┐
                    │  新连接   │
                    └────┬─────┘
                         │ addfd(EPOLLIN)
                         ▼
              ┌──────────────────────┐
              │    EPOLLIN 状态      │◄─────────────────┐
              │   （等待请求数据）    │                  │
              └──────────┬───────────┘                  │
                         │ 数据到来                      │
                         ▼                              │
              ┌──────────────────────┐                  │
              │    read_once()       │                  │
              │    process()         │                  │
              └──────────┬───────────┘                  │
                         │                              │
            ┌────────────┴────────────┐                 │
            │                         │                 │
     请求不完整                  请求完整                │
            │                         │                 │
            │                         ▼                 │
            │              ┌──────────────────────┐     │
            │              │    EPOLLOUT 状态     │     │
            │              │    （发送响应）       │     │
            │              └──────────┬───────────┘     │
            │                         │                 │
            │                         ▼                 │
            │              ┌──────────────────────┐     │
            │              │      write()         │     │
            │              └──────────┬───────────┘     │
            │                         │                 │
            │              ┌──────────┴───────────┐     │
            │              │                      │     │
            │         发送完成              keep-alive? │
            │              │                      │     │
            │              │                 是  │     │ 否
            │              │                      ▼     ▼
            │              │              ┌─────────┐ ┌─────────┐
            │              │              │ init()  │ │关闭连接  │
            │              │              │ 重置状态 │ │         │
            │              │              └────┬────┘ └─────────┘
            │              │                   │
            └──────────────┴───────────────────┘
                     modfd(EPOLLIN)
```

---

### 6.17 从 epoll 移除 removefd()

**位置**：http_conn.cpp 第 77 行

**作用**：从 epoll 实例中删除文件描述符，并关闭 socket。

```cpp
//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);  // 从 epoll 删除
    close(fd);                                  // 关闭文件描述符
}
```

**epoll_ctl 操作类型：**

| 操作 | 宏 | 说明 |
|------|-----|------|
| 添加 | `EPOLL_CTL_ADD` | 注册新的 fd 到 epoll |
| 修改 | `EPOLL_CTL_MOD` | 修改已注册 fd 的事件 |
| 删除 | `EPOLL_CTL_DEL` | 从 epoll 中删除 fd |

---

### 6.18 MySQL 初始化 initmysql_result()

**位置**：http_conn.cpp 第 20 行

**作用**：从数据库加载所有用户信息到内存（map）中，加速后续的登录验证。

```cpp
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);  // RAII 自动管理连接

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);  // 用户名
        string temp2(row[1]);  // 密码
        users[temp1] = temp2;  // 存入 map
    }
}
```

**数据流程图：**

```
┌─────────────────────────────────────────────────────────────────┐
│                    用户数据加载流程                               │
└─────────────────────────────────────────────────────────────────┘

MySQL 数据库：
┌─────────────────────────────────┐
│         user 表                  │
├─────────────┬───────────────────┤
│  username   │      passwd       │
├─────────────┼───────────────────┤
│   admin     │     123456        │
│   test      │     abc123        │
│   user1     │     password      │
└─────────────┴───────────────────┘
            │
            │ mysql_query("SELECT username,passwd FROM user")
            ▼
┌─────────────────────────────────┐
│       MYSQL_RES 结果集           │
│   row[0] = "admin"              │
│   row[1] = "123456"             │
└─────────────────────────────────┘
            │
            │ while (MYSQL_ROW row = mysql_fetch_row(result))
            ▼
内存中的 map<string, string> users：
┌─────────────────────────────────┐
│  users["admin"] = "123456"      │
│  users["test"] = "abc123"       │
│  users["user1"] = "password"    │
└─────────────────────────────────┘
            │
            │ 后续登录验证直接查 map
            ▼
┌─────────────────────────────────┐
│ if (users[name] == password)    │
│     → 登录成功                   │
└─────────────────────────────────┘
```

**为什么加载到内存？**

```
每次登录都查数据库：
┌─────────────────────────────────────────────────────────────────┐
│ 用户登录 → 连接数据库 → 执行 SQL → 获取结果 → 验证               │
│                                                                 │
│ 问题：                                                          │
│ 1. 每次都要建立数据库连接（慢）                                   │
│ 2. 执行 SQL 查询（慢）                                           │
│ 3. 高并发时数据库压力大                                          │
└─────────────────────────────────────────────────────────────────┘

加载到内存后：
┌─────────────────────────────────────────────────────────────────┐
│ 启动时：加载所有用户到 map                                        │
│                                                                 │
│ 用户登录 → 直接查 map（O(1) 时间复杂度）→ 验证                    │
│                                                                 │
│ 优点：                                                          │
│ 1. 不需要每次连接数据库                                          │
│ 2. map 查找是 O(1)，非常快                                       │
│ 3. 减轻数据库压力                                                │
│                                                                 │
│ 注意：注册新用户时要同时更新 map 和数据库                          │
└─────────────────────────────────────────────────────────────────┘
```

**MySQL C API 关键函数：**

| 函数 | 作用 |
|------|------|
| `mysql_query()` | 执行 SQL 语句 |
| `mysql_store_result()` | 获取完整结果集 |
| `mysql_num_fields()` | 获取列数 |
| `mysql_fetch_fields()` | 获取字段信息 |
| `mysql_fetch_row()` | 获取下一行数据 |
| `mysql_error()` | 获取错误信息 |

---

### 6.19 解除 mmap 映射 unmap()

**位置**：http_conn.cpp 第 539 行

**作用**：解除文件的内存映射，释放资源。

```cpp
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
```

**mmap/munmap 配对使用：**

```
do_request() 中：
┌─────────────────────────────────────────────────────────────────┐
│ m_file_address = (char *)mmap(0, m_file_stat.st_size,           │
│                                PROT_READ, MAP_PRIVATE, fd, 0);  │
│                                                                 │
│ → 文件被映射到内存                                               │
│ → m_file_address 指向文件内容                                    │
└─────────────────────────────────────────────────────────────────┘
            │
            │ 文件内容通过 writev() 发送
            ▼
write() 发送完成后：
┌─────────────────────────────────────────────────────────────────┐
│ unmap();                                                        │
│                                                                 │
│ → munmap(m_file_address, m_file_stat.st_size);                  │
│ → 解除映射，释放资源                                             │
│ → m_file_address = 0;                                           │
└─────────────────────────────────────────────────────────────────┘
```

**何时调用 unmap()？**

```cpp
// 1. 响应发送完成后
if (bytes_to_send <= 0)
{
    unmap();
    ...
}

// 2. 发送出错时
if (temp < 0)
{
    if (errno != EAGAIN)
    {
        unmap();
        return false;
    }
}
```

---

### 6.20 响应生成辅助函数详解

这些函数用于构建 HTTP 响应报文。

#### 6.20.1 add_response() - 核心格式化函数

**位置**：http_conn.cpp 第 604 行

```cpp
bool http_conn::add_response(const char *format, ...)
{
    // 检查缓冲区是否已满
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;

    va_list arg_list;                    // 可变参数列表
    va_start(arg_list, format);          // 初始化 arg_list

    // vsnprintf: 格式化输出到缓冲区
    int len = vsnprintf(m_write_buf + m_write_idx,
                        WRITE_BUFFER_SIZE - 1 - m_write_idx,
                        format, arg_list);

    // 检查是否超出缓冲区
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }

    m_write_idx += len;                  // 更新写入位置
    va_end(arg_list);                    // 清理 arg_list

    LOG_INFO("request:%s", m_write_buf); // 记录日志

    return true;
}
```

**可变参数详解：**

```cpp
// 可变参数函数的使用步骤：

// 1. 声明 va_list 变量
va_list arg_list;

// 2. 用 va_start 初始化，指向 format 后的第一个参数
va_start(arg_list, format);

// 3. 用 va_arg 获取参数（或用 vsnprintf 一次性处理）
int num = va_arg(arg_list, int);
char *str = va_arg(arg_list, char*);

// 4. 用 va_end 清理
va_end(arg_list);
```

**使用示例：**

```cpp
// 调用 add_response
add_response("HTTP/1.1 %d %s\r\n", 200, "OK");

// 展开后：
// format = "HTTP/1.1 %d %s\r\n"
// arg_list 包含: 200, "OK"
// vsnprintf 会生成: "HTTP/1.1 200 OK\r\n"
```

#### 6.20.2 add_status_line() - 添加状态行

**位置**：http_conn.cpp 第 623 行

```cpp
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
```

**HTTP 响应状态行格式：**

```
HTTP/1.1 200 OK\r\n
├──────┤ ├┤ ├──┤
  版本   状态码 原因短语

常见状态码：
200 OK                    - 成功
400 Bad Request           - 请求格式错误
403 Forbidden             - 无权限
404 Not Found             - 资源不存在
500 Internal Server Error - 服务器内部错误
```

#### 6.20.3 add_headers() - 添加响应头

**位置**：http_conn.cpp 第 627 行

```cpp
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) &&
           add_linger() &&
           add_blank_line();
}
```

**等价于依次调用：**

```cpp
add_content_length(content_len);  // Content-Length: xxx\r\n
add_linger();                     // Connection: keep-alive/close\r\n
add_blank_line();                 // \r\n（空行，表示头部结束）
```

#### 6.20.4 add_content_length() - 添加 Content-Length

**位置**：http_conn.cpp 第 632 行

```cpp
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
```

#### 6.20.5 add_linger() - 添加 Connection 头

**位置**：http_conn.cpp 第 640 行

```cpp
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n",
                        (m_linger == true) ? "keep-alive" : "close");
}
```

**keep-alive vs close：**

| 值 | 含义 | 连接行为 |
|-----|------|----------|
| `keep-alive` | 保持连接 | 发送完后不关闭，等待下一个请求 |
| `close` | 关闭连接 | 发送完后立即关闭连接 |

#### 6.20.6 add_blank_line() - 添加空行

**位置**：http_conn.cpp 第 644 行

```cpp
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
```

**空行的作用：**

```
HTTP 响应格式：
┌─────────────────────────────────────┐
│ HTTP/1.1 200 OK\r\n                 │ ← 状态行
│ Content-Length: 1234\r\n            │ ← 响应头
│ Connection: keep-alive\r\n          │ ← 响应头
│ \r\n                                │ ← 空行（分隔头部和正文）
│ <html>...</html>                    │ ← 响应正文
└─────────────────────────────────────┘

空行是头部和正文的分界线！
```

#### 6.20.7 add_content() - 添加响应正文

**位置**：http_conn.cpp 第 648 行

```cpp
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
```

**完整响应生成示例：**

```cpp
// 错误响应（如 404）
add_status_line(404, "Not Found");
add_headers(strlen("The requested file was not found"));
add_content("The requested file was not found");

// 生成的响应：
/*
HTTP/1.1 404 Not Found\r\n
Content-Length: 34\r\n
Connection: close\r\n
\r\n
The requested file was not found
*/
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

---

## 十一、全局变量和错误消息

**位置**：http_conn.cpp 第 7-18 行

```cpp
//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;              // 全局锁，用于多线程同步
map<string, string> users;  // 全局用户表，存储用户名和密码
```

**错误消息用途表：**

| 错误码 | title | form（正文） | 触发条件 |
|--------|-------|-------------|----------|
| 200 | OK | （文件内容） | 请求成功 |
| 400 | Bad Request | 请求语法错误 | URL 格式不正确 |
| 403 | Forbidden | 无权限 | 文件不可读 |
| 404 | Not Found | 文件不存在 | stat() 失败 |
| 500 | Internal Error | 服务器内部错误 | switch default |

---

## 十二、静态成员变量

**位置**：http_conn.cpp 第 97-98 行

```cpp
int http_conn::m_user_count = 0;   // 当前用户数
int http_conn::m_epollfd = -1;     // epoll 文件描述符
```

**为什么是静态变量？**

```cpp
// 静态成员变量属于类，不属于某个对象
// 所有 http_conn 对象共享这两个变量

class http_conn {
public:
    static int m_epollfd;      // 所有连接共享同一个 epoll
    static int m_user_count;   // 统计总用户数
private:
    int m_sockfd;              // 每个连接有自己的 socket
    ...
};

// 使用：
http_conn conn1, conn2, conn3;

conn1.m_epollfd == conn2.m_epollfd  // true，共享
conn1.m_sockfd != conn2.m_sockfd    // true，各自独立

conn1.m_user_count++;  // 所有对象看到的都是 +1 后的值
```

---

**文档版本**：v2.0
**最后更新**：2026-03-10
**适用代码**：TinyWebServer http 模块
