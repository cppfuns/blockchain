// Copyright (c) 2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "scheduler.h"

#include "reverselock.h"

#include <assert.h>
#include <boost/bind.hpp>
#include <utility>

CScheduler::CScheduler() : nThreadsServicingQueue(0), stopRequested(false), stopWhenEmpty(false)
{
}

CScheduler::~CScheduler()
{
    assert(nThreadsServicingQueue == 0);
}


#if BOOST_VERSION < 105000
static boost::system_time toPosixTime(const boost::chrono::system_clock::time_point& t)
{
    return boost::posix_time::from_time_t(boost::chrono::system_clock::to_time_t(t));
}
#endif

void CScheduler::serviceQueue()
{
    boost::unique_lock<boost::mutex> lock(newTaskMutex); // 上锁，保证函数线程安全
    ++nThreadsServicingQueue; // 使用队列的线程数加 1

    // newTaskMutex is locked throughout this loop EXCEPT
    // when the thread is waiting or when the user's function
    // is called.
    while (!shouldStop()) { // loop
        try {
            while (!shouldStop() && taskQueue.empty()) { // 任务队列为空
                // Wait until there is something to do.
                newTaskScheduled.wait(lock); // 等待条件满足
            }

            // Wait until either there is a new task, or until
            // the time of the first item on the queue:

// wait_until needs boost 1.50 or later; older versions have timed_wait:
#if BOOST_VERSION < 105000 // 任务队列非空
            while (!shouldStop() && !taskQueue.empty() &&
                   newTaskScheduled.timed_wait(lock, toPosixTime(taskQueue.begin()->first))) { // 获取新任务的 key（时间），进行等待
                // Keep waiting until timeout
            }
#else
            // Some boost versions have a conflicting overload of wait_until that returns void.
            // Explicitly use a template here to avoid hitting that overload.
            while (!shouldStop() && !taskQueue.empty() &&
                   newTaskScheduled.wait_until<>(lock, taskQueue.begin()->first) != boost::cv_status::timeout) { // 105000 之后的 boost 版本
                // Keep waiting until timeout
            }
#endif
            // If there are multiple threads, the queue can empty while we're waiting (another
            // thread may service the task we were waiting on).
            if (shouldStop() || taskQueue.empty())
                continue;

            Function f = taskQueue.begin()->second; // 获取队列中第一个任务
            taskQueue.erase(taskQueue.begin()); // 清除该任务

            {
                // Unlock before calling f, so it can reschedule itself or another task
                // without deadlocking:
                reverse_lock<boost::unique_lock<boost::mutex> > rlock(lock); // 在调用 f 前解锁，防止死锁
                f(); // 执行任务
            }
        } catch (...) {
            --nThreadsServicingQueue;
            throw;
        }
    } // end of loop
    --nThreadsServicingQueue; // 使用任务队列的线程数减 1
}

void CScheduler::stop(bool drain)
{
    {
        boost::unique_lock<boost::mutex> lock(newTaskMutex);
        if (drain)
            stopWhenEmpty = true;
        else
            stopRequested = true;
    }
    newTaskScheduled.notify_all();
}

void CScheduler::schedule(CScheduler::Function f, boost::chrono::system_clock::time_point t)
{
    {
        boost::unique_lock<boost::mutex> lock(newTaskMutex);
        taskQueue.insert(std::make_pair(t, f));
    }
    newTaskScheduled.notify_one();
}

void CScheduler::scheduleFromNow(CScheduler::Function f, int64_t deltaSeconds)
{
    schedule(f, boost::chrono::system_clock::now() + boost::chrono::seconds(deltaSeconds));
}

static void Repeat(CScheduler* s, CScheduler::Function f, int64_t deltaSeconds)
{
    f();
    s->scheduleFromNow(boost::bind(&Repeat, s, f, deltaSeconds), deltaSeconds);
}

void CScheduler::scheduleEvery(CScheduler::Function f, int64_t deltaSeconds)
{
    scheduleFromNow(boost::bind(&Repeat, this, f, deltaSeconds), deltaSeconds);
}

size_t CScheduler::getQueueInfo(boost::chrono::system_clock::time_point &first,
                             boost::chrono::system_clock::time_point &last) const
{
    boost::unique_lock<boost::mutex> lock(newTaskMutex);
    size_t result = taskQueue.size();
    if (!taskQueue.empty()) {
        first = taskQueue.begin()->first;
        last = taskQueue.rbegin()->first;
    }
    return result;
}
