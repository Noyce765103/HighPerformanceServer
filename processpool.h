#ifndef PROCESSPOOL_H
#define PROCESSPOOL_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

/* 描述一个子进程的类，m_pid是目标子进程的PID, m_pipefd是父进程和子进程通信用的管道 */
class Process
{
public:
    pid_t m_pid;
    int m_pipefd[2];
    Process() : m_pid(-1)   {} 
};
/* 进程池类，将它定义为模板类是为了代码复用。其模板参数是处理逻辑任务的类 */
template< typename T >
class Processpool
{
private:
    //单例模式
    Processpool(int listenfd, int process_number = 4);
public:
    static Processpool<T>* create(int listenfd, int process_number = 4)
    {
        if(!m_instance)
        {
            m_instance = new Processpool<T>(listenfd, process_number);
        }
        return m_instance;
    }

    ~Processpool()
    {
        delete [] m_sub_process;
    }

    //启动进程池
    void run();
private:
    void setup_sig_pipe();
    void run_parent();
    void run_child();
private:
    // 进程池允许的最大子进程数量
    static const int MAX_PROCESS_NUMBER = 8;
    // 每个子进程最多能处理的客户数量
    static const int USER_PER_PROCESS = 65536;
    // epoll最多能处理的事件数
    static const int MAX_EVENT_NUMBER = 10000;
    //进程池中的进程总数
    int m_process_number;
    //子进程在池中的序号，从0开始, 当m_idx=-1代表父进程
    int m_idx;
    //每个进程都有一个epoll内核事件表，用m_epollfd标识
    int m_epollfd;
    //监听socket
    int m_listenfd;
    //子进程通过m_stop来决定是否停止运行
    int m_stop;
    //保存所有子进程的描述信息
    process* m_sub_process;
    //进程池静态实例
    static Processpool<T>* m_instance = nullptr;
};

//用于处理信号的管道，以实现统一事件源。
static int sig_pipefd[2];

//将文件描述符设置为非阻塞，返回旧描述符信息
static int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将fd注册到epoll事件集中，采用ET边缘触发模式
static void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从epollfd标识的epoll内核事件表中删除fd上的所有注册事件
static void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}
//信号处理回调函数
static void sig_handler(int sig)
{
    //保留原来的errno，在函数最后恢复，以保证函数二点可重入性
    int save_errno = errno;
    int msg = sig;
    send(sig_pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

static void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if(restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//进程池构造函数。参数listenfd是监听socket，它必须在创建进程池之前被创建，否则子进程无法直接引用它。参数process_number指定进程池中子进程的数量
template<typename T>
Processpool<T>::Processpool(int listenfd, int process_number):m_listenfd(listenfd), m_process_number(process_number), m_idx(-1), m_stop(false)
{
    assert((process_number > 0) && (process_number <= MAX_PROCESS_NUMBER));
    m_sub_process = new Process[process_number];
    assert(m_sub_process);
    //创建process_number个子进程，并建立他们和父进程之间的管道
    for(int i = 0; i < process_number; i ++)
    {
        int ret = socketpair(PF_UNIX, SOCK_STREAM, 0. m_sub_process[i].m_pipdfd);
        assert(ret == 0);
        m_sub_process[i].m_pid = fork();
        assert(m_sub_process[i].m_pid >= 0);
        if(m_sub_process[i].m_pid > 0)  //父进程继续生成子进程
        {
            close(m_sub_process[i].m_pipefd[1]);
            continue;
        }
        else    //子进程执行的代码块，子进程不再生成自己的子进程，所以break
        {
            close(m_sub_process[i].m_pipefd[0]);
            m_idx = i;
            break;
        }
    }
}


//统一事件源
template<typename T>
void Processpool<T>::setup_sig_pipe()
{
    //创建epoll事件监听表和信号管道
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0 , sig_pipefd);
    assert(ret != -1);
    setnonblocking(sig_pipefd[1]);
    addfd(m_epollfd, sig_pipefd[0]);

    //设置信号处理函数
    addsig(SIGCHLD, sig_handler);
    addsig(SIGTERM, sig_handler);
    addsig(SIGINT, sig_handler);
    addsig(SIGPIPE, SIG_IGN);   //SIG_IGN信号忽略
}
//父进程中m_idx值为-1，子进程中m_idx值大于等于0， 我们据此判断接下来要运行的是父进程代码还是子进程代码
template<typename T>
void Processpool<T>::run()
{
    if(m_idx != -1)
    {
        run_child();
        return;
    }
    run_parent();
}

#endif