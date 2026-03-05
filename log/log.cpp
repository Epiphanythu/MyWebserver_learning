/*
 * ============================================
 * 日志系统实现文件 - log.cpp
 * ============================================
 * 
 * 本文件实现了 Log 类的所有成员函数
 * 包括构造、析构、初始化、写日志、刷新等核心功能
 */

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

/*
 * =====================================================
 * Log 类构造函数
 * =====================================================
 * 
 * 功能：
 * - 初始化成员变量
 * - 设置默认值
 * 
 * 初始化内容：
 * - m_count: 日志行数计数器，初始化为 0
 * - m_is_async: 异步模式标志，初始化为 false（同步模式）
 * 
 * 注意：
 * - 构造函数是私有的，符合单例模式要求
 * - 只能通过 get_instance() 获取实例
 */
Log::Log()
{
    m_count = 0;
    m_is_async = false;
}

/*
 * =====================================================
 * Log 类析构函数
 * =====================================================
 * 
 * 功能：
 * - 关闭日志文件
 * - 释放相关资源
 * 
 * 资源清理：
 * - 如果日志文件指针不为空，关闭文件
 * 
 * 注意：
 * - 析构函数是私有的，符合单例模式要求
 * - 程序退出时自动调用，确保日志文件正确关闭
 */
Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp);
    }
}

/*
 * =====================================================
 * 日志系统初始化函数实现
 * =====================================================
 * 
 * 功能：
 * - 根据参数配置日志系统
 * - 决定同步/异步模式
 * - 创建并打开日志文件
 * 
 * 参数：
 * - file_name: 日志文件名（可包含路径）
 * - close_log: 是否关闭日志
 * - log_buf_size: 日志缓冲区大小
 * - split_lines: 日志文件最大行数
 * - max_queue_size: 阻塞队列最大长度（决定同步/异步模式）
 * 
 * 返回值：
 * - true: 初始化成功
 * - false: 初始化失败（文件打开失败）
 */
//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    /*
     * 第一步：判断并设置异步模式
     * 
     * 判断逻辑：
     * - 如果 max_queue_size >= 1，则启用异步模式
     * - 否则使用同步模式（默认）
     * 
     * 异步模式设置：
     * 1. 设置 m_is_async 为 true
     * 2. 创建阻塞队列对象，指定队列最大长度
     * 3. 创建异步写入线程
     *    - 线程回调函数：flush_log_thread
     *    - 该线程会持续从队列中取出日志并写入文件
     * 
     * 同步模式：
     * - 不创建阻塞队列
     * - 日志直接写入文件
     */
    //如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }
    
    /*
     * 第二步：保存配置参数
     * 
     * 保存的内容：
     * - m_close_log: 日志开关标志
     * - m_log_buf_size: 缓冲区大小
     * - m_split_lines: 日志文件最大行数
     */
    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    
    /*
     * 第三步：分配日志缓冲区
     * 
     * 缓冲区用途：
     * - 临时存储格式化后的日志字符串
     * - 避免频繁的内存分配，提高性能
     * 
     * 初始化：
     * - 分配指定大小的内存
     * - 初始化为全 0（'\0'）
     */
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    /*
     * 第四步：获取当前时间
     * 
     * 时间用途：
     * - 用于生成日志文件名（按日期命名）
     * - 用于记录日志的时间戳
     * 
     * 获取方法：
     * 1. time() 获取秒级时间戳
     * 2. localtime() 转换为本地时间结构体
     */
    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    /*
     * 第五步：解析日志文件名和路径
     * 
     * 目的：
     * - 分离路径和文件名
     * - 用于后续生成新的日志文件名（滚动时）
     * 
     * 处理逻辑：
     * - strrchr() 查找最后一个 '/' 字符
     * - 如果找到 '/'：
     *   - dir_name: 保存路径部分
     *   - log_name: 保存文件名部分
     * - 如果没找到 '/'：
     *   - 只有文件名，没有路径
     *   - dir_name 为空
     */
    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    if (p == NULL)
    {
        /*
         * 没有路径，只有文件名
         * 生成格式：YYYY_MM_DD_filename
         * 例如：2024_03_02_server.log
         */
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else
    {
        /*
         * 包含路径，分离路径和文件名
         * 生成格式：path/YYYY_MM_DD_filename
         * 例如：/var/log/2024_03_02_server.log
         */
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    /*
     * 第六步：保存当前日期并打开日志文件
     * 
     * 保存当前日期：
     * - 用于后续判断日期是否改变
     * - 实现按天滚动日志
     * 
     * 打开日志文件：
     * - 模式："a" (append 追加模式)
     * - 如果文件不存在，创建新文件
     * - 如果文件存在，在文件末尾追加写入
     */
    m_today = my_tm.tm_mday;
    
    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL)
    {
        return false;
    }

    return true;
}

/*
 * =====================================================
 * 写日志核心函数实现
 * =====================================================
 * 
 * 功能：
 * - 格式化日志内容
 * - 根据模式写入日志（同步/异步）
 * - 支持日志文件滚动（按天/按行）
 * 
 * 参数：
 * - level: 日志级别（0=DEBUG, 1=INFO, 2=WARN, 3=ERROR）
 * - format: 格式化字符串
 * - ...: 可变参数列表
 * 
 * 工作流程：
 * 1. 获取当前时间（精确到微秒）
 * 2. 根据日志级别添加标签
 * 3. 检查是否需要滚动日志文件
 * 4. 格式化日志内容
 * 5. 根据模式写入（异步->队列，同步->文件）
 */
