#pragma once
#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <atomic>
#include <future>
//#include <condition_variable>
//#include <thread>
//#include <functional>
#include <stdexcept>

namespace std
{
//线程池最大容量,应尽量设小一点
#define  THREADPOOL_MAX_NUM 16
//线程池是否可以自动增长(如果需要,且不超过 THREADPOOL_MAX_NUM)
//#define  THREADPOOL_AUTO_GROW

//线程池,可以提交变参函数或拉姆达表达式的匿名函数执行,可以获取执行返回值
//不直接支持类成员函数, 支持类静态成员函数或全局函数,Opteron()函数等
class threadpool
{
	unsigned short _initSize;       //初始化线程数量
	using Task = function<void()>; //定义类型
	vector<thread> _pool;          //线程池
	queue<Task> _tasks;            //任务队列
	mutex _lock;                   //任务队列同步锁
#ifdef THREADPOOL_AUTO_GROW
	mutex _lockGrow;               //线程池增长同步锁
#endif // !THREADPOOL_AUTO_GROW
	condition_variable _task_cv;   //条件阻塞
	atomic<bool> _run{ true };     //线程池是否执行
	atomic<int>  _idlThrNum{ 0 };  //空闲线程数量

public:
	inline threadpool(unsigned short size = 4) { _initSize = size; addThread(size); }
	inline ~threadpool()
	{
		_run=false;
		_task_cv.notify_all(); // 唤醒所有线程执行
		for (thread& thread : _pool) {
			//thread.detach(); // 让线程“自生自灭”
			if (thread.joinable())
				thread.join(); // 等待任务结束， 前提：线程一定会执行完
		}
	}

public:
	// 提交一个任务
	// 调用.get()获取返回值会等待任务执行完,获取返回值
	// 有两种方法可以实现调用类成员，
	// 一种是使用   bind： .commit(std::bind(&Dog::sayHello, &dog));
	// 一种是用   mem_fn： .commit(std::mem_fn(&Dog::sayHello), this)
	template<class F, class... Args>
	auto commit(F&& f, Args&&... args) -> future<decltype(f(args...))>
	{
		if (!_run)    // stoped ??
			throw runtime_error("commit on ThreadPool is stopped.");

		using RetType = decltype(f(args...)); // typename std::result_of<F(Args...)>::type, 函数 f 的返回值类型
		auto task = make_shared<packaged_task<RetType()>>(
			bind(forward<F>(f), forward<Args>(args)...)
		); // 把函数入口及参数,打包(绑定)
		future<RetType> future = task->get_future();
		{    // 添加任务到队列
			lock_guard<mutex> lock{ _lock };//对当前块的语句加锁  lock_guard 是 mutex 的 stack 封装类，构造的时候 lock()，析构的时候 unlock()
			_tasks.emplace([task]() { // push(Task{...}) 放到队列后面
				(*task)();
			});
		}
#ifdef THREADPOOL_AUTO_GROW
		if (_idlThrNum < 1 && _pool.size() < THREADPOOL_MAX_NUM)
			addThread(1);
#endif // !THREADPOOL_AUTO_GROW
		_task_cv.notify_one(); // 唤醒一个线程执行

		return future;
	}
	// 提交一个无参任务, 且无返回值
	template <class F>
	void commit2(F&& task)
	{
		if (!_run) return;
		{
			lock_guard<mutex> lock{ _lock };
			_tasks.emplace(std::forward<F>(task));
		}
#ifdef THREADPOOL_AUTO_GROW
		if (_idlThrNum < 1 && _pool.size() < THREADPOOL_MAX_NUM)
			addThread(1);
#endif // !THREADPOOL_AUTO_GROW
		_task_cv.notify_one();
	}
	//空闲线程数量
	int idlCount() { return _idlThrNum; }
	//线程数量
	int thrCount() { return _pool.size(); }

#ifndef THREADPOOL_AUTO_GROW
private:
#endif // !THREADPOOL_AUTO_GROW
	//添加指定数量的线程
	void addThread(unsigned short size)
	{
#ifdef THREADPOOL_AUTO_GROW
		if (!_run)    // stoped ??
			throw runtime_error("Grow on ThreadPool is stopped.");
		unique_lock<mutex> lockGrow{ _lockGrow }; //自动增长锁
#endif // !THREADPOOL_AUTO_GROW
		for (; _pool.size() < THREADPOOL_MAX_NUM && size > 0; --size)
		{   //增加线程数量,但不超过 预定义数量 THREADPOOL_MAX_NUM
			_pool.emplace_back( [this]{ //工作线程函数
				while (true) //防止 _run==false 时立即结束,此时任务队列可能不为空
				{
					Task task; // 获取一个待执行的 task
					{
						// unique_lock 相比 lock_guard 的好处是：可以随时 unlock() 和 lock()
						unique_lock<mutex> lock{ _lock };
						_task_cv.wait(lock, [this] { // wait 直到有 task, 或需要停止
							return !_run || !_tasks.empty();
						});
						if (!_run && _tasks.empty())
							return;
						_idlThrNum--;
						task = move(_tasks.front()); // 按先进先出从队列取一个 task
						_tasks.pop();
					}
					task();//执行任务
#ifdef THREADPOOL_AUTO_GROW
					if (_idlThrNum>0 && _pool.size() > _initSize) //支持自动释放空闲线程,避免峰值过后大量空闲线程
						return;
#endif // !THREADPOOL_AUTO_GROW
					{
						unique_lock<mutex> lock{ _lock };
						_idlThrNum++;
					}
				}
			});
			{
				unique_lock<mutex> lock{ _lock };
				_idlThrNum++;
			}
		}
	}
};

}

#endif  //https://github.com/lzpong/
