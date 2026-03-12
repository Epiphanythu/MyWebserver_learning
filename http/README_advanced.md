# HTTP 模块详解（进阶篇）

> 本文档是 [README_basic.md](./README_basic.md) 的续篇，包含：
> - 关键技术点详解（状态机、C 字符串处理、mmap、writev、非阻塞 I/O）
> - 完整处理流程图
> - 函数总结
> - 核心思想总结
> - epoll 详解（结构体、函数、LT/ET 模式）
> - 全局变量与静态成员
> - HTTP 模块面试题精选

---

## 七、关键技术点详解

### 7.1 状态机设计（核心架构）

#### 7.1.1 为什么需要状态机？

HTTP 请求不是一次性读取的，可能分多次到达。状态机记录"解析到哪一步了"。

```
请求数据可能分多次到达：
第1次收到: "GET /index.html HTTP/1.1\r\n"
第2次收到: "Host: localhost\r\n"
第3次收到: "Connection: keep-alive\r\n\r\n"

状态机的作用：记录当前解析到哪个阶段
```

#### 7.1.2 双层状态机架构

```
┌─────────────────────────────────────────────────────────────────┐
│                      双层状态机架构                               │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                     从状态机 parse_line()                        │
│                                                                 │
│  职责：从缓冲区读出一行（以 \r\n 结尾）                            │
│  输入：原始缓冲区数据                                            │
│  输出：LINE_OK（读完一行） / LINE_OPEN（数据不完整）              │
│                                                                 │
│  技巧：把 \r\n 替换成 \0\0，使每行成为独立的 C 字符串             │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ LINE_OK（有一行了）
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    主状态机 process_read()                       │
│                                                                 │
│  职责：根据当前状态，把行交给对应的解析函数                        │
│                                                                 │
│  ┌──────────────────────┐                                       │
│  │ CHECK_STATE_         │                                       │
│  │ REQUESTLINE          │ ──► parse_request_line()              │
│  │ (解析请求行)          │     "GET / HTTP/1.1"                  │
│  └──────────┬───────────┘                                       │
│             │ 完成后                                             │
│             ▼                                                   │
│  ┌──────────────────────┐                                       │
│  │ CHECK_STATE_         │                                       │
│  │ HEADER               │ ──► parse_headers()                   │
│  │ (解析头部)            │     "Host: localhost"                 │
│  └──────────┬───────────┘     "Connection: keep-alive"          │
│             │ 遇到空行                                          │
│             ▼                                                   │
│  ┌──────────────────────┐                                       │
│  │ CHECK_STATE_         │                                       │
│  │ CONTENT              │ ──► parse_content()                   │
│  │ (解析正文)            │     "user=admin&passwd=123"           │
│  └──────────────────────┘                                       │
└─────────────────────────────────────────────────────────────────┘
```

#### 7.1.3 状态转换完整流程

```
开始解析
    │
    ▼
┌─────────────────────────────────────────────────────────────────┐
│ 状态: CHECK_STATE_REQUESTLINE                                   │
│ 调用: parse_request_line(text)                                  │
│ 输入: "GET /index.html HTTP/1.1"                                │
│                                                                 │
│ 提取: m_method = GET                                            │
│       m_url = "/index.html"                                     │
│       m_version = "HTTP/1.1"                                    │
│                                                                 │
│ 状态切换: m_check_state = CHECK_STATE_HEADER                    │
└─────────────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────────────┐
│ 状态: CHECK_STATE_HEADER                                        │
│ 调用: parse_headers(text)                                       │
│                                                                 │
│ 第1行: "Host: localhost:9006"     → m_host = "localhost:9006"   │
│ 第2行: "Connection: keep-alive"   → m_linger = true             │
│ 第3行: "Content-Length: 25"       → m_content_length = 25       │
│ 第4行: "" (空行)                  → 头部结束！                   │
│                                                                 │
│ 判断: m_content_length > 0 ?                                    │
│   是 → 状态切换到 CHECK_STATE_CONTENT                           │
│   否 → 直接调用 do_request()                                    │
└─────────────────────────────────────────────────────────────────┘
    │
    ▼ (如果有正文)
┌─────────────────────────────────────────────────────────────────┐
│ 状态: CHECK_STATE_CONTENT                                       │
│ 调用: parse_content(text)                                       │
│                                                                 │
│ 检查: m_read_idx >= (m_content_length + m_checked_idx) ?        │
│   是 → 正文读完了，m_string = text                              │
│        返回 GET_REQUEST                                         │
│   否 → 返回 NO_REQUEST，继续读                                  │
└─────────────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────────────┐
│ do_request()                                                    │
│ 处理请求，生成响应                                               │
└─────────────────────────────────────────────────────────────────┘
```

#### 7.1.4 关键位置变量说明

```cpp
// 三个关键位置变量
m_read_idx     // 已读取的总字节数（下次 recv 从这里开始写）
m_checked_idx  // 已分析到的位置（parse_line 扫描到这里）
m_start_line   // 当前行的起始位置（get_line() 用这个）

// 缓冲区示意图：
// m_read_buf:
// ┌─────────────────────────────────────────────────────────────┐
// │ G E T   /   H T T P / 1 . 1 \r \n H o s t : ... \r \n ...   │
// └─────────────────────────────────────────────────────────────┘
// ↑                                   ↑                         ↑
// 0                              m_checked_idx             m_read_idx
// m_start_line (当前行起始)
```

---

### 7.2 C 字符串处理技巧

#### 7.2.1 核心函数一览

| 函数 | 作用 | 示例 |
|------|------|------|
| `strpbrk(s, chars)` | 找到第一个在 chars 中的字符 | `strpbrk("hello", "aeiou")` → 指向 'e' |
| `strspn(s, chars)` | 计算开头有多少字符在 chars 中 | `strspn("   abc", " ")` → 返回 3 |
| `strcasecmp(a, b)` | 忽略大小写比较字符串 | `strcasecmp("GET", "get")` → 返回 0 |
| `strncasecmp(a, b, n)` | 忽略大小写比较前 n 个字符 | `strncasecmp("http://", "HTTP", 4)` → 0 |
| `strchr(s, c)` | 找到字符 c 第一次出现的位置 | `strchr("/index", '/')` → 指向第一个 '/' |
| `strrchr(s, c)` | 从后往前找字符 | `strrchr("/a/b/c", '/')` → 指向最后的 '/' |

#### 7.2.2 strpbrk 详解（分割字符串神器）

