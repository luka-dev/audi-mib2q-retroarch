#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static float math_path(float x) {
   return std::pow(x, 3.0f) + std::sqrt(x) + std::floor(x + 0.75f);
}

struct ReusedSync {
   std::mutex mutex;
   std::condition_variable condition;
   bool ready = false;
};

static bool test_reused_sync_address() {
   alignas(ReusedSync) unsigned char storage[sizeof(ReusedSync)];
   for (int round = 0; round < 32; ++round) {
      ReusedSync *sync = new (storage) ReusedSync();
      std::thread waiter([sync] {
         std::unique_lock<std::mutex> lock(sync->mutex);
         sync->condition.wait(lock, [sync] { return sync->ready; });
      });
      {
         std::lock_guard<std::mutex> lock(sync->mutex);
         sync->ready = true;
      }
      sync->condition.notify_one();
      waiter.join();
      sync->~ReusedSync();
   }
   return true;
}

int main() {
   std::vector<std::string> values{"qnx", "ppsspp", "savestate"};
   bool caught = false;
   try {
      throw std::runtime_error(values.at(2));
   } catch (const std::runtime_error &error) {
      caught = std::string(error.what()) == "savestate";
   }

   const float result = math_path(4.0f);
   const bool sync = test_reused_sync_address();

   std::promise<int> promise;
   std::future<int> future = promise.get_future();
   std::thread producer([&promise] { promise.set_value(65); });
   const bool future_ok = future.get() == 65;
   producer.join();

   std::printf("CXX_RUNTIME exception=%s math=%.3f sync-reuse=%s future=%s\n",
         caught ? "ok" : "FAIL", result,
         sync ? "ok" : "FAIL", future_ok ? "ok" : "FAIL");
   return caught && std::fabs(result - 70.0f) < 0.001f && sync && future_ok ? 0 : 1;
}
