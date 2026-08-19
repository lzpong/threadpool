#pragma once
#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <deque>
#include <atomic>
#include <future>
#include <stdexcept>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <unordered_set>

namespace threadpool
{
//线程池最大容量(可在包含本头文件前 #define THREADPOOL_MAX_NUM 覆盖)
#ifndef THREADPOOL_MAX_NUM
#define  THREADPOOL_MAX_NUM 16
#endif
//启用自动增长(不超过 THREADPOOL_MAX_NUM)(可在包含本头文件前 #define THREADPOOL_AUTO_GROW 开启)
//#define  THREADPOOL_AUTO_GROW

//线程池: 支持提交变参函数/lambda, 可获取返回值(future)
//支持类静态成员函数/全局函数, 不直接支持类成员函数(可用bind/mem_fn适配)
//使用约束:
//  1. 禁止任务内同步等待提交到本池的子任务(会死锁)
//  2. 析构期间不得并发调用commit/commit2
//  3. 队列满时commit阻塞(背压), 防止无界增长
class threadpool
{
	unsigned short _initSize;           //初始化线程数量
	using Task = std::function<void()>; //定义类型
	std::vector<std::thread> _pool;     //线程池
	std::deque<Task> _tasks;            //任务队列(有界, 背压控制)
	std::mutex _lock;                   //任务队列同步锁, 保护_tasks/_exitedIds
	std::condition_variable _task_cv;   //条件阻塞(任务到来/停止)
	std::condition_variable _spaceCv;   //条件阻塞(队列有空位, 唤醒生产者)
	unsigned int _maxQueueSize;         //任务队列最大容量
	std::atomic<bool> _run{ true };     //线程池是否执行
	std::atomic<int>  _idlThrNum{ 0 };  //空闲线程数量
#ifdef THREADPOOL_AUTO_GROW
	std::mutex _lockGrow;                  //线程池增长同步锁, 保护_pool
	std::vector<std::thread::id> _exitedIds; //已退出线程ID(受_lock保护)
	std::atomic<bool> _hasExited{ false };   //是否有已退出线程待清理(避免每次commit都try_lock)
	std::atomic<int>  _activeThrNum{ 0 };    //活跃线程数量(替代_pool.size()做逻辑判断)
#endif // !THREADPOOL_AUTO_GROW

public:
	inline threadpool(unsigned short size = 4, unsigned int maxQueueSize = 1024) {
		if (size == 0)
			throw std::invalid_argument("threadpool size must be >= 1");
		_initSize = size;
		_maxQueueSize = (maxQueueSize < 2) ? 2 : maxQueueSize;
		try {
			addThread(size);
		} catch (...) {
			// 构造失败: 停止线程池 + 等待已创建线程退出, 防止泄漏
			_run = false;
			_task_cv.notify_all();
			_spaceCv.notify_all();
			for (auto& t : _pool) {
				if (t.joinable()) t.join();
			}
			throw; // 重新抛出原始异常
		}
	}
	inline ~threadpool()
	{
		{
			std::lock_guard<std::mutex> lock{ _lock };
			_run = false;
		}
		_task_cv.notify_all();  // 唤醒所有工作线程退出
		_spaceCv.notify_all();  // 唤醒所有阻塞的生产者(它们将因!_run抛异常退出)
#ifdef THREADPOOL_AUTO_GROW
		// 先清理已退出的线程(释放内存), 再join剩余活跃线程
		// 析构时无并发addThread, 但仍需_lockGrow防止与addThread竞争_pool操作
		{
			std::lock_guard<std::mutex> lockGrow{ _lockGrow };
			_cleanupExitedThreads_nolock();
		}
#endif // !THREADPOOL_AUTO_GROW
		for (std::thread& thread : _pool) {
			if (thread.joinable())
				thread.join(); // 等待线程退出
		}
	}

public:
	// 提交一个任务, 返回future
	// 调用.get()获取返回值会等待任务执行完,获取返回值
	// 有两种方法可以实现调用类成员，
	// 一种是使用   bind： .commit(std::bind(&Dog::sayHello, &dog));
	// 一种是用   mem_fn： .commit(std::mem_fn(&Dog::sayHello), this)
	template<class F, class... Args>
	auto commit(F&& f, Args&&... args) -> std::future<decltype(f(args...))>
	{
		using RetType = decltype(f(args...)); // 函数 f 的返回值类型
		auto task = std::make_shared<std::packaged_task<RetType()>>(
			std::bind(std::forward<F>(f), std::forward<Args>(args)...)
		); // 把函数入口及参数,打包(绑定)
		std::future<RetType> future = task->get_future();
#ifdef THREADPOOL_AUTO_GROW
		int needGrow = 0; // 预计需要新增的线程数(供AUTO_GROW精确扩容)
#endif // !THREADPOOL_AUTO_GROW
		{    // 添加任务到队列(检查_run和入队在同一锁内,防止与析构竞态)
			std::unique_lock<std::mutex> lock{ _lock };
			if (!_run)    // 已停止
				throw std::runtime_error("commit on ThreadPool is stopped.");
			// 有界队列背压: 队列满时阻塞生产者, 直到有空位或被停止
			_spaceCv.wait(lock, [this] {
				return !_run || _tasks.size() < _maxQueueSize;
			});
			if (!_run)    // 等待期间线程池已停止
				throw std::runtime_error("commit on ThreadPool is stopped.");
			_tasks.emplace_back([task]() { // 放到队列后面
				(*task)();
			});
#ifdef THREADPOOL_AUTO_GROW
			// 计算需要新增的线程数(队列任务数 - 空闲线程数), 锁内读取保证一致性快照
			needGrow = (int)_tasks.size() - _idlThrNum.load();
#endif // !THREADPOOL_AUTO_GROW
		}
#ifdef THREADPOOL_AUTO_GROW
		if (needGrow > 0 && _activeThrNum < THREADPOOL_MAX_NUM)
			addThread((unsigned short)needGrow, true); // 精确扩容, addThread内部复核实际需求
		else if (_hasExited)
			_cleanupIfNeeded(); // 非增长路径也顺便清理已退出线程, 释放内存
#endif // !THREADPOOL_AUTO_GROW
		_task_cv.notify_one(); // 唤醒一个线程执行

		return future;
	}
	// 提交一无参任务, 无返回值
	template <class F>
	void commit2(F&& task)
	{
#ifdef THREADPOOL_AUTO_GROW
		int needGrow = 0;
#endif // !THREADPOOL_AUTO_GROW
		{
			std::unique_lock<std::mutex> lock{ _lock };
			if (!_run)
				throw std::runtime_error("commit2 on ThreadPool is stopped.");
			_spaceCv.wait(lock, [this] {
				return !_run || _tasks.size() < _maxQueueSize;
			});
			if (!_run)
				throw std::runtime_error("commit2 on ThreadPool is stopped.");
			_tasks.emplace_back(std::forward<F>(task));
#ifdef THREADPOOL_AUTO_GROW
			needGrow = (int)_tasks.size() - _idlThrNum.load();
#endif // !THREADPOOL_AUTO_GROW
		}
#ifdef THREADPOOL_AUTO_GROW
		if (needGrow > 0 && _activeThrNum < THREADPOOL_MAX_NUM)
			addThread((unsigned short)needGrow, true);
		else if (_hasExited)
			_cleanupIfNeeded();
#endif // !THREADPOOL_AUTO_GROW
		_task_cv.notify_one();
	}
	//空闲线程数量(瞬时快照, 仅参考)
	int idlCount() { return _idlThrNum; }
	//线程数量(瞬时快照, 仅参考)
#ifdef THREADPOOL_AUTO_GROW
	int thrCount() { return _activeThrNum; }
#else
	int thrCount() { return _pool.size(); }
#endif // !THREADPOOL_AUTO_GROW
	//任务队列长度(瞬时快照, 仅参考)
	unsigned int queueSize() {
		std::lock_guard<std::mutex> lock{ _lock };
		return (unsigned int)_tasks.size();
	}

#ifndef THREADPOOL_AUTO_GROW
private:
#endif // !THREADPOOL_AUTO_GROW
#ifdef THREADPOOL_AUTO_GROW
	// 清理已退出线程(join + erase), 调用者必须持有 _lockGrow
	void _cleanupExitedThreads_nolock()
	{
		std::unordered_set<std::thread::id> ids;
		{
			std::lock_guard<std::mutex> lock{ _lock };
			ids.insert(_exitedIds.begin(), _exitedIds.end()); // 一次性取出, 减少持锁时间
			_exitedIds.clear();
			_hasExited = false;   // 已全部取出, 清除标记(如有新退出线程会重新置位)
		}
		if (ids.empty()) return;
		for (auto it = _pool.begin(); it != _pool.end(); ) {
			if (it->joinable() && ids.count(it->get_id())) { // O(1)查找
				it->join(); // 线程已退出, join立即返回
				it = _pool.erase(it);
				continue;
			}
			++it;
		}
	}
	// 非增长路径下尝试清理已退出线程(try_lock避免阻塞commit调用者)
	// 锁顺序: _lockGrow → _lock, 与addThread一致, 防止死锁
	void _cleanupIfNeeded()
	{
		// try_lock: 如果有其他线程在addThread(持有_lockGrow), 直接跳过不等待
		std::unique_lock<std::mutex> lockGrow{ _lockGrow, std::try_to_lock };
		if (!lockGrow.owns_lock()) return;
		_cleanupExitedThreads_nolock(); // 内部会获取_lock检查_exitedIds
	}
#endif // !THREADPOOL_AUTO_GROW

