#include "asyncloop.h"
#include "asyncpool.h"

using namespace openset::async;

AsyncLoop::AsyncLoop(AsyncPool* asyncPool, const int partitionId, const int workerId) :
    queueSize(0),
    loopCount(0),
    asyncPool(asyncPool),
    runTime(100),
    partition(partitionId),
    worker(workerId)
{}

AsyncLoop::~AsyncLoop()
{
    release();
}

void AsyncLoop::release()
{
    csLock lock(pendLock);

    while (queued.size())
    {
        queued.back()->partitionRemoved();
        delete queued.back();
        queued.pop_back();
    }

    while (active.size())
    {
        // we are force removing, this member can be over-ridden to
        // allow for graceful error handling (i.e. incomplete shuttle calls)
        active.back()->partitionRemoved();
        delete active.back();
        active.pop_back();
    }
}

// uses locks - may be called from other threads
void AsyncLoop::queueCell(OpenLoop* work)
{
    {
        csLock lock(pendLock);
        // assign this loop to the cell
        work->assignLoop(this);
        queued.push_back(work);
        ++queueSize;
    }

    // trigger and run immediately?
    asyncPool->workerInfo[worker].triggered = true;
    asyncPool->workerInfo[worker].conditional.notify_one();
}

void AsyncLoop::purgeByTable(const std::string& tableName)
{
    csLock lock(pendLock);

    vector<OpenLoop*> newActive;
    for (auto a: active)
        if (a->owningTable != tableName)
            newActive.push_back(a);
        else
            delete a;

    active = std::move(newActive);

    vector<OpenLoop*> newQueued;
    for (auto q: queued)
        if (q->owningTable != tableName)
            newQueued.push_back(q);
        else
            delete q;

    queued = std::move(newQueued);

}

// this will add any queued jobs to the
// active Loop. This is particularly useful
// because a job Cell can spawn more job cells
// and they will be ready queued on the next cycle
//
// Note, this is also where prepare is called, at this
// point 'loop' as been assigned.
void AsyncLoop::scheduleQueued()
{
    // call prepare on the cell to set it's state
    // outside the constructor, this happens in the partition thread
    csLock lock(pendLock);

    queueSize -= queued.size();
    active.insert(active.end(), make_move_iterator(queued.begin()), make_move_iterator(queued.end()));
    queued.clear();
}

// this runs one iteration of the main Loop
bool AsyncLoop::run(int64_t &nextRun)
{
    // actual number of worker cells that did anything
    auto runCount = 0;

    // inject any queued work
    if (queueSize)
        scheduleQueued();

    // nothing to do
    if (!active.size())
        return false;

    vector<OpenLoop*> rerun;
    rerun.reserve(active.size());

    // this is the inside of our open ended Loop
    // it will call each job that is ready to run
    for (auto w : active)
    {
        const auto now = Now();

        if (w->checkCondition() &&
            w->checkTimer(now) &&
            w->state == oloopState_e::running) // check - some cells will complete in prepare
        {
            if (!w->prepared)
            {
                w->prepare();
                w->prepared = true;

                // if the worker completed or terminated during
                // the prepare step, then do not run
                if (w->state == oloopState_e::done)
                {
                    delete w;
                    continue;
                }
            }

            w->runStart = now;

            // count runs that have asked for an immediate re-run (returned true)
            if (w->run())
                ++runCount;

            // look for next scheduled (future) run operation
            if (w->state == oloopState_e::running &&
                w->runAt > now && (nextRun == -1 || w->runAt < nextRun))
                nextRun = w->runAt;
        }

        if (w->state == oloopState_e::done)
            delete w;
        else
            rerun.push_back(w); // reschedule jobs that have more to do
    }

    // swap rerun queue to active queue
    active = std::move(rerun);

    // nothing to do
    return (!runCount) ? false : true;
}