```cpp
// 函数原型
char *strpbrk(const char *str, const char *accept);
// 作用：在 str 中找到第一个出现在 accept 中的字符
// 返回：找到则返回指向该字符的指针，没找到返回 NULL

// 示例：解析 "GET /index.html HTTP/1.1"
char text[] = "GET /index.html HTTP/1.1";
char *p = strpbrk(text, " \t");  // 找第一个空格或制表符
// p 指向 "GET" 后面的空格

// 关键技巧：用 \0 截断
*p++ = '\0';
// 这行代码做了两件事：
// 1. *p = '\0' - 把空格改成字符串结束符
// 2. p++ - 指针后移一位

// 结果：
// text = "GET"（因为空格变成了 \0）
// p 指向 "/index.html HTTP/1.1"
```

**内存变化图解：**

```
原始：
┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
│ G │ E │ T │   │ / │ i │ n │ d │ e │ x │ . │...│
└───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
              ↑
              p 指向这里（空格）

执行 *p++ = '\0' 后：
┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
│ G │ E │ T │ \0│ / │ i │ n │ d │ e │ x │ . │...│
└───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
              ↑   ↑
            \0   p 现在指向这里

现在：
- text = "GET"（C 字符串，遇到 \0 结束）
- p = "/index.html HTTP/1.1"
```

#### 7.2.3 strspn 详解（跳过空白字符）

```cpp
// 函数原型
size_t strspn(const char *str, const char *accept);
// 作用：计算 str 开头有多少个字符在 accept 中
// 返回：匹配的字符数量

// 示例
strspn("   hello", " \t");  // 返回 3（开头有3个空格）
strspn("abc123", "abc");    // 返回 3（abc都在"abc"中）
strspn("xyz", "abc");       // 返回 0（没有匹配）

// 使用场景：跳过 URL 前面的空格
// m_url = "   /index.html HTTP/1.1"
m_url += strspn(m_url, " \t");
// m_url 现在指向 "/index.html HTTP/1.1"
```

#### 7.2.4 完整解析流程示例

```cpp
// 输入: "GET /index.html HTTP/1.1"
char text[] = "GET /index.html HTTP/1.1";

// ========== 第1步：提取方法 ==========
m_url = strpbrk(text, " \t");  // 找到第一个空格
*m_url++ = '\0';               // 截断，m_url 后移

char *method = text;           // method = "GET"
if (strcasecmp(method, "GET") == 0)
    m_method = GET;

// ========== 第2步：提取 URL ==========
m_url += strspn(m_url, " \t"); // 跳过空格
// m_url = "/index.html HTTP/1.1"

m_version = strpbrk(m_url, " \t");  // 找下一个空格
*m_version++ = '\0';                // 截断
// m_url = "/index.html"
// m_version = " HTTP/1.1"

// ========== 第3步：提取版本 ==========
m_version += strspn(m_version, " \t"); // 跳过空格
// m_version = "HTTP/1.1"
```

#### 7.2.5 关键技巧总结

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

### 7.3 mmap 内存映射（零拷贝技术）

#### 7.3.1 为什么用 mmap？

```
传统方式读取文件并发送：
┌─────────┐    read()    ┌─────────┐    拷贝    ┌─────────┐    write()   ┌─────────┐
│  文件   │ ──────────► │ 内核缓冲区│ ─────────► │用户缓冲区│ ──────────► │  socket │
└─────────┘             └─────────┘            └─────────┘             └─────────┘
                                                    ↑
                                              需要这一次拷贝！

mmap 方式：
┌─────────┐    mmap()   ┌─────────────┐   writev()  ┌─────────┐
│  文件   │ ──────────► │ 内存映射区域  │ ──────────► │  socket │
└─────────┘   (直接映射)  └─────────────┘             └─────────┘
                              ↑
                         零拷贝！
```

**mmap 优点：**
- 避免用户态和内核态之间的数据拷贝
- 不需要 read/write 系统调用
- 文件内容直接在内存中，可以像数组一样访问

#### 7.3.2 mmap 函数详解

```cpp
void *mmap(
    void *addr,      // 映射起始地址（NULL 表示让系统选择）
    size_t length,   // 映射长度（文件大小）
    int prot,        // 内存保护标志
    int flags,       // 映射类型
    int fd,          // 文件描述符
    off_t offset     // 文件偏移量
);

// 参数详解：
// prot（保护标志）：
//   PROT_READ   - 可读
//   PROT_WRITE  - 可写
//   PROT_EXEC   - 可执行
//   PROT_NONE   - 不可访问

// flags（映射类型）：
//   MAP_SHARED   - 共享映射（修改会写入文件）
//   MAP_PRIVATE  - 私有映射（修改不影响原文件）← 本项目使用
```

#### 7.3.3 本项目中的使用

```cpp
// 把文件映射到内存，直接像操作内存一样操作文件
int fd = open(filename, O_RDONLY);
m_file_address = (char *)mmap(0, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
close(fd);  // 映射后可以关闭文件描述符

// 用完后解除映射
munmap(m_file_address, file_size);
```

#### 7.3.4 mmap 使用流程

```
┌─────────────────────────────────────────────────────────────────┐
│                    mmap 使用流程                                 │
└─────────────────────────────────────────────────────────────────┘

do_request() 中：
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 1. 检查文件                                                      │
│    stat(m_real_file, &m_file_stat)                              │
│    → 获取文件大小、权限等信息                                     │
└─────────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 2. 打开文件                                                      │
│    int fd = open(m_real_file, O_RDONLY);                        │
└─────────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 3. mmap 映射                                                     │
│    m_file_address = (char *)mmap(0, size, PROT_READ,            │
│                                  MAP_PRIVATE, fd, 0);           │
│                                                                 │
│    → 文件内容直接映射到内存                                       │
│    → m_file_address 指向文件内容的起始位置                        │
└─────────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 4. 关闭文件描述符                                                │
│    close(fd);                                                   │
│    → 映射后不再需要 fd                                           │
└─────────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 5. 使用 m_file_address 发送数据                                  │
│    writev() 可以直接发送这块内存                                  │
└─────────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 6. 发送完成后解除映射                                            │
│    munmap(m_file_address, m_file_stat.st_size);                 │
│    m_file_address = 0;                                          │
└─────────────────────────────────────────────────────────────────┘
```

---

### 7.4 writev 分散写（高效发送响应）

#### 7.4.1 为什么用 writev？

```
HTTP 响应由两部分组成：
1. 响应头（在 m_write_buf 中）: "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n"
2. 响应正文（mmap 映射的文件内容）: "<html>...</html>"

传统方式（两次 write）：
write(sockfd, header, header_len);   // 系统调用1
write(sockfd, body, body_len);       // 系统调用2
→ 两次系统调用，效率低

writev 方式（一次调用）：
writev(sockfd, iov, 2);              // 一次系统调用
→ 一次系统调用，效率高
```

#### 7.4.2 iovec 结构体

