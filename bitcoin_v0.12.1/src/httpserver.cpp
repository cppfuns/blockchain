// Copyright (c) 2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "httpserver.h"

#include "chainparamsbase.h"
#include "compat.h"
#include "util.h"
#include "netbase.h"
#include "rpcprotocol.h" // For HTTP status codes
#include "sync.h"
#include "ui_interface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>

#include <event2/event.h>
#include <event2/http.h>
#include <event2/thread.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>

#ifdef EVENT__HAVE_NETINET_IN_H
#include <netinet/in.h>
#ifdef _XOPEN_SOURCE_EXTENDED
#include <arpa/inet.h>
#endif
#endif

#include <boost/algorithm/string/case_conv.hpp> // for to_lower()
#include <boost/foreach.hpp>
#include <boost/scoped_ptr.hpp>

/** Maximum size of http request (request line + headers) */
static const size_t MAX_HEADERS_SIZE = 8192; // http 请求行 + 请求头部大小限制 8K

/** HTTP request work item */
class HTTPWorkItem : public HTTPClosure
{
public:
    HTTPWorkItem(HTTPRequest* req, const std::string &path, const HTTPRequestHandler& func):
        req(req), path(path), func(func)
    {
    }
    void operator()()
    {
        func(req.get(), path);
    }

    boost::scoped_ptr<HTTPRequest> req;

private:
    std::string path;
    HTTPRequestHandler func;
};

/** Simple work queue for distributing work over multiple threads.
 * Work items are simply callable objects.
 */
template <typename WorkItem>
class WorkQueue
{
private:
    /** Mutex protects entire object */
    CWaitableCriticalSection cs;
    CConditionVariable cond;
    /* XXX in C++11 we can use std::unique_ptr here and avoid manual cleanup */
    std::deque<WorkItem*> queue; // 任务队列
    bool running; // 运行状态（决定是否运行/退出循环）
    size_t maxDepth; // 最大深度（容量）
    int numThreads; // 线程数

    /** RAII object to keep track of number of running worker threads */
    class ThreadCounter // 嵌套类
    {
    public:
        WorkQueue &wq;
        ThreadCounter(WorkQueue &w): wq(w)
        {
            boost::lock_guard<boost::mutex> lock(wq.cs);
            wq.numThreads += 1;
        }
        ~ThreadCounter()
        {
            boost::lock_guard<boost::mutex> lock(wq.cs);
            wq.numThreads -= 1;
            wq.cond.notify_all();
        }
    };

public:
    WorkQueue(size_t maxDepth) : running(true),
                                 maxDepth(maxDepth),
                                 numThreads(0)
    {
    }
    /*( Precondition: worker threads have all stopped
     * (call WaitExit)
     */
    ~WorkQueue()
    {
        while (!queue.empty()) {
            delete queue.front();
            queue.pop_front();
        }
    }
    /** Enqueue a work item */
    bool Enqueue(WorkItem* item)
    {
        boost::unique_lock<boost::mutex> lock(cs);
        if (queue.size() >= maxDepth) {
            return false;
        }
        queue.push_back(item);
        cond.notify_one();
        return true;
    }
    /** Thread function */
    void Run() // 线程函数：不断从任务队列中读取、删除并执行任务，任务类型为 WorkItem（类类型）
    {
        ThreadCounter count(*this);
        while (running) { // loop
            WorkItem* i = 0;
            {
                boost::unique_lock<boost::mutex> lock(cs);
                while (running && queue.empty()) // 任务队列为空
                    cond.wait(lock); // 等待条件被激活（往队列里添加任务时）
                if (!running)
                    break; // break out of loop
                i = queue.front(); // 取队头元素（任务队列中第一个元素）
                queue.pop_front(); // 队头出队
            }
            (*i)(); // 执行任务
            delete i; // 执行后删除
        }
    }
    /** Interrupt and exit loops */
    void Interrupt() // 打断并退出循环
    {
        boost::unique_lock<boost::mutex> lock(cs);
        running = false; // 改变运行状态为 false
        cond.notify_all();
    }
    /** Wait for worker threads to exit */
    void WaitExit()
    {
        boost::unique_lock<boost::mutex> lock(cs);
        while (numThreads > 0)
            cond.wait(lock);
    }

