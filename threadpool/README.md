
半同步/半反应堆线程池
===============

## 一、什么是线程池？

**类比**：餐厅提前招聘好一批服务员，客人来了直接服务，避免临时招聘的延迟。

**线程池**：预先创建好多个线程，有任务来了直接分配给空闲线程，避免频繁创建/销毁线程的开销。

---

## 二、核心成员变量

```cpp
private:
    int m_thread_number;           // 线程数量（默认8个）
    int m_max_requests;            // 请求队列最大长度（默认10000）
    pthread_t *m_threads;          // 线程数组
    std::list<T *> m_workqueue;    // 请求队列（存放待处理的任务）
    locker m_queuelocker;          // 互斥锁（保护队列）
    sem m_queuestat;               // 信号量（通知有任务了）
    connection_pool *m_connPool;   // 数据库连接池
    int m_actor_model;             // Reactor/Proactor 模式
```

**结构图**：

```
                    ┌─────────────────────────────────────┐
                    │           线程池                     │
                    │                                     │
   主线程 ──任务──► │  ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐   │
                    │  │线程1│ │线程2│ │线程3│ │线程4│   │
                    │  └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘   │
                    │     │       │       │       │       │
                    │     ▼       ▼       ▼       ▼       │
                    │  ┌────────────────────────────┐     │
                    │  │      请求队列 (任务列表)     │     │
                    │  │  [任务1][任务2][任务3]...   │     │
                    │  └────────────────────────────┘     │
                    │     ↑                               │
                    │  互斥锁保护 + 信号量通知              │
                    └─────────────────────────────────────┘
```

---

## 三、核心代码解析

### 1. 构造函数 - 创建线程

```cpp
threadpool<T>::threadpool(int actor_model, connection_pool *connPool,
                          int thread_number, int max_requests)
{
    m_threads = new pthread_t[m_thread_number];

    for (int i = 0; i < thread_number; ++i)
    {
        // 创建线程，执行 worker 函数，传入 this 指针
        pthread_create(m_threads + i, NULL, worker, this);

        // 设置为分离态，线程结束后自动回收资源
        pthread_detach(m_threads[i]);
    }
}
```

### 2. 添加任务到队列

```cpp
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();                      // 加锁

    if (m_workqueue.size() >= m_max_requests)  // 检查队列是否已满
    {
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push_back(request);            // 任务入队
    m_queuelocker.unlock();                    // 解锁
    m_queuestat.post();                        // 信号量+1，唤醒等待线程

    return true;
}
```

### 3. 工作线程入口函数

```cpp
// 静态函数，作为线程入口
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;  // arg 就是传入的 this
    pool->run();                           // 调用 run 函数
    return pool;
}
```

### 4. 核心运行函数

```cpp
void threadpool<T>::run()
{
    while (true)
    {
        m_queuestat.wait();       // 等待信号量（无任务时阻塞）

        m_queuelocker.lock();     // 加锁取任务
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();   // 解锁

        if (!request) continue;

        // 处理任务
        if (1 == m_actor_model)   // Reactor 模式
        {
            if (0 == request->m_state)
                request->read_once();
            else
                request->write();
        }
        else                       // Proactor 模式
        {
            request->process();
        }
    }
}
```

---

## 四、pthread_create 函数

```cpp
int pthread_create(pthread_t *thread,        // 存储线程ID
                   const pthread_attr_t *attr, // 线程属性（NULL为默认）
                   void *(*start_routine)(void *), // 线程执行的函数
                   void *arg);               // 传给线程函数的参数

// 示例
pthread_create(m_threads + i, NULL, worker, this);
//               ↑             ↑      ↑      ↑
//           线程ID存储位置   默认属性  线程函数  传递的参数
```

---

## 五、this 指针

**含义**：`this` 是指向当前对象的指针。

```cpp
class Person {
public:
    string name;
    void sayHello() {
        cout << this->name << endl;  // this 指向调用者对象
    }
};

Person p1;
p1.sayHello();  // this == &p1
```

**为什么要传 this？**

`worker` 是静态函数，没有 `this` 指针，无法直接访问类的成员。需要通过参数传入：

```cpp
static void *worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;  // 把 arg 转回对象指针
    pool->run();  // 通过对象指针调用成员函数
}
```

---

## 六、同步机制

### 互斥锁 (locker)

```cpp
locker.lock();    // 加锁
locker.unlock();  // 解锁
```

作用：保证同一时刻只有一个线程访问请求队列。

### 信号量 (sem)

```cpp
sem.wait();  // P操作，-1，为0时阻塞等待
sem.post();  // V操作，+1，唤醒等待的线程
```

作用：`wait()` 让线程在没有任务时睡眠，`post()` 在有新任务时唤醒线程。

---

## 七、工作流程图

```
【主线程】                          【工作线程们】
    │                                    │
    │ 1. 收到 HTTP 请求                   │
    ▼                                    │
 append(request)                         │
    │                                    │
    ├─► 加锁                              │
    ├─► 任务加入队列                       │
    ├─► 解锁                              │
    ├─► post() 信号量+1 ──────────────────┼─► wait() 被唤醒
    │                                    │
    │                              取出任务 (加锁)
    │                                    │
    │                              执行 request->process()
    │                                    │
    │                              处理完毕，继续等待
    ▼                                    ▼
```

---

## 八、为什么用线程池？

| 不用线程池 | 用线程池 |
|-----------|---------|
| 每个请求都创建新线程 | 线程预先创建好 |
| 创建线程耗时 ~1ms | 直接分配，几乎无延迟 |
| 频繁创建销毁，开销大 | 线程复用，高效 |
| 并发高时系统崩溃 | 控制并发数量，稳定 |

---

## 九、总结

线程池的核心是 **生产者-消费者模型**：

1. **生产者**（主线程）：不断往队列添加任务
2. **消费者**（工作线程）：不断从队列取任务执行
3. **同步机制**：互斥锁保护队列，信号量通知任务到达