```cpp
struct iovec {
    void  *iov_base;  // 缓冲区起始地址
    size_t iov_len;   // 缓冲区长度
};

// 函数原型
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
// fd      : 文件描述符（socket）
// iov     : iovec 数组
// iovcnt  : 数组元素个数
```

#### 7.4.3 本项目中的使用

```cpp
// process_write() 中设置 iovec
m_iv[0].iov_base = m_write_buf;       // 缓冲区1：响应头
m_iv[0].iov_len = m_write_idx;        // 响应头长度
m_iv[1].iov_base = m_file_address;    // 缓冲区2：文件内容（mmap）
m_iv[1].iov_len = m_file_stat.st_size; // 文件大小
m_iv_count = 2;                       // 两个缓冲区

// write() 中发送
temp = writev(m_sockfd, m_iv, m_iv_count);
```

#### 7.4.4 部分发送的处理

```cpp
// 发送前：
// m_iv[0]: 响应头（100 字节）
// m_iv[1]: 文件内容（1234 字节）

// 假设 writev 返回 50（只发送了 50 字节）
if (bytes_have_send >= m_iv[0].iov_len) {
    // 响应头发完了，调整文件指针
    m_iv[0].iov_len = 0;
    m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
    m_iv[1].iov_len = bytes_to_send;
} else {
    // 响应头还没发完，调整指针
    m_iv[0].iov_base = m_write_buf + bytes_have_send;
    m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
}
// 下次调用 writev 时，从新位置继续发送
```

**图解：**

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

### 7.5 非阻塞 I/O

#### 7.5.1 设置非阻塞

```cpp
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);      // 获取当前标志
    int new_option = old_option | O_NONBLOCK; // 添加非阻塞标志
    fcntl(fd, F_SETFL, new_option);           // 设置新标志
    return old_option;                         // 返回旧标志（方便恢复）
}
```

#### 7.5.2 阻塞 vs 非阻塞对比

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

#### 7.5.3 为什么 ET 模式必须用非阻塞？

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
4. recv() 返回 -1，errno = EAGAIN（非阻塞，立即返回）
5. 检测到 EAGAIN，跳出循环
6. 完美！
```

**为什么需要非阻塞？**
- 配合 epoll ET 模式使用
- 避免在 read/write 时阻塞整个线程
- EAGAIN 表示"暂时没有数据"，不是错误

---

### 7.6 技术点关系图

```
┌─────────────────────────────────────────────────────────────────┐
│                     技术点关系图                                  │
└─────────────────────────────────────────────────────────────────┘

                        ┌─────────────┐
                        │  LT / ET    │
                        │  触发模式    │
                        └──────┬──────┘
                               │ 决定读取方式
                               ▼
┌─────────────┐         ┌─────────────┐         ┌─────────────┐
│   状态机    │ ◄─────► │  字符串处理  │ ◄─────► │    mmap    │
│  解析请求   │         │  解析请求行  │         │  零拷贝读取 │
└─────────────┘         └─────────────┘         └──────┬──────┘
                                                       │
                                                       ▼
                                                ┌─────────────┐
                                                │   writev   │
                                                │  分散写发送 │
                                                └─────────────┘

数据流向：
接收请求(ET/LT) → 状态机解析(字符串处理) → mmap映射文件 → writev发送
```

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

---

# 十、epoll 详解

## 10.1 epoll 简介

epoll 是 Linux 特有的 I/O 多路复用机制，用于监控多个文件描述符的读写事件。

### 10.1.1 为什么用 epoll？

```
select/poll 的问题：
┌─────────────────────────────────────────────────────────────────┐
│ 1. 每次调用都要把 fd 集合从用户态拷贝到内核态（开销大）            │
│ 2. 每次调用都要遍历所有 fd（O(n) 复杂度）                         │
│ 3. 支持的 fd 数量有限（select 默认 1024）                        │
└─────────────────────────────────────────────────────────────────┘

epoll 的优势：
┌─────────────────────────────────────────────────────────────────┐
│ 1. 只需要在创建时拷贝一次（epoll_create）                         │
│ 2. 只返回就绪的 fd（O(1) 复杂度获取就绪事件）                     │
│ 3. 支持的 fd 数量几乎无限制（受系统内存限制）                      │
│ 4. 支持 LT 和 ET 两种触发模式                                    │
└─────────────────────────────────────────────────────────────────┘
```

---

## 10.2 epoll 核心结构体

### 10.2.1 epoll_event 结构体

```cpp
struct epoll_event {
    uint32_t     events;    // 事件类型（位掩码）
    epoll_data_t data;      // 用户数据
};

typedef union epoll_data {
    void    *ptr;     // 指针（可用于关联自定义数据）
    int      fd;      // 文件描述符
    uint32_t u32;     // 32位整数
    uint64_t u64;     // 64位整数
} epoll_data_t;
```

**使用示例：**

```cpp
struct epoll_event event;
event.data.fd = sockfd;           // 关联 socket
event.events = EPOLLIN | EPOLLET; // 设置事件类型

// 常用方式：使用 data.fd
if (events[i].events & EPOLLIN) {
    int fd = events[i].data.fd;
    // 处理 fd 的读事件
}
```

### 10.2.2 事件类型详解

| 事件常量 | 值 | 说明 | 使用场景 |
|----------|-----|------|----------|
| `EPOLLIN` | 0x001 | 可读事件 | socket 有数据可读 |
| `EPOLLOUT` | 0x004 | 可写事件 | socket 可以发送数据 |
| `EPOLLET` | 0x80000000 | 边缘触发模式 | 高性能服务器 |
| `EPOLLLT` | 0 | 水平触发模式（默认） | 简单场景 |
| `EPOLLRDHUP` | 0x2000 | 对方关闭连接 | 检测客户端断开 |
| `EPOLLPRI` | 0x002 | 紧急数据 | 带外数据 |
| `EPOLLERR` | 0x008 | 错误事件 | 无需设置，自动监听 |
| `EPOLLHUP` | 0x010 | 挂起事件 | 无需设置，自动监听 |
| `EPOLLONESHOT` | 0x40000000 | 一次性事件 | 多线程防止竞争 |

**事件组合使用：**

```cpp
// LT 模式，监听读事件和连接断开
event.events = EPOLLIN | EPOLLRDHUP;

// ET 模式，监听读写事件
event.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;

// ET 模式 + ONESHOT（多线程安全）
event.events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
```

---

## 10.3 epoll 核心函数

### 10.3.1 epoll_create() - 创建 epoll 实例

```cpp
#include <sys/epoll.h>

