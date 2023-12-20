#include <glog/logging.h>

#include "timer.h"

namespace core {

TimerEvent::TimerEvent(
    int delay,
    std::shared_ptr<AsyncExecutor> executor,
    std::shared_ptr<TimerQueue> timer_queue)
  : m_executor(std::move(executor))
  , m_request_queue(std::move(timer_queue))
  , m_cancelled(false)
  , m_delay(delay) {
  SetDeadline(delay);
}

TimerEvent::~TimerEvent() {
}

TimerEvent::time_point TimerEvent::Deadline() const {
  return m_deadline;
}

void TimerEvent::SetDeadline(int delay) {
  m_deadline = clock_type::now() + milliseconds(delay);
}

void TimerEvent::Cancel() {
    m_cancelled.store(true);
}

bool TimerEvent::Cancelled() const {
    return m_cancelled.load();
}

int TimerEvent::Delay() const {
  return m_delay;
}

void TimerEvent::SetDelay(int delay) {
  m_delay = delay;
}

void TimerEvent::Reset() {
  m_cancelled.store(false);
}

bool TimerEvent::Expired(time_point time) const {
  return m_deadline <= time; 
}

bool TimerEvent::Comparator(const std::shared_ptr<TimerEvent>& lhs, const std::shared_ptr<TimerEvent>& rhs) {
  return lhs->Deadline() < rhs->Deadline();
}

std::shared_ptr<TimerQueue> TimerEvent::TimerQueueInstance() const {
  return m_request_queue;
}

std::shared_ptr<AsyncExecutor> TimerEvent::ExecutorInstance() const {
  return m_executor;
}

TimerCallbackEvent::TimerCallbackEvent(
    int delay,
    std::shared_ptr<AsyncExecutor> executor,
    std::shared_ptr<TimerQueue> timer_queue,
    std::function<void()>&& callback)
  : TimerEvent(delay, std::move(executor), std::move(timer_queue))
  , m_callback(std::move(callback)) {
}

void TimerCallbackEvent::Execute() {
  if (Cancelled()) {
    return;
  }

  m_executor->Enqueue(m_callback);
}

void TimerCallbackEvent::SetCallback(std::function<void()>&& callback) {
  m_callback = std::move(callback);
}

DeadlineTimer::DeadlineTimer(std::shared_ptr<TimerEvent> ctx)
  : m_timer_ctx(std::move(ctx)) {
}

DeadlineTimer::~DeadlineTimer() {
}

DeadlineTimerImpl::DeadlineTimerImpl(std::shared_ptr<TimerEvent> ctx)
  : DeadlineTimer(ctx) {
}

DeadlineTimerImpl::~DeadlineTimerImpl() {
  Cancel();
}

void DeadlineTimerImpl::Cancel() {
  m_timer_ctx->Cancel();
  m_timer_ctx->TimerQueueInstance()->RemoveTimer(m_timer_ctx);
}

void DeadlineTimerImpl::Reset() {
  auto timer_queue = m_timer_ctx->TimerQueueInstance();
  timer_queue->RemoveTimer(m_timer_ctx);
  m_timer_ctx->SetDeadline(m_timer_ctx->Delay());
  m_timer_ctx->Reset();
  timer_queue->AddTimer(m_timer_ctx);
}

void DeadlineTimerImpl::Reset(int delay) {
  m_timer_ctx->SetDelay(delay);
  Reset();
}

void DeadlineTimerImpl::Reset(std::function<void()>&& callback, int delay) {
  m_timer_ctx->SetCallback(std::move(callback));
  Reset(delay);
}

TimerQueue::TimerQueue()
  : m_abort(false), m_idle(true) {
}

TimerQueue::~TimerQueue() {
  Shutdown();
}

std::shared_ptr<DeadlineTimer> TimerQueue::CreateTimer(
    int delay,
    std::shared_ptr<AsyncExecutor> executor,
    std::function<void()>&& callback) {
  auto ctx = std::make_shared<TimerCallbackEvent>(
      delay, std::move(executor), shared_from_this(), std::move(callback));
  auto new_timer = std::make_shared<DeadlineTimerImpl>(ctx);

  return new_timer;
}

void TimerQueue::AddTimer(std::shared_ptr<TimerEvent> timer) {
  std::unique_lock<std::mutex> lock(m_lock);

  if (m_abort) {
    LOG(FATAL) << "Unable to schedule task, timer queue process was aborted";
  }

  auto prev_thread = UpdateWorkerThread(lock);
  
  m_request_queue.push_back({std::move(timer), TimerEventType::ADD});
  lock.unlock();

  m_cond.notify_one();

  if (prev_thread.joinable()) {
    prev_thread.join();
  }
}

void TimerQueue::RemoveTimer(std::shared_ptr<TimerEvent> timer) {
  std::unique_lock<std::mutex> lock(m_lock);

  m_request_queue.push_back({std::move(timer), TimerEventType::REMOVE});
  lock.unlock();

  m_cond.notify_one();
}

void TimerQueue::Shutdown() {
  auto prev_state = m_abort.exchange(true);
  if (prev_state) {
    return;
  }

  std::unique_lock<std::mutex> lock(m_lock);
  if (!m_worker.joinable()) {
    return;
  }

  m_request_queue.clear();
  lock.unlock();

  m_cond.notify_all();
  m_worker.join();
}

void TimerQueue::EventLoop() {
  TimerEvent::time_point next_deadline;

  while (true) {
    std::unique_lock<std::mutex> lock(m_lock);
    if (m_process_queue.empty()) {
      auto result = m_cond.wait_for(lock, std::chrono::seconds(5), [this] {
        return !m_request_queue.empty() || m_abort.load();
      });

      if (!result) {
        m_idle = true;
        return;
      }
    } else {
      m_cond.wait_until(lock, next_deadline, [this] {
        return !m_request_queue.empty() || m_abort.load();
      });
    }

    if (m_abort.load()) {
      return;
    }

    auto request_queue = std::move(m_request_queue);
    lock.unlock();

    next_deadline = ProcessTimers(request_queue);
  }
}

TimerEvent::time_point TimerQueue::ProcessTimers(event_queue& events) {
  for (auto& event:events) {
    auto& timer = event.first;
    auto event_type = event.second;

    if (event_type == TimerEventType::ADD) {
      AddTimerInternal(std::move(timer));
    } else {
      RemoveTimerInternal(std::move(timer));
    }
  }

  const auto curr_time = TimerEvent::clock_type::now();
  timer_set temp_set;

  while (true) {
    if (m_process_queue.empty()) {
      break;
    }

    auto timer_it = m_process_queue.begin();
    auto timer_ptr = *timer_it;
    if (!timer_ptr->Expired(curr_time)) {
      break;
    }

    auto node = m_process_queue.extract(timer_it);
    auto temp_it = temp_set.insert(std::move(node));

    const auto cancelled = timer_ptr->Cancelled();
    if (!cancelled) {
      timer_ptr->Execute();
      continue;
    }
  }

  if (m_process_queue.empty()) {
    return curr_time + std::chrono::seconds(5);
  }

  return (*m_process_queue.begin())->Deadline();
}

void TimerQueue::AddTimerInternal(std::shared_ptr<TimerEvent> new_timer) {
  m_process_queue.emplace(std::move(new_timer));
}

void TimerQueue::RemoveTimerInternal(std::shared_ptr<TimerEvent> existing_timer) {
  auto timer_it = m_process_queue.find(existing_timer);
  if (timer_it == m_process_queue.end()) {
    return;
  }
  m_process_queue.erase(timer_it);
}

std::thread TimerQueue::UpdateWorkerThread(std::unique_lock<std::mutex>& lock) {
  assert(lock.owns_lock());
  if (!m_idle) {
    return {};
  }

  auto prev_worker = std::move(m_worker);
  m_worker = std::thread([this] {
      EventLoop();
  });

  m_idle = false;
  return prev_worker;
}

bool TimerQueue::Empty() const {
  return m_request_queue.empty();
}

}

