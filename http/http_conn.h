#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:

    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    // 请求方法
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    // 主状态机状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,    // 解析请求行
        CHECK_STATE_HEADER, // 解析请求头
        CHECK_STATE_CONTENT // 解析请求体
    };
    // 服务器处理HTTP请求的可能结果
    enum HTTP_CODE
    {
        NO_REQUEST, // 请求不完整，需要继续读取客户数据
        GET_REQUEST, // 获得了一个完整的客户请求
        BAD_REQUEST, // 客户请求语法错误
        NO_RESOURCE, // 没有资源
        FORBIDDEN_REQUEST, // 客户对资源没有足够的访问权限
        FILE_REQUEST, // 文件请求,获取文件成功
        INTERNAL_ERROR, // 服务器内部错误
        CLOSED_CONNECTION // 客户端已经关闭连接了
    };
    // 从状态机状态（行的读取状态）
    enum LINE_STATUS
    {
        LINE_OK = 0, // 读取到一个完整的行
        LINE_BAD, // 行出错
        LINE_OPEN // 行数据尚且不完整
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);
    void process(); // 主入口：解析 + 处理 + 生成响应
    bool read_once();   // 请求读取数据
    bool write();   // 发送响应数据
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);
    int timer_flag;
    int improv;


private:
    // 解析相关
    void init();

    HTTP_CODE process_read();   // 主状态机
    bool process_write(HTTP_CODE ret); // 生成响应
    HTTP_CODE parse_request_line(char *text); // 解析请求行，获得请求方法、目标URL，以及HTTP版本号
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();  // 处理请求
    char *get_line() { return m_read_buf + m_start_line; };
    
    LINE_STATUS parse_line();   // 从状态机，用于解析出一行内容，分析客户请求行、请求头、请求体的结束标志
    void unmap();
    // 响应相关
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_sockfd;
    sockaddr_in m_address;
    // 读缓冲区
    char m_read_buf[READ_BUFFER_SIZE];
    long m_read_idx; // 当前已读取位置，m_read_buf中数据的最后一个字节的下一个位置
    long m_checked_idx; // 当前分析位置
    int m_start_line;   // 当前行的起始位置
    // 写缓冲区
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;
    // 解析状态
    CHECK_STATE m_check_state; // 主状态机当前所处的状态
    METHOD m_method;    

    char m_real_file[FILENAME_LEN]; // 实际文件路径

    // 请求信息
    char *m_url;    //请求目标文件的文件名
    char *m_version;
    char *m_host;
    long m_content_length; // POST请求需要解析的请求体长度
    bool m_linger;  //是否保持连接

    char *m_file_address; //客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;    // 文件信息
    // 分散写
    struct iovec m_iv[2];   // iovec结构体是writev和readv函数使用的结构体，表示一个缓冲区
    int m_iv_count; // 被写内存块的数量

    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send;
    int bytes_have_send;
    char *doc_root;

    map<string, string> m_users;
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