int epoll_create(int size);
int epoll_create1(int flags);
```

**参数说明：**
- `size`：告诉内核预期监听的 fd 数量（仅作参考，Linux 2.6.8 后忽略）
- `flags`：`0` 或 `EPOLL_CLOEXEC`（exec 时自动关闭）

**返回值：**
- 成功：返回 epoll 实例的文件描述符
- 失败：返回 -1，设置 errno

**使用示例：**

```cpp
// 创建 epoll 实例
int epollfd = epoll_create(5);  // 5 只是个提示值
if (epollfd == -1) {
    perror("epoll_create failed");
    exit(1);
}

// 使用完后关闭
close(epollfd);
```

### 10.3.2 epoll_ctl() - 控制 epoll 实例

```cpp
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
```

**参数说明：**
- `epfd`：epoll 实例的文件描述符
- `op`：操作类型
- `fd`：要操作的文件描述符
- `event`：事件结构体

**操作类型（op）：**

| 操作 | 宏 | 说明 | 示例 |
|------|-----|------|------|
| 添加 | `EPOLL_CTL_ADD` | 注册新的 fd | 添加新连接到 epoll |
| 修改 | `EPOLL_CTL_MOD` | 修改已注册 fd 的事件 | 从读事件改为写事件 |
| 删除 | `EPOLL_CTL_DEL` | 删除已注册的 fd | 连接关闭时删除 |

**使用示例：**

```cpp
// ========== 添加事件 ==========
struct epoll_event event;
event.data.fd = sockfd;
event.events = EPOLLIN | EPOLLET;
if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &event) == -1) {
    perror("epoll_ctl add failed");
}

// ========== 修改事件 ==========
event.events = EPOLLOUT | EPOLLET;
if (epoll_ctl(epollfd, EPOLL_CTL_MOD, sockfd, &event) == -1) {
    perror("epoll_ctl mod failed");
}

// ========== 删除事件 ==========
if (epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, NULL) == -1) {
    perror("epoll_ctl del failed");
}
```

### 10.3.3 epoll_wait() - 等待事件

```cpp
int epoll_wait(int epfd, struct epoll_event *events,
               int maxevents, int timeout);
```

**参数说明：**
- `epfd`：epoll 实例的文件描述符
- `events`：输出参数，用于存放返回的事件数组
- `maxevents`：最多返回多少个事件（必须 > 0）
- `timeout`：超时时间（毫秒）
  - `-1`：永久阻塞
  - `0`：立即返回（非阻塞）
  - `> 0`：等待指定毫秒数

**返回值：**
- 成功：返回就绪的 fd 数量
- 超时：返回 0
- 失败：返回 -1，设置 errno

**使用示例：**

```cpp
#define MAX_EVENTS 1024

struct epoll_event events[MAX_EVENTS];

// 等待事件，超时 1000ms
int nfds = epoll_wait(epollfd, events, MAX_EVENTS, 1000);
if (nfds == -1) {
    perror("epoll_wait failed");
    // 如果是信号中断，继续
    if (errno == EINTR) continue;
    break;
}

// 遍历就绪的事件
for (int i = 0; i < nfds; i++) {
    int fd = events[i].data.fd;

    // 处理不同类型的事件
    if (events[i].events & EPOLLIN) {
        // 可读事件
        handle_read(fd);
    }
    if (events[i].events & EPOLLOUT) {
        // 可写事件
        handle_write(fd);
    }
    if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        // 连接断开或错误
        close_connection(fd);
    }
}
```

---

## 10.4 epoll 编程完整流程

```
┌─────────────────────────────────────────────────────────────────┐
│                    epoll 编程完整流程                             │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ 1. 创建 epoll 实例                                               │
│    int epollfd = epoll_create(1);                               │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ 2. 创建监听 socket                                               │
│    int listenfd = socket(AF_INET, SOCK_STREAM, 0);              │
│    bind(listenfd, ...);                                         │
│    listen(listenfd, 5);                                         │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ 3. 将 listenfd 添加到 epoll                                      │
│    struct epoll_event ev;                                       │
│    ev.events = EPOLLIN;                                         │
│    ev.data.fd = listenfd;                                       │
│    epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &ev);            │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ 4. 事件循环                                                      │
│    while (1) {                                                  │
│        int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);  │
│        for (int i = 0; i < nfds; i++) {                         │
│            if (events[i].data.fd == listenfd) {                 │
│                // 新连接到来                                     │
│                accept_new_connection();                         │
│            } else {                                             │
│                // 处理客户端请求                                 │
│                handle_client_request();                         │
│            }                                                    │
│        }                                                        │
│    }                                                            │
└─────────────────────────────────────────────────────────────────┘
```

### 10.4.1 完整代码示例

```cpp
#include <sys/epoll.h>      // epoll 相关函数：epoll_create, epoll_ctl, epoll_wait
#include <sys/socket.h>     // socket 相关函数：socket, bind, listen, accept, recv
#include <netinet/in.h>     // sockaddr_in 结构体
#include <fcntl.h>          // fcntl 函数，用于设置非阻塞
#include <unistd.h>         // close 函数
#include <stdio.h>          // printf, perror
#include <stdlib.h>         // exit
#include <errno.h>          // errno 变量

#define MAX_EVENTS 1024     // epoll_wait 一次最多返回的事件数
#define PORT 8888           // 监听端口

/**
 * 设置文件描述符为非阻塞模式
 * @param fd 要设置的文件描述符
 * @return 成功返回 0，失败返回 -1
 *
 * 非阻塞模式的重要性：
 * - 在 ET 模式下必须使用非阻塞，否则 recv 可能会一直阻塞
 * - 配合 epoll 实现高并发的关键
 */
int setnonblocking(int fd) {
    // F_GETFL: 获取当前文件状态标志
    int flags = fcntl(fd, F_GETFL, 0);
    // F_SETFL: 设置文件状态标志，添加 O_NONBLOCK（非阻塞）
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * 将文件描述符添加到 epoll 实例中
 * @param epollfd epoll 实例的文件描述符
 * @param fd      要添加的文件描述符
 * @param et_mode 是否使用 ET（边缘触发）模式
 */
void addfd(int epollfd, int fd, bool et_mode) {
    struct epoll_event event;
    event.data.fd = fd;                          // 保存 fd，事件触发时可以获取
    event.events = EPOLLIN | EPOLLRDHUP;         // 监听可读事件 + 连接断开事件

    if (et_mode) {
        event.events |= EPOLLET;                 // 添加 ET（边缘触发）标志
    }

    // EPOLL_CTL_ADD: 将 fd 添加到 epoll 实例中
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);                          // 设置为非阻塞（ET 模式必须）
}