	//添加线程, needBased=true时按需创建(队列任务数>空闲线程数)
	void addThread(unsigned short size, bool needBased = false)
	{
		auto worker = [this] { //工作线程函数
			while (true) //防止 _run==false 时立即结束,此时任务队列可能不为空
			{
				Task task; // 获取一个待执行的 task
				{
					// unique_lock 相比 lock_guard 的好处是：可以随时 unlock() 和 lock()
					std::unique_lock<std::mutex> lock{ _lock };
					_task_cv.wait(lock, [this] { // wait 直到有 task, 或需要停止
						return !_run || !_tasks.empty();
					});
					if (!_run && _tasks.empty())
						return;
					task = std::move(_tasks.front()); // 按先进先出从队列取一个 task
					_tasks.pop_front();
					_idlThrNum--;
				}
				_spaceCv.notify_one(); // 队列有空位, 唤醒一个阻塞的生产者(锁外通知, 避免唤醒后抢锁) 
				try {
					task();//执行任务
				} catch (...) {
					// 用户任务抛异常时不影响线程池, 异常被packaged_task捕获, 通过future传播给调用者; 此处仅保证_idlThrNum正确恢复
				}
#ifdef THREADPOOL_AUTO_GROW
				if (_idlThrNum.load()>0 && _activeThrNum > _initSize) { //支持自动释放空闲线程,避免峰值过后大量空闲线程
					// 退出前: 记录ID + 标记有退出线程
					{
						std::lock_guard<std::mutex> lock{ _lock };
						_exitedIds.push_back(std::this_thread::get_id());
						_hasExited = true;
					}
					_activeThrNum--;
					return;
				}
#endif // !THREADPOOL_AUTO_GROW
				_idlThrNum++; // 任务完成: 忙碌→空闲(atomic RMW, 无需_lock)
			}
		};
#ifdef THREADPOOL_AUTO_GROW
		std::unique_lock<std::mutex> lockGrow{ _lockGrow };
		{
			std::lock_guard<std::mutex> lock{ _lock };
			if (!_run)
				throw std::runtime_error("Grow on ThreadPool is stopped.");
		}
		// 先清理已退出的线程, 释放内存
		_cleanupExitedThreads_nolock();

		while (size > 0)
		{
			bool needCreate = true;
			{
				std::lock_guard<std::mutex> lock{ _lock };
				if (needBased && _idlThrNum.load() >= (int)_tasks.size())
					needCreate = false; // 需求不足(队列任务数<=空闲线程数), 不创建
				if (_activeThrNum >= THREADPOOL_MAX_NUM)
					needCreate = false; // 已达线程数上限
			}
			if (!needCreate) break;
			_pool.emplace_back(worker); //增加线程数量,但不超过 预定义数量 THREADPOOL_MAX_NUM
			_activeThrNum++;
			_idlThrNum++; // 新建线程空闲(atomic RMW, 无需_lock)
			--size;
		}
#else
		(void)needBased; // 非自动增长模式无需求判断
		for (; _pool.size() < THREADPOOL_MAX_NUM && size > 0; --size)
		{
			_pool.emplace_back(worker); //增加线程数量,但不超过 预定义数量 THREADPOOL_MAX_NUM
			_idlThrNum++; // 新建线程空闲(atomic RMW, 无需_lock)
		}
#endif // !THREADPOOL_AUTO_GROW
	}
};

}

#endif  // THREAD_POOL_H  //https://github.com/lzpong/
