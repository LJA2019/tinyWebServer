/* 使用模板参数的线程池，用以封装对逻辑任务的处理 */
#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>

#include "locker.h"     // 线程同步包装类

/* 线程池类，定义为模板方便代码复用，参数T为任务类 */
template<typename T>
class threadpool {
public:
    // thread_number是线程池中线程数量，max_requests是请求队列中最多允许的 等待处理的请求的数量
    threadpool(int thread_number = 8, int max_requests = 10000);    
    ~threadpool();
    bool append(T *request);    // 往请求队列中添加任务
private:
    /* 工作线程运行的函数，不断从请求队列中取出任务并执行 */
    // 使用静态函数，避免传入thread_create(...)的第三个参数函数指针的类型不对
    // 方法二，添加类的静态成员指针，然后通过静态成员指针调用？？
    static void *worker(void *arg);     
    void run();

    int m_thread_number;    // 线程池中线程数
    int m_max_requests;     // 请求队列最大容量
    pthread_t *m_threads;   // 描述线程池的数组，大小为 m_thread_number
    std::list<T*>m_workqueue; // 请求队列
    locker m_queuelocker;   // 保护请求队列的互斥锁
    sem m_queuestat;    // 信号量，是否有任务需要处理
    bool m_stop;    // 是否结束线程
};

template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) :
    m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL) {
    if ((thread_number <= 0) || (max_requests <= 0)) {
        throw std::exception();
    }
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) {
        throw std::exception();
    }
    for (int i = 0; i < thread_number; ++i) {
        printf("create the %dth thread\n", i);
        if (pthread_create(m_threads+i, NULL, worker, this) != 0) {
            delete [] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]) != 0) {
            delete [] m_threads;
            throw std::exception();
        }
    }
}

template<typename T>
threadpool<T>::~threadpool() {
    delete [] m_threads;
    m_stop = true;
}

template<typename T>
bool threadpool<T>::append(T *request) {
    m_queuelocker.lock();   // 操作工作队列时需要加锁，因为它被所有线程共享
    if (m_workqueue.size() >= m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();     // 信号量加1
    return true;
}

template<typename T>
void *threadpool<T>::worker(void *arg) {
    threadpool<T> *pool = (threadpool<T>*)arg;  // 可以省略模板？？ threadpool *pool = (threadpool*)arg;
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>::run() {
    while (!m_stop) {
        m_queuestat.wait();
        m_queuelocker.lock();
        // if (m_workqueue.empty()) {
        //     m_queuelocker.unlock();
        //     continue;
        // }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request) {
            continue;
        }
        request->process();
    }
}

#endif