int main() {
    // ===== 1. 创建监听 socket =====
    // AF_INET: IPv4
    // SOCK_STREAM: TCP 流式套接字
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;                   // IPv4
    addr.sin_addr.s_addr = INADDR_ANY;           // 监听所有网卡（0.0.0.0）
    addr.sin_port = htons(PORT);                 // 端口号，htons 转为网络字节序

    bind(listenfd, (struct sockaddr*)&addr, sizeof(addr));  // 绑定地址和端口
    listen(listenfd, 5);                         // 开始监听，backlog=5（等待队列长度）

    // ===== 2. 创建 epoll 实例 =====
    // 参数在 Linux 2.6.8 后被忽略，只需大于 0 即可
    int epollfd = epoll_create(1);

    // ===== 3. 将 listenfd 添加到 epoll =====
    addfd(epollfd, listenfd, true);              // true 表示使用 ET 模式

    // ===== 4. 事件循环 =====
    struct epoll_event events[MAX_EVENTS];       // 用于存放返回的事件

    while (1) {
        // 阻塞等待事件发生
        // -1 表示无限等待，直到有事件发生
        // 返回值 nfds 是就绪的 fd 数量
        int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);

        // 错误处理（EINTR 是被信号中断，不算真正的错误）
        if (nfds < 0 && errno != EINTR) {
            perror("epoll_wait");
            break;
        }

        // 遍历所有就绪的事件
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;          // 获取触发事件的 fd

            // ===== 情况1：新连接到来 =====
            // listenfd 可读说明有新的客户端连接
            if (fd == listenfd) {
                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);

                // 接受新连接，返回新的 socket fd
                int connfd = accept(listenfd, (struct sockaddr*)&client_addr, &len);

                // 将新连接也加入 epoll 监听
                addfd(epollfd, connfd, true);
                printf("new connection: %d\n", connfd);
            }
            // ===== 情况2：客户端发来数据 =====
            else if (events[i].events & EPOLLIN) {
                char buf[1024];

                // ★ ET 模式必须循环读取，直到 EAGAIN ★
                // 因为 ET 模式只在状态变化时通知一次
                while (1) {
                    int n = recv(fd, buf, sizeof(buf), 0);

                    if (n == -1) {
                        // EAGAIN/EWOULDBLOCK：没有更多数据可读
                        // 这是非阻塞 IO 的正常情况，表示数据读完了
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;  // 退出循环，等待下一次 EPOLLIN 事件
                        }
                        // 其他错误，关闭连接
                        close(fd);
                        break;
                    }
                    else if (n == 0) {
                        // recv 返回 0 表示对方关闭了连接
                        close(fd);
                        break;
                    }

                    // 处理接收到的数据...
                    printf("recv %d bytes from %d\n", n, fd);
                }
            }

            // ===== 情况3：连接断开或出错 =====
            // EPOLLRDHUP: 对方关闭写端（半关闭）或完全关闭
            // EPOLLHUP: 挂断
            // EPOLLERR: 错误
            if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                close(fd);
                printf("connection closed: %d\n", fd);
            }
        }
    }

    // 清理资源
    close(listenfd);
    close(epollfd);
    return 0;
}
```

---

## 10.5 ET/LT 模式详解

### 10.5.1 LT 模式（Level-Triggered，水平触发，默认模式）

#### 行为特点：
- 只要 socket 的接收缓冲区**还有数据未读完**，每次调用 `epoll_wait()` 都会返回该 fd。
- 即使你只读了一部分数据，下次 `epoll_wait()` 仍会通知你"可读"。

#### 图解：

```
LT 模式工作流程：
┌─────────────────────────────────────────────────────────────────┐
│                    Socket 接收缓冲区                              │
│ ┌─────────────────────────────────────────────────────────────┐ │
│ │ [数据1][数据2][数据3]                                         │ │
│ └─────────────────────────────────────────────────────────────┘ │
│                                                                 │
│ 第1次 epoll_wait() → 返回可读                                    │
│ recv() → 读数据1（缓冲区还有数据2、3）                            │
│                                                                 │
│ 第2次 epoll_wait() → 还会返回可读！（因为还有数据）               │
│ recv() → 读数据2                                                │
│                                                                 │
│ 第3次 epoll_wait() → 还会返回可读！                              │
│ recv() → 读数据3                                                │
│                                                                 │
│ 第4次 epoll_wait() → 不返回了（缓冲区空了）                      │
└─────────────────────────────────────────────────────────────────┘
```

#### 类比：
> 像门铃：只要有人在门口（缓冲区有数据），门铃就会一直响，直到你把人请进来（读完数据）。

#### 优点：
- 编程简单，容错性强
- 适合初学者
- 不需要一次性读完数据

#### 缺点：
- 可能重复通知，效率略低（但通常可接受）

#### TinyWebServer 中的使用：
```cpp
// LT 模式（默认）
event.events = EPOLLIN | EPOLLRDHUP;

// read_once() 中 LT 模式只需读一次
if (0 == m_TRIGMode) {  // LT 模式
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                      READ_BUFFER_SIZE - m_read_idx, 0);
    m_read_idx += bytes_read;
    return bytes_read > 0;
}
```

---

### 10.5.2 ET 模式（Edge-Triggered，边缘触发）

#### 行为特点：
- **仅在状态变化时通知一次**。
  例如：socket 接收缓冲区**从空 → 非空**时，`epoll_wait()` 返回一次。
- 如果你没有一次性读完所有数据，**即使缓冲区还有数据，也不会再次通知！**

#### 图解：

```
ET 模式工作流程：
┌─────────────────────────────────────────────────────────────────┐
│                    Socket 接收缓冲区                              │
│ ┌─────────────────────────────────────────────────────────────┐ │
│ │ [数据1][数据2][数据3]                                         │ │
│ └─────────────────────────────────────────────────────────────┘ │
│                                                                 │
│ epoll_wait() → 只返回一次！（状态从空→非空）                     │
│                                                                 │
│ 必须用 while 循环一次性读完：                                    │
│   while(1) {                                                    │
│     recv() → 读数据1                                             │
│     recv() → 读数据2                                             │
│     recv() → 读数据3                                             │
│     recv() → 返回 -1，errno = EAGAIN（没数据了，退出）           │
│   }                                                             │
│                                                                 │
│ 如果不用循环读，数据2和3就丢失了！                                │
└─────────────────────────────────────────────────────────────────┘
```

#### 类比：
> 像拍肩膀：只拍你一下，提醒"有新数据来了"。如果你没理，就不会再拍。

#### 优点：
- 减少 `epoll_wait()` 的调用次数，**性能更高**
- 适合高并发场景（如 Nginx 默认使用 ET）

#### 缺点：
- **必须一次性读完所有数据**，否则会丢数据！
- 必须配合 **非阻塞 socket（O_NONBLOCK）** 使用
- 编程复杂度更高

#### TinyWebServer 中的使用：
```cpp
// ET 模式
event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
setnonblocking(fd); // 必须设为非阻塞！

