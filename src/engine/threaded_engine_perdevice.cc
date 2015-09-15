/*!
 * Copyright (c) 2015 by Contributors
 * \file threaded_engine_perdevice.cc
 * \brief ThreadedEngine that uses fix amount of thread for each device.
 */
#include <dmlc/base.h>
#include <dmlc/logging.h>
#include <dmlc/parameter.h>
#include <dmlc/concurrency.h>
#include <array>
#include "./threaded_engine.h"
#include "./thread_pool.h"
#include "./stream_manager.h"

namespace mxnet {
namespace engine {
/*!
 * \brief ThreadedEngine uses per device threads.
 * The policy of this Engine:
 *  - Execute Async operation immediately if pushed from Pusher.
 *  - Use fixed amount of threads for each device.
 *  - Use special threads for copy operations.
 *  - Each stream is allocated and binded to each of the thread.
 */
class ThreadedEnginePerDevice : public ThreadedEngine {
 public:
  ThreadedEnginePerDevice() noexcept(false) {
    cpu_worker_nthreads_ = dmlc::GetEnv("MXNET_CPU_WORKER_NTHREADS", 2);
    gpu_worker_nthreads_ = dmlc::GetEnv("MXNET_GPU_WORKER_NTHREADS", 2);
    gpu_copy_nthreads_ = dmlc::GetEnv("MXNET_GPU_COPY_NTHREADS", 1);

    // create CPU task
    auto *cpu_queue = &(cpu_worker_.task_queue);
    cpu_worker_.pool.reset(new ThreadPool(
        cpu_worker_nthreads_, [this, cpu_queue] {
          this->CPUWorker(cpu_queue);
        }));
    // GPU tasks will be created lazily
  }
  ~ThreadedEnginePerDevice() noexcept(false) {
  }

 protected:
  void PushToExecute(OprBlock *opr_block, bool pusher_thread) override {
    if (opr_block->opr->prop == FnProperty::kAsync && pusher_thread) {
      CHECK_EQ(opr_block->ctx.dev_mask, cpu::kDevMask);
      RunContext run_ctx;
      run_ctx.stream = nullptr;
      this->ExecuteOprBlock(run_ctx, opr_block);
    } else {
      const Context& ctx = opr_block->ctx;
      if (ctx.dev_mask == cpu::kDevMask) {
        cpu_worker_.task_queue.Push(opr_block);
      } else {
        CHECK_EQ(ctx.dev_mask, gpu::kDevMask);
        ThreadWorkerBlock* block = this->GetGPUWorkerBlock(
            ctx.dev_id, opr_block->opr->prop);
        block->task_queue.Push(opr_block);
      }
    }
  }

 private:
  // working unit for each of the task.
  struct ThreadWorkerBlock {
    // task queue on this task
    dmlc::ConcurrentBlockingQueue<OprBlock*>  task_queue;
    // thread pool that works on this task
    std::unique_ptr<ThreadPool> pool;
    // destructor
    ~ThreadWorkerBlock() noexcept(false) {
      task_queue.SignalForKill();
    }
  };
  /*! \brief number of concurrent thread cpu worker uses */
  int cpu_worker_nthreads_;
  /*! \brief number of concurrent thread each gpu worker uses */
  int gpu_worker_nthreads_;
  /*! \brief number of concurrent thread each gpu copy worker uses */
  int gpu_copy_nthreads_;
  // mutex used when creating a ThreadWorkerBlock
  std::mutex create_mutex_;
  // cpu worker
  ThreadWorkerBlock cpu_worker_;
  // workers doing normal works on GPU
  std::array<std::unique_ptr<ThreadWorkerBlock>, kMaxNumGPUs> gpu_normal_workers_;
  // workers doing copy works from/to GPU
  std::array<std::unique_ptr<ThreadWorkerBlock>, kMaxNumGPUs> gpu_copy_workers_;
  /*!
   * \brief get GPU Task Worker
   * \param dev_id the device id
   * \param prop The property of the function.
   */
  inline ThreadWorkerBlock *GetGPUWorkerBlock(size_t dev_id,
                                              FnProperty prop) {
    bool is_copy = (prop == FnProperty::kCopyFromGPU ||
                    prop == FnProperty::kCopyToGPU);
    CHECK_LT(dev_id, kMaxNumGPUs)
        << "GPU Device index " << dev_id
        << " exceed bound " << kMaxNumGPUs;
    std::array<std::unique_ptr<ThreadWorkerBlock>, kMaxNumGPUs> *workers;
    if (is_copy) {
      workers = &gpu_copy_workers_;
    } else {
      workers = &gpu_normal_workers_;
    }
    ThreadWorkerBlock *block = workers->at(dev_id).get();
    if (block != nullptr) return block;
    {
      // only lock when block is not available.
      std::lock_guard<std::mutex> lock(create_mutex_);
      // need to double check, because state can change
      ThreadWorkerBlock *block = workers->at(dev_id).get();
      if (block != nullptr) return block;
      int nthread = is_copy ? gpu_copy_nthreads_ : gpu_worker_nthreads_;
      workers->at(dev_id).reset(new ThreadWorkerBlock());
      block = workers->at(dev_id).get();
      block->pool.reset(new ThreadPool(nthread, [this, dev_id, is_copy, block] () {
            this->GPUWorker(dev_id, is_copy, &(block->task_queue));
          }));
      return block;
    }
  }
  /*!
   * \brief GPU worker that performs operations on a certain device.
   * \param dev_id The device id of the worker.
   * \param is_copy_worker whether the worker only do copy job
   * \param task_queue the device id of the worker.
   */
  inline void GPUWorker(int dev_id,
                        bool is_copy_worker,
                        dmlc::ConcurrentBlockingQueue<OprBlock*>* task_queue) {
    #if MXNET_USE_CUDA
    // allocate stream
    mshadow::SetDevice<gpu>(dev_id);
    RunContext run_ctx;
    mshadow::Stream<gpu> *stream;
    if (is_copy_worker) {
      stream = mshadow::NewStream<gpu>(false, false);
    } else {
      stream = mshadow::NewStream<gpu>(true, MXNET_USE_CUDNN != 0);
    }
    run_ctx.stream = stream;
    // execute task
    OprBlock* opr_block;
    while (task_queue->Pop(&opr_block)) {
      this->ExecuteOprBlock(run_ctx, opr_block);
    }
    mshadow::DeleteStream<gpu>(stream);
    #endif
  }
  /*!
   * \brief CPU worker that performs operations on CPU.
   * \param task_queue the device id of the worker.
   */
  inline void CPUWorker(dmlc::ConcurrentBlockingQueue<OprBlock*>* task_queue) {
    RunContext run_ctx;
    run_ctx.stream = nullptr;
    // execute task
    OprBlock* opr_block;
    while (task_queue->Pop(&opr_block)) {
      this->ExecuteOprBlock(run_ctx, opr_block);
    }
  }
};

Engine *CreateThreadedEnginePerDevice() {
  return new ThreadedEnginePerDevice();
}
}  // namespace engine
}  // namespace mxnet
