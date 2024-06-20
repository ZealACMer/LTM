#include <strings.h>
#include <functional>

#include "TcpServer.h"
#include "Logger.h"
#include "TcpConnection.h" //构造函数error:invalid use of incomplete type 'class ...' =>可能是没有包含相应的头文件

static EventLoop* CheckLoopNotNull(EventLoop *loop) //如果不设置成静态，在多个文件中定义时，编译会产生冲突错误
{
    if(loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d mainLoop is null! \n", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}

TcpServer::TcpServer(EventLoop *loop,
                const InetAddress &listenAddr,
                const std::string &nameArg,
                Option option)
                : loop_(CheckLoopNotNull(loop)) //mainLoop必须不为空
                , ipPort_(listenAddr.toIpPort()) //用户传入
                , name_(nameArg) //服务器名称
                , acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)) //Acceptor(创建socket，然后listen，Acceptor被封装成acceptChannel，才能调用Poller的函数注册在Poller上，开始监听事件)运行在mainLoop中，用于监听listenAddr
                , threadPool_(new EventLoopThreadPool(loop, name_))
                , connectionCallback_()
                , messageCallback_()
                , nextConnId_(1)
                , started_(0)
{
    //当有新用户连接时，会执行TcpServer::newConnection回调
    acceptor_->setNewConnectionCallback(std::bind(&TcpServer::newConnection, this,
        std::placeholders::_1, std::placeholders::_2)); //参数为（用于和客户端通信的socketfd，客户端的ip地址端口号）
}

TcpServer::~TcpServer()
{
    for(auto &item : connections_)
    {
        //这个局部的shared_ptr智能指针对象，出右括号，可以自动释放new出来的TcpConnection对象资源
        TcpConnectionPtr conn(item.second);
        item.second.reset();

        //销毁连接
        conn->getLoop()->runInLoop(
            std::bind(&TcpConnection::connectDestroyed, conn)
        );
    }
}

//设置底层subloop的个数
void TcpServer::setThreadNum(int numThreads)
{
    threadPool_->setThreadNum(numThreads);
}

//开启服务器监听 -> 随后使用loop.loop()开启poller监听，即epoll_wait()
void TcpServer::start()
{
    if(started_++ == 0) //防止一个TcpServer对象被start多次
    {
        threadPool_->start(threadInitCallback_); //启动底层的loop线程池
        loop_->runInLoop(std::bind(&Acceptor::listen, acceptor_.get())); //执行回调，listen && enableReading，将acceptChannel注册在poller上
    }
}

//有一个新的客户端的连接，acceptor会执行这个回调操作
void TcpServer::newConnection(int sockfd, const InetAddress &peerAddr)
{
    //轮询算法，选择一个subLoop，来管理channel
    EventLoop *ioLoop = threadPool_->getNextLoop();
    char buf[64] = {0};
    snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);
    ++nextConnId_;
    std::string connName = name_ + buf;

    LOG_INFO("TcpServer::newConnection [%s] - new connection [%s] from %s \n", name_.c_str(), connName.c_str(), peerAddr.toIpPort().c_str());

    //通过sockfd获取其绑定的本机的ip地址和端口信息
    sockaddr_in local;
    ::bzero(&local, sizeof local);
    socklen_t addrlen = sizeof local;
    if(::getsockname(sockfd, (sockaddr*)&local, &addrlen) < 0)
    {
        LOG_ERROR("sockets::getLocalAddr error!");
    }
    InetAddress localAddr(local);

    //根据连接成功的sockfd，创建TcpConnection连接对象
    TcpConnectionPtr conn(new TcpConnection(
                    ioLoop,
                    connName,
                    sockfd, //通过此sockfd，底层可以创建Socket和Channel
                    localAddr,
                    peerAddr
                    ));
    connections_[connName] = conn;
    //下面的回调是用户设置给TcpServer的 => TcpServer继续设置给TcpConnection => TcpConnection继续设置给Channel => 
    //Channel注册到Poller => Poller监听的事件发生后，通知（notify）channel调用回调
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);

    //设置如何关闭连接的回调 conn->shutDown()
    conn->setCloseCallback(
        std::bind(&TcpServer::removeConnection, this, std::placeholders::_1)
    );

    //直接调用TcpConnection::connectEstablished
    ioLoop->runInLoop(std::bind(&TcpConnection::connectEstablished, conn));
}

void TcpServer::removeConnection(const TcpConnectionPtr &conn)
{
    loop_->runInLoop(
        std::bind(&TcpServer::removeConnectionInLoop, this, conn)
    );
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr &conn)
{
    LOG_INFO("TcpServer::removeConnectionInLoop [%s] - connection %s \n", name_.c_str(), conn->name().c_str());

    connections_.erase(conn->name());
    EventLoop *ioLoop = conn->getLoop(); //获取connection所在的loop
    ioLoop->queueInLoop(
        std::bind(&TcpConnection::connectDestroyed, conn)
    );
}