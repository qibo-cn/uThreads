/*******************************************************************************
 *     Copyright © 2015, 2016 Saman Barghi
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *******************************************************************************/

#include "kThread.h"
#include "BlockingSync.h"
#include <unistd.h>

std::atomic_uint kThread::totalNumberofKTs(0);

__thread kThread* kThread::currentKT = nullptr;
__thread IntrusiveList<uThread>* kThread::ktReadyQueue = nullptr;
__thread funcvoid2_t kThread::postSuspendFunc = nullptr;

/*
 * This is only called to create defaultKT
 */
kThread::kThread() :
        cv_flag(true), threadSelf() {
    localCluster = &Cluster::defaultCluster;
    initialize();
    initializeMainUT(true);
    uThread::initUT = uThread::createMainUT(Cluster::defaultCluster);
    /*
     * Since this is for defaultKT, current running uThread
     * is the initial one which is responsible for running
     * the main() function.
     */
    currentUT = uThread::initUT;
    initialSynchronization();
}
kThread::kThread(Cluster& cluster, std::function<void(ptr_t)> func, ptr_t args) :
        localCluster(&cluster), threadSelf(&kThread::runWithFunc, this, func,
                args) {
    initialSynchronization();
}

kThread::kThread(Cluster& cluster) :
        localCluster(&cluster), cv_flag(false), threadSelf(&kThread::run, this) {
    initialSynchronization();
}

kThread::~kThread() {
    totalNumberofKTs--;
    localCluster->numberOfkThreads--;

    //free thread local members
    //delete kThread::ktReadyQueue;
    //mainUT->destory(true);
}

void kThread::initialSynchronization() {
    //prevent overflow
    if(totalNumberofKTs + 1 > UINTMAX_MAX)
        exit(EXIT_FAILURE);
    totalNumberofKTs++;
    /*
     * Increase the number of kThreads in the cluster.
     * Since this is always < totalNumberofKTs will
     * not overflow.
     */
    localCluster->numberOfkThreads++;

    //set kernel thread variables
    threadID = (this == &defaultKT) ? std::this_thread::get_id() : this->threadSelf.get_id();
}

void kThread::run() {
    initialize();
    initializeMainUT(false);
    defaultRun(this);
}
void kThread::runWithFunc(std::function<void(ptr_t)> func, ptr_t args) {
    initialize();
    //There is no need for a mainUT to be created
    func(args);
}

void kThread::switchContext(uThread* ut, void* args) {
    assert(ut != nullptr);
    assert(ut->stackPointer != 0);
    stackSwitch(ut, args, &kThread::currentKT->currentUT->stackPointer,
            ut->stackPointer, postSwitchFunc);
}

void kThread::switchContext(void* args) {
    uThread* ut = nullptr;
    /*	First check the local queue */
    IntrusiveList<uThread>* ktrq = ktReadyQueue;
    if (!ktrq->empty()) {   //If not empty, grab a uThread and run it
        ut = ktrq->front();
        ktrq->pop_front();
    } else {                //If empty try to fill

        ssize_t res = localCluster->tryGetWorks(*ktrq);   //Try to fill the local queue
        if (res > 0) {       //If there is more work start using it
            ut = ktrq->front();
            ktrq->pop_front();
        } else {        //If no work is available, Switch to defaultUt
            if (res == 0 && kThread::currentKT->currentUT->state == uThread::State::YIELD)
                return; //if the running uThread yielded, continue running it
            ut = mainUT;
        }
    }
    assert(ut != nullptr);
    switchContext(ut, args);
}

void kThread::initialize() {
    /*
     * Set the thread_local pointer to this thread, later we can
     * find the executing thread by referring to this.
     */
    kThread::currentKT = this;

    kThread::ktReadyQueue = new IntrusiveList<uThread>();
}
void kThread::initializeMainUT(bool isDefaultKT) {
    /*
     * if defaultKT, then create a stack for mainUT.
     * kernel thread's stack is assigned to initUT.
     */
    if (slowpath(isDefaultKT)) {
        mainUT = uThread::create(defaultStackSize);
        /*
         * can't use mainUT->start as mainUT should not end up in
         * the Ready Queue. Thus, the stack pointer shoud be initiated
         * directly.
         */
        mainUT->stackPointer = (vaddr) stackInit(mainUT->stackPointer, (ptr_t) uThread::invoke,
                (ptr_t) kThread::defaultRun, (void*)this, nullptr, nullptr); //Initialize the thread context
        mainUT->state = uThread::State::READY;
    } else {
        /*
         * Default function takes up the default kernel thread's
         * stack pointer and run from there
         */
        mainUT = uThread::createMainUT(*localCluster);
        currentUT = mainUT;
        mainUT->state = uThread::State::RUNNING;
    }

    //Default uThreads are not being counted
    uThread::totalNumberofUTs--;
}

void kThread::defaultRun(void* args) {
    kThread* thisKT = (kThread*) args;
    uThread* ut = nullptr;

    while (true) {
        ssize_t res = thisKT->localCluster->getWork(*thisKT->ktReadyQueue);
        if(res ==0) continue;
        //ktReadyQueue should not be empty at this point
        assert(!ktReadyQueue->empty());
        ut = thisKT->ktReadyQueue->front();
        thisKT->ktReadyQueue->pop_front();
        //Switch to the new uThread
        thisKT->switchContext(ut, nullptr);
    }
}

void kThread::postSwitchFunc(uThread* nextuThread, void* args = nullptr) {

    kThread* ck = kThread::currentKT;
    //mainUT does not need to be managed here
    if (fastpath(ck->currentUT != kThread::currentKT->mainUT)) {
        switch (ck->currentUT->state) {
        case uThread::State::TERMINATED:
            ck->currentUT->destory(false);
            break;
        case uThread::State::YIELD:
            ck->currentUT->resume();
            ;
            break;
        case uThread::State::MIGRATE:
            ck->currentUT->resume();
            break;
        case uThread::State::WAITING: {
            //function and the argument should be set for pss
            assert(postSuspendFunc != nullptr);
            postSuspendFunc((void*)ck->currentUT, args);
            break;
        }
        default:
            break;
        }
    }
    //Change the current thread to the next
    ck->currentUT = nextuThread;
    nextuThread->state = uThread::State::RUNNING;
}

//TODO: How can I make this work for defaultKT?
std::thread::native_handle_type kThread::getThreadNativeHandle() {
    if(this != &defaultKT)
        return threadSelf.native_handle();
    else
        return 0;
}

std::thread::id kThread::getID() {
   return threadID;
}