void Log::write_log(int level, const char *format, ...)
{
    /*
     * 第一步：获取精确时间
     * 
     * 获取方法：
     * - gettimeofday() 获取秒和微秒
     * - localtime() 转换为本地时间结构体
     * 
     * 时间精度：
     * - 精确到微秒级别
     * - 用于记录日志的详细时间戳
     */
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    
    /*
     * 第二步：根据日志级别添加标签
     * 
     * 级别映射：
     * - 0 -> "[debug]:"
     * - 1 -> "[info]:"
     * - 2 -> "[warn]:"
     * - 3 -> "[erro]:"
     * - 其他 -> "[info]:"（默认）
     * 
     * 用途：
     * - 在日志中显示日志级别
     * - 便于日志过滤和分析
     */
    char s[16] = {0};
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }
    
    /*
     * 第三步：检查并处理日志文件滚动
     * 
     * 滚动条件（满足任一条件即滚动）：
     * 1. 按天滚动：日期改变（m_today != my_tm.tm_mday）
     * 2. 按行滚动：行数达到阈值（m_count % m_split_lines == 0）
     * 
     * 滚动操作：
     * 1. 关闭当前日志文件
     * 2. 生成新的日志文件名
     *    - 按天滚动：新日期的文件名
     *    - 按行滚动：添加序号（如 .1, .2）
     * 3. 打开新的日志文件
     * 
     * 线程安全：
     * - 使用互斥锁保护整个滚动过程
     */
    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();
    m_count++;

    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) //everyday log
    {
        /*
         * 准备滚动：
         * - 刷新并关闭当前文件
         * - 准备新文件名
         */
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};
       
        /*
         * 生成日期后缀
         * 格式：YYYY_MM_DD_
         */
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
       
        /*
         * 根据滚动类型生成新文件名
         */
        if (m_today != my_tm.tm_mday)
        {
            /*
             * 按天滚动
             * 新文件名：dir_name + 日期 + log_name
             * 例如：/var/log/2024_03_03_server.log
             */
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else
        {
            /*
             * 按行滚动
             * 新文件名：dir_name + 日期 + log_name + 序号
             * 例如：/var/log/2024_03_02_server.log.1
             */
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        
        /*
         * 打开新日志文件
         * - 追加模式
         * - 如果失败，后续写入会出错
         */
        m_fp = fopen(new_log, "a");
    }
 
    m_mutex.unlock();

    /*
     * 第四步：格式化日志内容
     * 
     * 格式化步骤：
     * 1. 初始化可变参数列表
     * 2. 格式化时间戳和日志级别
     * 3. 格式化用户提供的日志消息
     * 4. 添加换行符和结束符
     * 
     * 日志格式：
     * YYYY-MM-DD HH:MM:SS.微秒 [level]: 用户消息\n
     * 例如：2024-03-02 15:30:45.123456 [info]: Server started\n
     * 
     * 线程安全：
     * - 使用互斥锁保护格式化过程
     * - 防止多个线程同时修改缓冲区
     */
    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    /*
     * 格式化时间戳和日志级别
     * - snprintf: 安全的格式化函数
     * - 限制最大长度为 48 字节
     * - 格式：YYYY-MM-DD HH:MM:SS.微秒 [level]: 
     */
    //写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    /*
     * 格式化用户提供的日志消息
     * - vsnprintf: 处理可变参数的格式化函数
     * - 从 m_buf + n 开始写入，追加到时间戳后面
     * - 剩余缓冲区空间：m_log_buf_size - n - 1
     */
    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    
    /*
     * 添加换行符和字符串结束符
     * - 确保每条日志独占一行
     * - 确保字符串正确终止
     */
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    
    /*
     * 将格式化后的日志转换为 string 对象
     */
    log_str = m_buf;

    m_mutex.unlock();

    /*
     * 第五步：根据模式写入日志 
     * 
     * 异步模式（m_is_async == true）：
     * - 检查队列是否已满
     * - 未满：推入阻塞队列，由异步线程写入
     * - 已满：降级为同步模式，直接写入文件
     * 
     * 同步模式（m_is_async == false 或队列已满）：
     * - 直接写入文件
     * 
     * 线程安全：
     * - 同步模式使用互斥锁保护文件写入
     * - 异步模式下，队列本身也是线程安全的
     */
    if (m_is_async && !m_log_queue->full())
    {
        /*
         * 异步模式：推入阻塞队列
         * - 如果队列未满，快速返回
         * - 异步线程会从队列中取出并写入
         */
        m_log_queue->push(log_str);
    }
    else
    {
        /*
         * 同步模式：直接写入文件
         * - 或者是队列已满时的降级处理
         * - 使用互斥锁保护文件写入
         */
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    /*
     * 第六步：清理可变参数列表
     * 
     * va_end：
     * - 清理可变参数列表
     * - 防止资源泄漏
     */
    va_end(valst);
}

/*
 * =====================================================
 * 刷新日志缓冲区函数实现
 * =====================================================
 * 
 * 功能：
 * - 强制刷新文件流缓冲区
 * - 确保日志立即写入磁盘
 * 
 * 工作原理：
 * - fflush() 刷新 C 标准库的缓冲区
 * - 将缓冲区中的数据立即写入文件
 * - 而不是等待缓冲区满或程序结束
 * 
 * 使用场景：
 * - 程序退出前，确保所有日志都已写入
 * - 重要日志记录后，立即持久化
 * - 异步模式下，由日志宏自动调用
 */
void Log::flush(void)
{
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}
