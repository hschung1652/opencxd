#include <boost/lockfree/queue.hpp>

#include <unordered_set>
#include <queue>
#include <mutex>
#include <condition_variable>
#include "cache.h"


template <typename T>
class SafeQueue {
public:
    SafeQueue() {}

    // Get the size of the queue
    size_t size() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return queue_.size();
    }

    // Enqueue an element to the back of the queue
    void enqueue(const T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        queue_.push(value);
        lock.unlock();  // Unlock before notifying to minimize lock contention
        cond_.notify_one();
    }

    // Dequeue an element from the front of the queue
    T dequeue() {
        std::unique_lock<std::mutex> lock(mutex_);
        // Use a while loop to handle spurious wake-ups
        cond_.wait(lock, [this] { return !queue_.empty(); });
        T value = queue_.front();
        queue_.pop();
        return value;
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
};

// Example usage:
// SafeQueue<int> myQueue;
// myQueue.enqueue(42);
// int value = myQueue.dequeue();


struct log_write{
    uint64_t lpa;
    uint64_t size;
    uint64_t stime;
};

struct page_promotion_migration
{
    int64_t pm_index;
    uint64_t stime;
};



using boost::lockfree::queue;
using boost::lockfree::fixed_sized;
class cache_controller{
    public:
    sa_cache the_cache;
    //std::unordered_set<int64_t> promoted_set;
    sa_cache host_dram;
    int64_t host_dram_size_pagenum;

    //Test statistics
    int64_t total_access_num;
    int64_t host_hit;
    int64_t anywaydram_hit;

    // queue<log_write, fixed_sized<true>> WritelogQueue;
    queue<page_promotion_migration> PromotionQueue;
    queue<log_write> WritelogQueue;
    //SafeQueue<page_promotion_migration> PromotionQueue;

    cache_controller(int64_t cache_size_in_byte, int way, int64_t maxthreshold, 
    int64_t resetepoch, int64_t host_dram_size_in_byte, int64_t host_way);
    //void process_a_memrequest(char type, int64_t addr);
    void snapshot(FILE* output_file);
    void replay_snapshot(FILE* input_file);

    void report_statistics();
};