    /** Return current depth of queue */
    size_t Depth()
    {
        boost::unique_lock<boost::mutex> lock(cs);
        return queue.size();
    }
};

struct HTTPPathHandler
{
    HTTPPathHandler() {}
    HTTPPathHandler(std::string prefix, bool exactMatch, HTTPRequestHandler handler):
        prefix(prefix), exactMatch(exactMatch), handler(handler)
    {
    }
    std::string prefix; // 请求的路径
    bool exactMatch; // 精确匹配 或 前缀匹配（在 http_request_cb 中完成验证）
    HTTPRequestHandler handler; // 对某个 http 路径请求
};

/** HTTP module state */

//! libevent event loop
static struct event_base* eventBase = 0;
//! HTTP server
struct evhttp* eventHTTP = 0;
//! List of subnets to allow RPC connections from
static std::vector<CSubNet> rpc_allow_subnets;
//! Work queue for handling longer requests off the event loop thread
static WorkQueue<HTTPClosure>* workQueue = 0;
//! Handlers for (sub)paths
std::vector<HTTPPathHandler> pathHandlers; // http 请求路径对应的处理函数列表
//! Bound listening sockets
std::vector<evhttp_bound_socket *> boundSockets; // 已绑定的 http socket 列表

/** Check if a network address is allowed to access the HTTP server */
static bool ClientAllowed(const CNetAddr& netaddr) // 检查一个网络地址是否被允许访问 HTTP 服务器
{
    if (!netaddr.IsValid()) // 检查地址有效性
        return false;
    BOOST_FOREACH (const CSubNet& subnet, rpc_allow_subnets) // 遍历 ACL 访问控制列表，并与指定地址比对
        if (subnet.Match(netaddr))
            return true;
    return false;
}

/** Initialize ACL list for HTTP server */
static bool InitHTTPAllowList() // ACL: Allow Control List
{
    rpc_allow_subnets.clear(); // 清空子网列表
    rpc_allow_subnets.push_back(CSubNet("127.0.0.0/8")); // always allow IPv4 local subnet
    rpc_allow_subnets.push_back(CSubNet("::1"));         // always allow IPv6 localhost
    if (mapMultiArgs.count("-rpcallowip")) { // 获取 -rpcallowip 设置的 ip 列表
        const std::vector<std::string>& vAllow = mapMultiArgs["-rpcallowip"];
        BOOST_FOREACH (std::string strAllow, vAllow) { // 遍历该列表
            CSubNet subnet(strAllow); // 创建子网对象
            if (!subnet.IsValid()) { // 检查子网有效性
                uiInterface.ThreadSafeMessageBox(
                    strprintf("Invalid -rpcallowip subnet specification: %s. Valid are a single IP (e.g. 1.2.3.4), a network/netmask (e.g. 1.2.3.4/255.255.255.0) or a network/CIDR (e.g. 1.2.3.4/24).", strAllow),
                    "", CClientUIInterface::MSG_ERROR);
                return false;
            }
            rpc_allow_subnets.push_back(subnet); // 加入 ACL 列表
        }
    }
    std::string strAllowed; // 记录日志
    BOOST_FOREACH (const CSubNet& subnet, rpc_allow_subnets)
        strAllowed += subnet.ToString() + " ";
    LogPrint("http", "Allowing HTTP connections from: %s\n", strAllowed);
    return true;
}

/** HTTP request method as string - use for logging only */
static std::string RequestMethodString(HTTPRequest::RequestMethod m)
{
    switch (m) {
    case HTTPRequest::GET:
        return "GET";
        break;
    case HTTPRequest::POST:
        return "POST";
        break;
    case HTTPRequest::HEAD:
        return "HEAD";
        break;
    case HTTPRequest::PUT:
        return "PUT";
        break;
    default:
        return "unknown";
    }
}

