#include "base/logging.h"
#include "thread/threadpool.h"
#include "thread/threadutil.h"

///////////////////////////// WorkerThread

WorkerThread::WorkerThread() {
	thread.reset(new std::thread(std::bind(&WorkerThread::WorkFunc, this)));
}

WorkerThread::~WorkerThread() {
	{
		std::lock_guard<std::mutex> guard(mutex);
		active = false;
		signal.notify_one();
	}
	thread->join();
}

void WorkerThread::Process(std::function<void()> work) {
	std::lock_guard<std::mutex> guard(mutex);
	work_ = std::move(work);
	jobsTarget = jobsDone + 1;
	signal.notify_one();
}

void WorkerThread::WaitForCompletion() {
	std::unique_lock<std::mutex> guard(doneMutex);
	while (jobsDone < jobsTarget) {
		done.wait(guard);
	}
}

void WorkerThread::WorkFunc() {
	setCurrentThreadName("Worker");
	std::unique_lock<std::mutex> guard(mutex);
	while (active) {
		// 'active == false' is one of the conditions for signaling,
		// do not "optimize" it
		while (active && jobsTarget <= jobsDone) {
			signal.wait(guard);
		}
		if (active) {
			work_();

			std::lock_guard<std::mutex> doneGuard(doneMutex);
			jobsDone++;
			done.notify_one();
		}
	}
}

LoopWorkerThread::LoopWorkerThread() : WorkerThread(true) {
	thread.reset(new std::thread(std::bind(&LoopWorkerThread::WorkFunc, this)));
}

void LoopWorkerThread::Process(std::function<void(int, int)> work, int start, int end) {
	std::lock_guard<std::mutex> guard(mutex);
	work_ = std::move(work);
	start_ = start;
	end_ = end;
	jobsTarget = jobsDone + 1;
	signal.notify_one();
}

void LoopWorkerThread::WorkFunc() {
	setCurrentThreadName("LoopWorker");
	std::unique_lock<std::mutex> guard(mutex);
	while (active) {
		// 'active == false' is one of the conditions for signaling,
		// do not "optimize" it
		while (active && jobsTarget <= jobsDone) {
			signal.wait(guard);
		}
		if (active) {
			work_(start_, end_);

			std::lock_guard<std::mutex> doneGuard(doneMutex);
			jobsDone++;
			done.notify_one();
		}
	}
}

///////////////////////////// ThreadPool

ThreadPool::ThreadPool(int numThreads) {
	if (numThreads <= 0) {
		numThreads_ = 1;
		ILOG("ThreadPool: Bad number of threads %i", numThreads);
	} else if (numThreads > 8) {
		ILOG("ThreadPool: Capping number of threads to 8 (was %i)", numThreads);
		numThreads_ = 8;
	} else {
		numThreads_ = numThreads;
	}
}

void ThreadPool::StartWorkers() {
	if (!workersStarted) {
		for(int i = 0; i < numThreads_; ++i) {
			workers.push_back(std::make_shared<LoopWorkerThread>());
		}
		workersStarted = true;
	}
}

void ThreadPool::ParallelLoop(const std::function<void(int,int)> &loop, int lower, int upper) {
	int range = upper - lower;
	if (range >= numThreads_ * 2) { // don't parallelize tiny loops (this could be better, maybe add optional parameter that estimates work per iteration)
		std::lock_guard<std::mutex> guard(mutex);
		StartWorkers();

		// could do slightly better load balancing for the generic case, 
		// but doesn't matter since all our loops are power of 2
		int chunk = range / numThreads_;
		int s = lower;
		for (int i = 0; i < numThreads_ - 1; ++i) {
			workers[i]->Process(loop, s, s+chunk);
			s+=chunk;
		}
		// This is the final chunk.
		loop(s, upper);
		for (int i = 0; i < numThreads_ - 1; ++i) {
			workers[i]->WaitForCompletion();
		}
	} else {
		loop(lower, upper);
	}
}

