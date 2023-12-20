#ifndef ASYNC_EXECUTOR
#define ASYNC_EXECUTOR

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <queue>

namespace core {

class AsyncExecutor {
public:
  using callback_t = std::function<void()>;
public:
  AsyncExecutor();
  virtual ~AsyncExecutor();

  virtual void Enqueue(const callback_t callback) = 0;

  virtual void Shutdown() = 0;

protected:
  std::atomic<bool> m_abort;
  std::mutex m_lock;
  std::condition_variable m_cond;
  std::queue<callback_t> m_request_queue;
  std::queue<callback_t> m_process_queue;
};

class Strand : public AsyncExecutor {
public:
  Strand();
  ~Strand();

  void Enqueue(const callback_t callback) override;

  void Shutdown() override;

private:
  void EventLoop();
  void ProcessEvents();

  std::thread UpdateWorkerThread(std::unique_lock<std::mutex>& lock);

private:
  bool m_idle;
  std::thread m_worker;
};

class ThreadPoolExecutor : public AsyncExecutor, public std::enable_shared_from_this<ThreadPoolExecutor> {
private:
  class ThreadPoolWorker : public AsyncExecutor {
  public:
    ThreadPoolWorker(
        std::shared_ptr<ThreadPoolExecutor> executor,
        int index);
    ThreadPoolWorker(ThreadPoolWorker&& worker);

    void Enqueue(const callback_t callback) override;

    void Shutdown() override;

    bool Idle() const;
    void SetWorkerActivity(bool is_active);

  private:
    void EventLoop();
    void ProcessEvents();

    void BalanceWork();

    void UpdateWorkerThread(std::unique_lock<std::mutex>& lock);

  private:
    std::atomic<bool> m_idle;
    std::thread m_worker;
    int m_pool_index;
    std::shared_ptr<ThreadPoolExecutor> m_parent_pool;
  };

public:
  ThreadPoolExecutor(int pool_size);
  ~ThreadPoolExecutor();

  void Enqueue(const callback_t callback) override;

  void Shutdown() override;

  int PoolSize() const;

  ThreadPoolWorker& Worker(int index);

private:
  int FindIdleWorker() const;
  std::vector<int> FindIdleWorkers(int thread_count) const;

  int RoundRobinNext();

private:
  int m_pool_size;
  std::atomic<int> m_round_robin_index;
  std::vector<ThreadPoolWorker> m_workers;
};

}

#endif