/** HTTP request callback */
static void http_request_cb(struct evhttp_request* req, void* arg)
{
    std::auto_ptr<HTTPRequest> hreq(new HTTPRequest(req)); // 创建一个 HTTP 请求

    LogPrint("http", "Received a %s request for %s from %s\n",
             RequestMethodString(hreq->GetRequestMethod()), hreq->GetURI(), hreq->GetPeer().ToString());

    // Early address-based allow check
    if (!ClientAllowed(hreq->GetPeer())) { // 检查请求连入地址是否被允许，即是否存在于 ACL 访问控制列表中
        hreq->WriteReply(HTTP_FORBIDDEN);
        return;
    }

    // Early reject unknown HTTP methods
    if (hreq->GetRequestMethod() == HTTPRequest::UNKNOWN) { // 提前拒绝未知方法
        hreq->WriteReply(HTTP_BADMETHOD);
        return;
    }

    // Find registered handler for prefix
    std::string strURI = hreq->GetURI();
    std::string path;
    std::vector<HTTPPathHandler>::const_iterator i = pathHandlers.begin();
    std::vector<HTTPPathHandler>::const_iterator iend = pathHandlers.end();
    for (; i != iend; ++i) { // 查找处理函数，分精确匹配和前缀匹配两种
        bool match = false;
        if (i->exactMatch)
            match = (strURI == i->prefix);
        else
            match = (strURI.substr(0, i->prefix.size()) == i->prefix);
        if (match) {
            path = strURI.substr(i->prefix.size());
            break;
        }
    }

    // Dispatch to worker thread
    if (i != iend) { // 若找到了对应的处理函数，则派发到工作线程
        std::auto_ptr<HTTPWorkItem> item(new HTTPWorkItem(hreq.release(), path, i->handler)); // 把请求和对应的处理函数封装为 HTTPWorkItem 对象
        assert(workQueue);
        if (workQueue->Enqueue(item.get())) // 再把该对象加入任务队列，该任务队列由单独的线程不断执行
            item.release(); /* if true, queue took ownership */
        else
            item->req->WriteReply(HTTP_INTERNAL, "Work queue depth exceeded");
    } else {
        hreq->WriteReply(HTTP_NOTFOUND);
    }
}

/** Callback to reject HTTP requests after shutdown. */
static void http_reject_request_cb(struct evhttp_request* req, void*)
{
    LogPrint("http", "Rejecting request while shutting down\n");
    evhttp_send_error(req, HTTP_SERVUNAVAIL, NULL);
}

/** Event dispatcher thread */
static void ThreadHTTP(struct event_base* base, struct evhttp* http)
{
    RenameThread("bitcoin-http");
    LogPrint("http", "Entering http event loop\n");
    event_base_dispatch(base); // 5.派发事件循环
    // Event loop will be interrupted by InterruptHTTPServer()
    LogPrint("http", "Exited http event loop\n");
}

/** Bind HTTP server to specified addresses */
static bool HTTPBindAddresses(struct evhttp* http)
{
    int defaultPort = GetArg("-rpcport", BaseParams().RPCPort()); // 设置 RPC 端口
    std::vector<std::pair<std::string, uint16_t> > endpoints; // std::pair<IP, PORT>

    // Determine what addresses to bind to
    if (!mapArgs.count("-rpcallowip")) { // Default to loopback if not allowing external IPs // 优先白名单
        endpoints.push_back(std::make_pair("::1", defaultPort));
        endpoints.push_back(std::make_pair("127.0.0.1", defaultPort));
        if (mapArgs.count("-rpcbind")) {
            LogPrintf("WARNING: option -rpcbind was ignored because -rpcallowip was not specified, refusing to allow everyone to connect\n");
        }
    } else if (mapArgs.count("-rpcbind")) { // Specific bind address // 指定的 rpc 地址
        const std::vector<std::string>& vbind = mapMultiArgs["-rpcbind"];
        for (std::vector<std::string>::const_iterator i = vbind.begin(); i != vbind.end(); ++i) {
            int port = defaultPort;
            std::string host;
            SplitHostPort(*i, port, host);
            endpoints.push_back(std::make_pair(host, port));
        }
    } else { // No specific bind address specified, bind to any
        endpoints.push_back(std::make_pair("::", defaultPort));
        endpoints.push_back(std::make_pair("0.0.0.0", defaultPort));
    }

    // Bind addresses
    for (std::vector<std::pair<std::string, uint16_t> >::iterator i = endpoints.begin(); i != endpoints.end(); ++i) {
        LogPrint("http", "Binding RPC on address %s port %i\n", i->first, i->second);
        evhttp_bound_socket *bind_handle = evhttp_bind_socket_with_handle(http, i->first.empty() ? NULL : i->first.c_str(), i->second); // 绑定 IP 和端口
        if (bind_handle) {
            boundSockets.push_back(bind_handle); // 加入已绑定的 http socket 列表
        } else {
            LogPrintf("Binding RPC on address %s port %i failed.\n", i->first, i->second);
        }
    }
    return !boundSockets.empty();
}