// read_once() 中 ET 模式必须循环读完
else {  // ET 模式
    while (true) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                          READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;  // 数据读完了，正常退出
            return false;  // 真正的错误
        }
        else if (bytes_read == 0) {
            return false;  // 对方关闭连接
        }
        m_read_idx += bytes_read;
    }
    return true;
}
```

---

### 10.5.3 为什么 ET 必须搭配非阻塞？

```
ET 模式 + 阻塞 socket = 死锁！

┌─────────────────────────────────────────────────────────────────┐
│ 场景分析：                                                        │
│                                                                 │
│ 1. epoll 通知可读（ET 只通知一次）                                │
│ 2. 循环读取数据...                                                │
│ 3. 数据读完，调用 recv()                                         │
│ 4. recv() 阻塞等待（因为 socket 是阻塞的）                        │
│ 5. epoll 不会再通知（ET 只通知一次）                              │
│ 6. 线程永远卡在 recv()！                                          │
│                                                                 │
│ 结果：整个线程被阻塞，无法处理其他连接                             │
└─────────────────────────────────────────────────────────────────┘

ET 模式 + 非阻塞 socket = 正确！

┌─────────────────────────────────────────────────────────────────┐
│ 场景分析：                                                        │
│                                                                 │
│ 1. epoll 通知可读                                                │
│ 2. 循环读取数据...                                                │
│ 3. 数据读完，调用 recv()                                         │
│ 4. recv() 返回 -1，errno = EAGAIN（非阻塞，立即返回）             │
│ 5. 检测到 EAGAIN，跳出循环                                       │
│ 6. 完美！线程可以继续处理其他事情                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

### 10.5.4 对比总结

| 特性 | LT（水平触发） | ET（边缘触发） |
|------|----------------|----------------|
| 通知频率 | 只要有数据就通知 | 仅状态变化时通知一次 |
| 是否需循环读 | 否（可分多次读） | **是（必须一次读完）** |
| 是否需非阻塞 | 否（可用阻塞） | **是（必须非阻塞）** |
| 编程难度 | 简单 | 较难 |
| 性能 | 略低 | 更高 |
| 典型应用 | 初学者项目、简单服务器 | Nginx、Redis、高性能服务 |

---

## 10.6 在 TinyWebServer 中的使用

### 10.6.1 模式选择

启动时通过 `-m` 参数控制：
```bash
./server -m 0   # LT + LT（默认）
./server -m 1   # LT + ET
./server -m 2   # ET + LT
./server -m 3   # ET + ET（高性能）
```

### 10.6.2 相关函数

```cpp
// 添加 fd 到 epoll
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;  // ET
    else
        event.events = EPOLLIN | EPOLLRDHUP;            // LT

    if (one_shot)
        event.events |= EPOLLONESHOT;

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 修改 epoll 事件
void modfd(int epollfd, int fd, int ev, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 从 epoll 删除 fd
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}
```

---

## 10.7 EPOLLONESHOT 详解

### 10.7.1 为什么需要 EPOLLONESHOT？

```
没有 EPOLLONESHOT 的问题：
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

### 10.7.2 使用方式

```cpp
// 添加时设置 EPOLLONESHOT
event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

// 处理完后必须重置事件（否则下次不会通知）
void reset_oneshot(int epollfd, int fd) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}
```

---

## 10.8 最佳实践

### 10.8.1 建议

- **初学者先用 LT 模式**：逻辑简单，不易出错
- **理解后再尝试 ET 模式**：体会高性能服务器的设计精髓
- **永远记住**：ET + 非阻塞 + 循环读 = 正确姿势
- **多线程环境**：使用 EPOLLONESHOT 防止竞争

### 10.8.2 面试常问

> **Q1: epoll 和 select/poll 的区别？**
> - epoll 只返回就绪的 fd，select/poll 需要遍历
> - epoll 支持 ET 模式，效率更高
> - epoll 没有 fd 数量限制

> **Q2: 为什么 ET 模式要搭配非阻塞 socket？**
> 答：因为 `recv` 在无数据时会阻塞，而 ET 不会再次通知，导致线程卡死。

> **Q3: EPOLLONESHOT 的作用？**
> 答：确保同一时刻只有一个线程处理一个 socket，避免多线程竞争。

> **Q4: LT 和 ET 的区别？**
> 答：LT 只要有数据就持续通知，ET 只在状态变化时通知一次。ET 效率更高但编程更复杂。

---

## 十一、全局变量与静态成员

### 11.1 全局变量和错误消息


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

### 11.2 静态成员变量


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

### 11.3 静态变量 vs 全局变量 vs 成员变量

```
┌─────────────────────────────────────────────────────────────────┐
│                    变量类型对比                                  │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ 全局变量（如 users, m_lock）                                     │
│                                                                 │
│ 特点：                                                          │
│ - 在所有文件中可见（extern）                                     │
│ - 程序启动时创建，程序结束时销毁                                  │
│ - 用于跨文件/跨类共享数据                                        │
│                                                                 │
│ 本项目使用：                                                     │
│ - users: 存储所有用户信息，供登录验证使用                         │
│ - m_lock: 多线程同步锁，保护共享资源                              │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ 静态成员变量（如 m_epollfd, m_user_count）                       │
│                                                                 │
│ 特点：                                                          │
│ - 属于类，不属于某个对象                                         │
│ - 所有对象共享同一份                                             │
│ - 可以通过 类名::变量名 访问                                     │
│                                                                 │
│ 本项目使用：                                                     │
│ - m_epollfd: 所有连接共享同一个 epoll 实例                       │
│ - m_user_count: 统计当前连接总数                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ 普通成员变量（如 m_sockfd, m_read_buf）                          │
│                                                                 │
│ 特点：                                                          │
│ - 属于每个对象，各自独立                                         │
│ - 对象创建时分配，对象销毁时释放                                  │
│                                                                 │
│ 本项目使用：                                                     │
│ - m_sockfd: 每个连接有自己的 socket                              │
│ - m_read_buf: 每个连接有自己的读缓冲区                           │
└─────────────────────────────────────────────────────────────────┘
```

---

## 十二、HTTP 模块面试题精选

### 12.1 HTTP 协议相关

#### Q1: HTTP 请求报文的组成？

```
HTTP 请求报文 = 请求行 + 请求头部 + 空行 + 请求正文

示例：
POST /2log.html HTTP/1.1          ← 请求行（方法 URL 版本）
Host: 192.168.1.1:9006            ← 请求头部
Connection: keep-alive
Content-Length: 25
                                  ← 空行（分隔头部和正文）
