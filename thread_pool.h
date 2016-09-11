/*
 * mythread_pool.h
 *
 *  Created on: Sep 10, 2016
 *      Author: Jarvis
 */

#include <thread>
#include <queue>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <memory>
#include <atomic>
#include <algorithm>
#include <functional>

using namespace std;

template<typename T>
class Queue{
private:
	mutex mtx;
	queue<T> q;
	condition_variable cv;
	typedef const T& const_ref;
	typedef T& ref;

public:
	void push(const_ref val){
		unique_lock<mutex> lock(mtx);
		q.push(val);
		//cv.notify_all();
	}

	bool pop(ref val){
		unique_lock<mutex> lock(mtx);
		if(q.empty())
			return false;
		//cv.wait(lock,!q.empty());
		val = q.front();
		q.pop();
	}

	bool isempty(){
		unique_lock<mutex> lock(mtx);
		return q.empty();
	}
};

class mythread_pool{
public:
	mythread_pool(size_t size_ = 10):size(size_),isStop(false),isDone(false){
		resize(size_);
	}

	~mythread_pool(){
		isDone = true;
		{
			unique_lock<mutex> lock(mtx);
			cv.notify_all();
		}
		for_each(m_threads.begin(),m_threads.end(),
				[](decltype(*(m_threads.begin()))& item){
			if(item->joinable())
				item->join();
		});
		clear_queue();
		m_threads.clear();
	}

	template<typename F, typename... Args>
	auto push(F&& f, Args... args) ->future<decltype(f(0,args...))>{
		auto pck = make_shared<packaged_task<decltype(f(0,args...))(int)>>(
				std::bind(std::forward<F>(f), std::placeholders::_1, args...));
		auto _f = new function<void(int id)>(
				[pck](int id){
			(*pck)(id);
		});
		m_queue.push(_f);
		unique_lock<mutex> lock(mtx);
		cv.notify_one();
		return pck->get_future();
	}

	template<typename F>
	auto push(F&& f) ->future<decltype(f(0))>{
		auto pck = make_shared<packaged_task<decltype(f(0))(int)>>(forward<F>(f));
		auto _f = new function<void(int id)>(
				[pck](int id){
			(*pck)(id);
		});
		/*auto pck = make_shared<packaged_task<decltype(f(0))(int)>>(
						std::forward<F>(f));
		auto _f = new std::function<void(int id)>([pck](int id) {
					(*pck)(id);
				});*/
		m_queue.push(_f);
		unique_lock<mutex> lock(mtx);
		cv.notify_one();
		return pck->get_future();
	}

private:
	void resize(size_t size){
		if(!isStop && !isDone){
			int oldThreadSize = m_threads.size();
			if(oldThreadSize <= size){
				m_threads.resize(size);
				for(int i = oldThreadSize ; i < size; ++i){
					set_thread(i);
				}
			}
		}
	}

	void clear_queue(){
		function<void(int)>* _f;
		while(m_queue.pop(_f))
			delete _f;
	}
	void set_thread(int i){
		auto f = [i,this](){
			bool isempty;
			while(true){
				while(!this->m_queue.isempty()){
					std::function<void(int id)>* _f;
					this->m_queue.pop(_f);
					unique_ptr<function<void(int id)>> func(_f);
					//cout << i << endl;
					unique_lock<mutex> lock(this->mtx);
					(*_f)(i);
					lock.unlock();
				}
				//The queue is empty here, wait for the next command
				unique_lock<mutex> lock(this->mtx);
				this->cv.wait(lock,[&](){
					isempty = this->m_queue.isempty();
					return !isempty || this->isDone;
				});
				if(isempty)
					return;
			}
		};
		m_threads[i].reset(new thread(f));
	}

private:
	Queue<std::function<void(int id)>*> m_queue;
	size_t size;
	atomic<bool> isStop;
	atomic<bool> isDone;
	vector<unique_ptr<thread>> m_threads;
	mutex mtx;
	condition_variable cv;

};

