#pragma once

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class gThreadPool
{
public:
	explicit gThreadPool(size_t workerCount)
	{
		workerCount = std::max<size_t>(1, workerCount);
		for (size_t i = 0; i < workerCount; ++i)
		{
			m_workers.emplace_back([this]()
			{
				for (;;)
				{
					std::function<void()> task;
					{
						std::unique_lock<std::mutex> lock(m_mutex);
						m_ready.wait(lock, [this]() { return m_stopping || !m_tasks.empty(); });
						if (m_stopping && m_tasks.empty())
							return;
						task = std::move(m_tasks.front());
						m_tasks.pop();
					}
					task();
				}
			});
		}
	}

	~gThreadPool()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stopping = true;
		}
		m_ready.notify_all();
		for (auto& worker : m_workers)
		{
			if (worker.joinable())
				worker.join();
		}
	}

	gThreadPool(const gThreadPool&) = delete;
	gThreadPool& operator=(const gThreadPool&) = delete;

	template<typename Fn>
	auto addJob(Fn&& fn) -> std::future<typename std::invoke_result_t<Fn>>
	{
		using Result = typename std::invoke_result_t<Fn>;
		auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
		std::future<Result> result = task->get_future();
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_stopping)
				throw std::runtime_error("thread pool is stopped");
			m_tasks.emplace([task]() { (*task)(); });
		}
		m_ready.notify_one();
		return result;
	}

private:
	std::vector<std::thread> m_workers;
	std::queue<std::function<void()>> m_tasks;
	std::mutex m_mutex;
	std::condition_variable m_ready;
	bool m_stopping = false;
};
