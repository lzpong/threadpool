# threadpool
**threadpool** 管理一个任务队列和一个线程队列，每次从队列取一个任务分配给一个线程执行，循环往复。当限制只创建一个线程时，就是一个完全的任务队列。支持提交变参函数或 lambda 表达式，可获取执行返回值（future）。

> **目前已重构优化**：特别是针对自增长的线程池，优化了自动扩容和空闲线程回收的功能。

**拥有以下特性：**
- 基于 C++11 的线程池，代码简洁，仅 300 行左右。
- 由于可变参数模板，支持非固定参数数量、无数量限制。
- 有界任务队列 + 背压控制：队列满时 `commit` 阻塞生产者，防止任务无界增长。
- 支持自动扩容与空闲线程回收，避免峰值过后大量空闲线程（默认关闭，通过包含头文件前 `#define THREADPOOL_AUTO_GROW` 开启，最大线程数量仍受 `THREADPOOL_MAX_NUM` 限制）
- 异常安全：构造失败自动清理已创建线程；任务异常不影响线程池，通过 future 传播给调用者。
- 支持类静态成员函数/全局函数，不直接支持类成员函数（可用 `bind`/`mem_fn` 适配）。

## API
- 构造函数：`threadpool(size = 4, maxQueueSize = 1024)`
  - `size`：初始化线程数量（`>= 1`），**不会超过上限 `THREADPOOL_MAX_NUM`（默认=16），可通过包含头文件前 `#define THREADPOOL_MAX_NUM 128` 修改**
  - `maxQueueSize`：任务队列最大容量（`< 2` 时强制为 2），满时 `commit` 阻塞（背压）
- `auto commit(f, args...) -> std::future<Ret>`：提交任务，返回 future 获取返回值
- `void commit2(f)`：提交无返回值任务
- `int idlCount()`：空闲线程数量（瞬时快照，仅参考）
- `int thrCount()`：线程数量（瞬时快照，仅参考）
- `unsigned int queueSize()`：任务队列长度（瞬时快照，仅参考）

## 使用约束
1. 禁止任务内同步等待提交到本池的子任务（会死锁）。
2. 析构期间不得并发调用 `commit`/`commit2`。
3. 队列满时 `commit` 阻塞（背压），防止无界增长。

## C++11 语言细节
即使懂原理也不代表能写出程序，上面用了众多 c++11 的"奇技淫巧"，下面进行简单描述。

1. `using Task = std::function<void()>` 是类型别名，简化了 `typedef` 的用法。`std::function<void()>` 可以认为是一个函数类型，接受任意原型是 `void()` 的函数，或是函数对象，或是匿名函数。`void()` 意思是不带参数，没有返回值。
2. `pool.emplace_back([this]{...})` 和 `pool.push_back([this]{...})` 功能一样，只不过前者性能会更好；
3. `pool.emplace_back([this]{...})` 是构造了一个线程对象，执行函数是 lambda 匿名函数；
4. 所有对象的初始化方式均采用了 `{}`，而不再使用 `()` 方式，因为风格不够一致且容易出错；
5. 匿名函数： `[this]{...}` 不多说。`[]` 是捕捉器，`this` 是引用域外的变量 `this` 指针，内部使用死循环，由 `_task_cv.wait(lock,[this]{...})` 来阻塞线程；
6. `decltype(expr)` 用来推断 `expr` 的类型，和 `auto` 是类似的，相当于类型占位符，占据一个类型的位置；`auto f(A a, B b) -> decltype(a+b)` 是一种用法，不能写作 `decltype(a+b) f(A a, B b)`，为啥？！ c++ 就是这么规定的！
7. `commit` 方法是不是略奇葩！可以带任意多的参数，第一个参数是 `f`，后面依次是函数 `f` 的参数（*注意：参数要传 struct/class 的话，建议用 pointer，小心变量的作用域*）！可变参数模板是 c++11 的一大亮点，够亮！至于为什么是 `Arg...` 和 `arg...`，因为规定就是这么用的！
8. `commit` 直接使用智能调用 stdcall 函数，但有两种方法可以实现调用类成员：一种是使用 `bind`： `.commit(std::bind(&Dog::sayHello, &dog))`；一种是用 `mem_fn`： `.commit(std::mem_fn(&Dog::sayHello), this)`；
9. `make_shared` 用来构造 `shared_ptr` 智能指针。用法大体是 `shared_ptr<int> p = make_shared<int>(4)` 然后 `*p == 4`。智能指针的好处就是，自动 delete！
10. `bind` 函数，接受函数 `f` 和部分参数，返回 currying 后的匿名函数，譬如 `bind(add, 4)` 可以实现类似 `add4` 的函数！
11. `forward()` 函数，类似于 `move()` 函数，后者是将参数右值化，前者是… 大概意思就是：不改变最初传入的类型的引用类型（左值还是左值，右值还是右值）；
12. `packaged_task` 就是任务函数的封装类，通过 `get_future` 获取 `future`，然后通过 `future` 可以获取函数的返回值（`future.get()`）；`packaged_task` 本身可以像函数一样调用 `()`；
13. `deque` 是双端队列类，`front()` 获取头部元素，`pop_front()` 移除头部元素；`back()` 获取尾部元素，`push_back()` 尾部添加元素；
14. `lock_guard` 是 `mutex` 的 stack 封装类，构造的时候 `lock()`，析构的时候 `unlock()`，是 c++ RAII 的 idea；
15. `condition_variable cv;` 条件变量，需要配合 `unique_lock` 使用；`unique_lock` 相比 `lock_guard` 的好处是：可以随时 `unlock()` 和 `lock()`。`cv.wait()` 之前需要持有 mutex，wait 本身会 `unlock()` mutex，如果条件满足则会重新持有 mutex。
16. 最后线程池析构的时候，`join()` 可以等待任务都执行完再结束，很安全！

## 编译方式
Linux 系统下编译方式：
```shell
$ g++ Main.cpp -o main -lpthread # 编译方式
$ ./threadpool # 执行
```

Windows 系统下（Visual Studio 或 MinGW）直接编译 `Main.cpp` 即可，代码已兼容 `_WIN32`。

## 版权说明
代码是 me "写"的，但是思路来自 Internet，特别是 [这个线程池实现](https://github.com/progschj/ThreadPool)（基本 copy 了这个实现，加上[这位同学的实现](http://blog.csdn.net/zdarks/article/details/46994607)和解释，好东西值得 copy！* 然后综合更改了下，更加简洁）。