user=admin&passwd=123456          ← 请求正文（POST 才有）
```

#### Q2: GET 和 POST 的区别？

| 特性 | GET | POST |
|------|-----|------|
| 参数位置 | URL 中 | 请求正文中 |
| 参数长度 | 有限制（URL 长度限制） | 无限制 |
| 安全性 | 较低（参数可见） | 较高（参数在正文中） |
| 缓存 | 可被缓存 | 不可缓存 |
| 幂等性 | 幂等 | 非幂等 |
| 使用场景 | 获取数据 | 提交数据 |

#### Q3: HTTP 常见状态码？

| 状态码 | 类别 | 常见状态码 |
|--------|------|-----------|
| 1xx | 信息性 | 100 Continue |
| 2xx | 成功 | 200 OK, 204 No Content |
| 3xx | 重定向 | 301 永久重定向, 302 临时重定向 |
| 4xx | 客户端错误 | 400 Bad Request, 403 Forbidden, 404 Not Found |
| 5xx | 服务器错误 | 500 Internal Error, 502 Bad Gateway |

#### Q4: keep-alive 是什么？

```
HTTP/1.1 默认开启 keep-alive（持久连接）

作用：
- 一次 TCP 连接可以发送多个 HTTP 请求
- 减少建立/断开连接的开销
- 提高页面加载速度

实现：
Connection: keep-alive    ← 请求/响应头部
Keep-Alive: timeout=5, max=100  ← 超时时间和最大请求数

本项目中：
- m_linger 变量记录是否保持连接
- 发送完响应后判断 m_linger
  - true: 调用 init() 重置状态，等待下一个请求
  - false: 关闭连接
```

---

### 12.2 技术原理相关

#### Q5: 简述 HTTP 请求的处理流程？

```
1. epoll 监听到读事件（EPOLLIN）
       ↓
2. read_once() 从 socket 读取数据到 m_read_buf
       ↓
3. process() 入口函数
       ↓
4. process_read() 主状态机解析请求
   - parse_line(): 从状态机读一行
   - parse_request_line(): 解析请求行
   - parse_headers(): 解析头部
   - parse_content(): 解析正文
       ↓
5. do_request() 处理请求
   - URL 路由
   - 登录/注册验证
   - mmap 映射文件
       ↓
6. process_write() 生成响应到 m_write_buf
       ↓
7. 注册 EPOLLOUT 事件
       ↓
8. write() 发送响应（writev 分散写）
       ↓
9. 如果 keep-alive，重置状态等待下一个请求
   否则关闭连接
```

#### Q6: 什么是状态机？为什么要用状态机？

```
状态机是一种设计模式，用于管理对象在不同状态之间的转换。

HTTP 解析使用状态机的原因：
1. 数据可能分多次到达（TCP 分片）
2. 需要记录"解析到哪一步了"
3. 不同状态下处理逻辑不同

本项目的双层状态机：
┌─────────────────────────────────────────────────────────────────┐
│ 从状态机 parse_line()                                           │
│ - 职责：从缓冲区读出一行                                         │
│ - 状态：LINE_OK / LINE_OPEN / LINE_BAD                         │
└─────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│ 主状态机 process_read()                                          │
│ - 职责：根据当前状态调用不同解析函数                              │
│ - 状态转换：                                                     │
│   CHECK_STATE_REQUESTLINE → CHECK_STATE_HEADER                  │
│   CHECK_STATE_HEADER → CHECK_STATE_CONTENT 或 do_request        │
│   CHECK_STATE_CONTENT → do_request                              │
└─────────────────────────────────────────────────────────────────┘
```

#### Q7: mmap 是什么？为什么用 mmap？

```
mmap（内存映射）将文件映射到进程的虚拟地址空间，可以像操作内存一样操作文件。

传统方式读取文件并发送：
文件 ──read()──► 内核缓冲区 ──拷贝──► 用户缓冲区 ──write()──► socket
                                      ↑
                                 需要这次拷贝

mmap 方式：
文件 ──mmap──► 内存映射区域 ──writev──► socket
                   ↑
              零拷贝！

优点：
1. 减少数据拷贝（零拷贝）
2. 减少系统调用
3. 文件内容直接在内存中，访问方便