/** Simple wrapper to set thread name and run work queue */
static void HTTPWorkQueueRun(WorkQueue<HTTPClosure>* queue)
{
    RenameThread("bitcoin-httpworker");
    queue->Run(); // 依次运行队列中的任务
}

/** libevent event log callback */
static void libevent_log_cb(int severity, const char *msg)
{
#ifndef EVENT_LOG_WARN
// EVENT_LOG_WARN was added in 2.0.19; but before then _EVENT_LOG_WARN existed.
# define EVENT_LOG_WARN _EVENT_LOG_WARN
#endif
    if (severity >= EVENT_LOG_WARN) // Log warn messages and higher without debug category
        LogPrintf("libevent: %s\n", msg);
    else
        LogPrint("libevent", "libevent: %s\n", msg);
}

bool InitHTTPServer()
{
    struct evhttp* http = 0;
    struct event_base* base = 0;

    if (!InitHTTPAllowList()) // 初始化 HTTP ACL 访问控制列表（白名单）
        return false;

    if (GetBoolArg("-rpcssl", false)) { // rpcssl 默认关闭，当前版本不支持，如果设置了就报错
        uiInterface.ThreadSafeMessageBox(
            "SSL mode for RPC (-rpcssl) is no longer supported.",
            "", CClientUIInterface::MSG_ERROR);
        return false;
    }

    // Redirect libevent's logging to our own log
    event_set_log_callback(&libevent_log_cb); // 重定向 libevent 日志到当前日志系统
#if LIBEVENT_VERSION_NUMBER >= 0x02010100
    // If -debug=libevent, set full libevent debugging.
    // Otherwise, disable all libevent debugging.
    if (LogAcceptCategory("libevent"))
        event_enable_debug_logging(EVENT_DBG_ALL);
    else
        event_enable_debug_logging(EVENT_DBG_NONE);
#endif
#ifdef WIN32 // 初始化 libevent 的 http 服务端协议
    evthread_use_windows_threads();
#else
    evthread_use_pthreads();
#endif

    base = event_base_new(); // XXX RAII // 1.创建 event_base 对象
    if (!base) {
        LogPrintf("Couldn't create an event_base: exiting\n");
        return false;
    }

    /* Create a new evhttp object to handle requests. */
    http = evhttp_new(base); // XXX RAII 2.利用 base 创建 evhttp 对象
    if (!http) {
        LogPrintf("couldn't create evhttp. Exiting.\n");
        event_base_free(base);
        return false;
    }

    evhttp_set_timeout(http, GetArg("-rpcservertimeout", DEFAULT_HTTP_SERVER_TIMEOUT)); // 设置 http 服务超时时间为 rpc 服务超时，默认 30 秒
    evhttp_set_max_headers_size(http, MAX_HEADERS_SIZE); // http 头大小，默认 8K
    evhttp_set_max_body_size(http, MAX_SIZE); // 设置消息体大小，默认 32M
    evhttp_set_gencb(http, http_request_cb, NULL); // 4.设置处理请求的回调函数 http_request_cb

    if (!HTTPBindAddresses(http)) { // 3.evhttp_bind_socket(http, "0.0.0.0", port),绑定 IP 地址和端口
        LogPrintf("Unable to bind any endpoint for RPC server\n");
        evhttp_free(http);
        event_base_free(base);
        return false;
    }

    LogPrint("http", "Initialized HTTP server\n");
    int workQueueDepth = std::max((long)GetArg("-rpcworkqueue", DEFAULT_HTTP_WORKQUEUE), 1L); // 获取 HTTP 任务队列最大容量，默认 16，最小为 1
    LogPrintf("HTTP: creating work queue of depth %d\n", workQueueDepth);

    workQueue = new WorkQueue<HTTPClosure>(workQueueDepth); // 创建任务队列
    eventBase = base;
    eventHTTP = http;
    return true;
}

boost::thread threadHTTP;

bool StartHTTPServer()
{
    LogPrint("http", "Starting HTTP server\n");
    int rpcThreads = std::max((long)GetArg("-rpcthreads", DEFAULT_HTTP_THREADS), 1L); // 获取 RPC 线程数，默认为 4，至少为 1
    LogPrintf("HTTP: starting %d worker threads\n", rpcThreads);
    threadHTTP = boost::thread(boost::bind(&ThreadHTTP, eventBase, eventHTTP)); // 5.派发事件循环，http 协议启动

    for (int i = 0; i < rpcThreads; i++) // 创建 HTTP 服务（任务队列运行）线程
        boost::thread(boost::bind(&HTTPWorkQueueRun, workQueue));
    return true;
}

void InterruptHTTPServer()
{
    LogPrint("http", "Interrupting HTTP server\n");
    if (eventHTTP) {
        // Unlisten sockets
        BOOST_FOREACH (evhttp_bound_socket *socket, boundSockets) {
            evhttp_del_accept_socket(eventHTTP, socket);
        }
        // Reject requests on current connections
        evhttp_set_gencb(eventHTTP, http_reject_request_cb, NULL);
    }
    if (workQueue)
        workQueue->Interrupt();
}

void StopHTTPServer()
{
    LogPrint("http", "Stopping HTTP server\n");
    if (workQueue) {
        LogPrint("http", "Waiting for HTTP worker threads to exit\n");
        workQueue->WaitExit();
        delete workQueue;
    }
    if (eventBase) {
        LogPrint("http", "Waiting for HTTP event thread to exit\n");
        // Give event loop a few seconds to exit (to send back last RPC responses), then break it
        // Before this was solved with event_base_loopexit, but that didn't work as expected in
        // at least libevent 2.0.21 and always introduced a delay. In libevent
        // master that appears to be solved, so in the future that solution
        // could be used again (if desirable).
        // (see discussion in https://github.com/bitcoin/bitcoin/pull/6990)
#if BOOST_VERSION >= 105000
        if (!threadHTTP.try_join_for(boost::chrono::milliseconds(2000))) {
#else
        if (!threadHTTP.timed_join(boost::posix_time::milliseconds(2000))) {
#endif
            LogPrintf("HTTP event loop did not exit within allotted time, sending loopbreak\n");
            event_base_loopbreak(eventBase);
            threadHTTP.join();
        }
    }
    if (eventHTTP) {
        evhttp_free(eventHTTP);
        eventHTTP = 0;
    }
    if (eventBase) {
        event_base_free(eventBase);
        eventBase = 0;
    }
    LogPrint("http", "Stopped HTTP server\n");
}

struct event_base* EventBase()
{
    return eventBase;
}

static void httpevent_callback_fn(evutil_socket_t, short, void* data)
{
    // Static handler: simply call inner handler
    HTTPEvent *self = ((HTTPEvent*)data);
    self->handler();
    if (self->deleteWhenTriggered)
        delete self;
}

HTTPEvent::HTTPEvent(struct event_base* base, bool deleteWhenTriggered, const boost::function<void(void)>& handler):
    deleteWhenTriggered(deleteWhenTriggered), handler(handler)
{
    ev = event_new(base, -1, 0, httpevent_callback_fn, this);
    assert(ev);
}
HTTPEvent::~HTTPEvent()
{
    event_free(ev);
}
void HTTPEvent::trigger(struct timeval* tv)
{
    if (tv == NULL)
        event_active(ev, 0, 0); // immediately trigger event in main thread
    else
        evtimer_add(ev, tv); // trigger after timeval passed
}
HTTPRequest::HTTPRequest(struct evhttp_request* req) : req(req),
                                                       replySent(false)
{
}
HTTPRequest::~HTTPRequest()
{
    if (!replySent) {
        // Keep track of whether reply was sent to avoid request leaks
        LogPrintf("%s: Unhandled request\n", __func__);
        WriteReply(HTTP_INTERNAL, "Unhandled request");
    }
    // evhttpd cleans up the request, as long as a reply was sent.
}

std::pair<bool, std::string> HTTPRequest::GetHeader(const std::string& hdr)
{
    const struct evkeyvalq* headers = evhttp_request_get_input_headers(req);
    assert(headers);
    const char* val = evhttp_find_header(headers, hdr.c_str());
    if (val)
        return std::make_pair(true, val);
    else
        return std::make_pair(false, "");
}

std::string HTTPRequest::ReadBody()
{
    struct evbuffer* buf = evhttp_request_get_input_buffer(req);
    if (!buf)
        return "";
    size_t size = evbuffer_get_length(buf);
    /** Trivial implementation: if this is ever a performance bottleneck,
     * internal copying can be avoided in multi-segment buffers by using
     * evbuffer_peek and an awkward loop. Though in that case, it'd be even
     * better to not copy into an intermediate string but use a stream
     * abstraction to consume the evbuffer on the fly in the parsing algorithm.
     */
    const char* data = (const char*)evbuffer_pullup(buf, size);
    if (!data) // returns NULL in case of empty buffer
        return "";
    std::string rv(data, size);
    evbuffer_drain(buf, size);
    return rv;
}

void HTTPRequest::WriteHeader(const std::string& hdr, const std::string& value)
{
    struct evkeyvalq* headers = evhttp_request_get_output_headers(req);
    assert(headers);
    evhttp_add_header(headers, hdr.c_str(), value.c_str());
}

/** Closure sent to main thread to request a reply to be sent to
 * a HTTP request.
 * Replies must be sent in the main loop in the main http thread,
 * this cannot be done from worker threads.
 */
void HTTPRequest::WriteReply(int nStatus, const std::string& strReply)
{
    assert(!replySent && req);
    // Send event to main http thread to send reply message
    struct evbuffer* evb = evhttp_request_get_output_buffer(req);
    assert(evb);
    evbuffer_add(evb, strReply.data(), strReply.size());
    HTTPEvent* ev = new HTTPEvent(eventBase, true,
        boost::bind(evhttp_send_reply, req, nStatus, (const char*)NULL, (struct evbuffer *)NULL));
    ev->trigger(0);
    replySent = true;
    req = 0; // transferred back to main thread
}

CService HTTPRequest::GetPeer()
{
    evhttp_connection* con = evhttp_request_get_connection(req);
    CService peer;
    if (con) {
        // evhttp retains ownership over returned address string
        const char* address = "";
        uint16_t port = 0;
        evhttp_connection_get_peer(con, (char**)&address, &port); // 从 HTTP 连接中获取对方 IP 和 PORT
        peer = CService(address, port);
    }
    return peer;
}

std::string HTTPRequest::GetURI()
{
    return evhttp_request_get_uri(req);
}

HTTPRequest::RequestMethod HTTPRequest::GetRequestMethod()
{
    switch (evhttp_request_get_command(req)) {
    case EVHTTP_REQ_GET:
        return GET;
        break;
    case EVHTTP_REQ_POST:
        return POST;
        break;
    case EVHTTP_REQ_HEAD:
        return HEAD;
        break;
    case EVHTTP_REQ_PUT:
        return PUT;
        break;
    default:
        return UNKNOWN;
        break;
    }
}

void RegisterHTTPHandler(const std::string &prefix, bool exactMatch, const HTTPRequestHandler &handler)
{
    LogPrint("http", "Registering HTTP handler for %s (exactmatch %d)\n", prefix, exactMatch);
    pathHandlers.push_back(HTTPPathHandler(prefix, exactMatch, handler)); // 加入处理函数列表
}

void UnregisterHTTPHandler(const std::string &prefix, bool exactMatch)
{
    std::vector<HTTPPathHandler>::iterator i = pathHandlers.begin();
    std::vector<HTTPPathHandler>::iterator iend = pathHandlers.end();
    for (; i != iend; ++i)
        if (i->prefix == prefix && i->exactMatch == exactMatch)
            break;
    if (i != iend)
    {
        LogPrint("http", "Unregistering HTTP handler for %s (exactmatch %d)\n", prefix, exactMatch);
        pathHandlers.erase(i);
    }
}