本项目使用：
m_file_address = (char *)mmap(0, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
// 之后 writev 可以直接发送 m_file_address 指向的内容
```

#### Q8: writev 是什么？为什么用 writev？

```
writev（分散写）可以在一次系统调用中写入多个不连续的缓冲区。

HTTP 响应由两部分组成：
1. 响应头（在 m_write_buf 中）
2. 响应正文（mmap 映射的文件内容）

传统方式（两次 write）：
write(sockfd, header, header_len);   // 系统调用1
write(sockfd, body, body_len);       // 系统调用2
→ 两次系统调用，效率低

writev 方式：
struct iovec iov[2];
iov[0].iov_base = header;
iov[0].iov_len = header_len;
iov[1].iov_base = body;
iov[1].iov_len = body_len;
writev(sockfd, iov, 2);              // 一次系统调用
→ 一次系统调用，效率高

优点：
1. 减少系统调用次数
2. 适合发送"头部 + 正文"这种结构
3. 与 mmap 配合实现高效文件传输
```

---

### 12.3 epoll 相关

#### Q9: epoll 和 select/poll 的区别？

| 特性 | select | poll | epoll |
|------|--------|------|-------|
| fd 数量限制 | 1024（默认） | 无限制 | 无限制 |
| 获取就绪 fd | 遍历 O(n) | 遍历 O(n) | 直接返回 O(1) |
| 每次调用 | 需要拷贝 fd 集合 | 需要拷贝 fd 集合 | 只需创建时拷贝一次 |
| 触发模式 | 只有 LT | 只有 LT | 支持 LT 和 ET |
| 跨平台 | 是 | 是 | 仅 Linux |

```
select/poll 每次调用流程：
用户态 fd 集合 ──拷贝──► 内核态 ──遍历所有 fd──► 返回就绪 fd

epoll 流程：
创建时：用户态 fd ──拷贝──► 内核态（只一次）
等待时：内核直接返回就绪 fd（不需要遍历）
```

#### Q10: LT 和 ET 模式的区别？

```
LT（水平触发）：
- 只要缓冲区有数据，每次 epoll_wait 都会通知
- 可以分多次读取数据
- 编程简单，容错性强

ET（边缘触发）：
- 只在状态变化时通知一次（空 → 非空）
- 必须一次性读完所有数据
- 必须使用非阻塞 socket
- 编程复杂，但效率更高

类比：
- LT 像门铃：有人就一直响
- ET 像拍肩膀：只提醒一下

选择建议：
- 初学者：使用 LT 模式
- 高性能服务器：使用 ET 模式（如 Nginx）
```

#### Q11: 为什么 ET 模式必须使用非阻塞 socket？

```
ET 模式 + 阻塞 socket = 死锁！

场景：
1. epoll_wait 通知可读（ET 只通知一次）
2. while 循环读取数据...
3. 数据读完，继续调用 recv()
4. recv() 阻塞等待（因为 socket 是阻塞的）
5. epoll 不会再通知（ET 只通知一次）
6. 线程永远卡在 recv()！

ET 模式 + 非阻塞 socket = 正确！

场景：
1. epoll_wait 通知可读
2. while 循环读取数据...
3. 数据读完，调用 recv()
4. recv() 返回 -1，errno = EAGAIN（非阻塞，立即返回）
5. 检测到 EAGAIN，跳出循环
6. 完美！
```

#### Q12: EPOLLONESHOT 是什么？有什么用？

```
EPOLLONESHOT 确保一个 socket 在任意时刻只被一个线程处理。

没有 EPOLLONESHOT 的问题：
线程1 正在处理 socket A 的请求
此时 socket A 又来了新数据
epoll 再次通知 socket A 可读
线程2 也开始处理 socket A
→ 两个线程同时处理一个连接！数据竞争！

使用 EPOLLONESHOT：
线程1 正在处理 socket A
即使 socket A 又来了新数据
epoll 也不会再次通知
线程1 处理完后，调用 modfd() 重置事件
epoll 才会再次通知
→ 同一时刻只有一个线程处理一个连接

适用场景：
- 多线程 epoll 服务器
- 需要保证请求处理的原子性
```

---

### 12.4 架构设计相关

#### Q13: 这个 HTTP 模块采用了哪些优化技术？

```
1. I/O 多路复用
   - 使用 epoll 监听多个连接
   - 支持 LT 和 ET 两种触发模式

2. 零拷贝技术
   - mmap：文件直接映射到内存
   - writev：一次系统调用发送多个缓冲区

3. 非阻塞 I/O
   - 所有 socket 设为非阻塞
   - 避免 I/O 操作阻塞整个线程

4. 状态机模式
   - 主状态机 + 从状态机
   - 分步解析 HTTP 请求

5. 内存池（项目中其他模块）
   - 减少频繁的内存分配/释放

6. 线程池（项目中其他模块）
   - 复用线程，减少创建/销毁开销

7. 数据库连接池（项目中其他模块）
   - 复用数据库连接
```

#### Q14: 如何处理大量并发连接？

```
1. epoll I/O 多路复用
   - 单线程可以监控大量连接
   - 只返回就绪的 fd，不需要遍历

2. 非阻塞 I/O
   - 不会因为单个连接阻塞整个线程

3. 线程池
   - 将耗时的处理任务交给工作线程
   - 主线程只负责 I/O

4. Reactor 模式
   - 主线程负责监听事件
   - 工作线程负责处理业务逻辑

5. ET 模式（可选）
   - 减少 epoll_wait 调用次数
   - 提高吞吐量

6. 连接限制
   - 限制最大连接数
   - 超出限制时拒绝新连接
```

#### Q15: 用户登录验证是如何实现的？

```
流程：
1. 用户提交登录表单（POST 请求）
   POST /2log.html
   Body: user=admin&passwd=123456

2. parse_content() 解析正文
   m_string = "user=admin&passwd=123456"

3. do_request() 处理登录
   - 提取用户名和密码
   - 查询 users map（内存中）
   - 验证：users[name] == password

4. 根据验证结果返回不同页面
   成功 → /welcome.html
   失败 → /logError.html

优化：
- 启动时从数据库加载所有用户到内存 map
- 登录验证时直接查 map（O(1) 时间复杂度）
- 避免每次登录都查询数据库
- 注册时同时更新数据库和内存 map
```

---

### 12.5 代码细节相关

#### Q16: parse_line() 函数的作用？

```cpp
// 作用：从缓冲区读出一行（以 \r\n 结尾）
// 技巧：把 \r\n 替换成 \0\0，使每行成为独立的 C 字符串

http_conn::LINE_STATUS http_conn::parse_line() {
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        char temp = m_read_buf[m_checked_idx];

        if (temp == '\r') {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;  // 数据不完整
            else if (m_read_buf[m_checked_idx + 1] == '\n') {
                // 把 \r\n 替换成 \0\0
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;  // 读到完整一行
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}
```

#### Q17: strpbrk 和 strspn 的作用？

```cpp
// strpbrk: 找到第一个在指定字符集中的字符
char *strpbrk(const char *str, const char *accept);

// 示例：解析 "GET /index.html HTTP/1.1"
char text[] = "GET /index.html HTTP/1.1";
char *p = strpbrk(text, " \t");  // 找第一个空格
*p++ = '\0';  // 截断，p 指向后半部分
// 现在 text = "GET", p = "/index.html HTTP/1.1"

// strspn: 计算开头有多少字符在指定字符集中
size_t strspn(const char *str, const char *accept);

// 示例：跳过空格
strspn("   /index.html", " \t");  // 返回 3
// 用于跳过 URL 前面的空白字符
```

#### Q18: 如何处理部分发送的情况？

```cpp
// writev 可能只发送部分数据
// 需要调整 iovec 指针，下次继续发送

if (bytes_have_send >= m_iv[0].iov_len) {
    // 响应头发完了，调整文件指针
    m_iv[0].iov_len = 0;
    m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
    m_iv[1].iov_len = bytes_to_send;
} else {
    // 响应头还没发完，调整指针
    m_iv[0].iov_base = m_write_buf + bytes_have_send;
    m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
}

// 注册 EPOLLOUT，等待下次可写时继续发送
modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
```

---

### 12.6 面试题总结表

| 类别 | 问题 | 核心知识点 |
|------|------|-----------|
| HTTP 协议 | 请求报文组成 | 请求行、头部、正文 |
| HTTP 协议 | GET vs POST | 参数位置、安全性、幂等性 |
| HTTP 协议 | 状态码 | 2xx/3xx/4xx/5xx 含义 |
| HTTP 协议 | keep-alive | 持久连接、m_linger |
| 技术原理 | 处理流程 | 9步完整流程 |
| 技术原理 | 状态机 | 双层状态机、状态转换 |
| 技术原理 | mmap | 零拷贝、内存映射 |
| 技术原理 | writev | 分散写、减少系统调用 |
| epoll | vs select/poll | 效率、fd 限制、触发模式 |
| epoll | LT vs ET | 通知方式、循环读 |
| epoll | ET + 非阻塞 | 避免死锁 |
| epoll | EPOLLONESHOT | 线程安全 |
| 架构 | 优化技术 | 7 大优化点 |
| 架构 | 高并发 | epoll + 线程池 + 非阻塞 |
| 架构 | 登录验证 | 内存 map + 数据库 |

---


