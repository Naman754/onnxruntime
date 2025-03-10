// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/shared_library/provider_api.h"
#include "core/providers/rocm/rocm_execution_provider.h"
#include "core/providers/rocm/rocm_common.h"
#include "core/providers/rocm/rocm_allocator.h"
#include "core/providers/rocm/rocm_fence.h"
#include "core/providers/rocm/rocm_fwd.h"
#include "core/providers/rocm/gpu_data_transfer.h"

#ifndef DISABLE_CONTRIB_OPS
#include "contrib_ops/rocm/rocm_contrib_kernels.h"
#endif

#ifdef ENABLE_TRAINING
#include "orttraining/training_ops/rocm/rocm_training_kernels.h"
#endif

using namespace onnxruntime::common;

namespace onnxruntime {

class Memcpy final : public OpKernel {
 public:
  Memcpy(const OpKernelInfo& info) : OpKernel{info} {}

  Status Compute(OpKernelContext* ctx) const override {
    auto X_type = ctx->InputType(0);
    if (X_type->IsTensorType()) {
      const auto* X = ctx->Input<Tensor>(0);
      ORT_ENFORCE(X != nullptr, "Memcpy: Input tensor is nullptr.");
      Tensor* Y = ctx->Output(0, X->Shape());
      ORT_ENFORCE(Y != nullptr, "Memcpy: Failed to allocate output tensor.");
      return Info().GetDataTransferManager().CopyTensor(*X, *Y, Info().GetKernelDef().ExecQueueId());
    } else if (X_type->IsSparseTensorType()) {
      const auto* X = ctx->Input<SparseTensor>(0);
      ORT_ENFORCE(X != nullptr, "Memcpy: Input tensor is nullptr.");
      SparseTensor* Y = ctx->OutputSparse(0, X->DenseShape());
      ORT_ENFORCE(Y != nullptr, "Memcpy: Failed to allocate output sparse tensor.");
      return X->Copy(Info().GetDataTransferManager(), Info().GetKernelDef().ExecQueueId(), *Y);
    } else if (X_type->IsTensorSequenceType()) {
      const TensorSeq* X = ctx->Input<TensorSeq>(0);
      ORT_ENFORCE(X != nullptr, "Memcpy: Input tensor sequence is nullptr.");
      TensorSeq* Y = ctx->Output<TensorSeq>(0);
      ORT_ENFORCE(Y != nullptr, "Memcpy: Failed to allocate output tensor sequence.");
      auto X_dtype = X->DataType();
      Y->SetType(X_dtype);
      AllocatorPtr alloc;
      auto status = ctx->GetTempSpaceAllocator(&alloc);
      if (!status.IsOK()) {
        return Status(common::ONNXRUNTIME, common::FAIL,
                      "Memcpy rocm: unable to get an allocator.");
      }
      auto X_size = X->Size();
      for (size_t i = 0; i < X_size; ++i) {
        const Tensor& source_tensor = X->Get(i);
        std::unique_ptr<Tensor> target_tensor = Tensor::Create(source_tensor.DataType(), source_tensor.Shape(), alloc);
        Status retval = Info().GetDataTransferManager().CopyTensor(source_tensor, *target_tensor, Info().GetKernelDef().ExecQueueId());
        if (!retval.IsOK()) {
          return retval;
        }
        Y->Add(std::move(*target_tensor));
      }
      return Status::OK();
    }
    return Status(common::ONNXRUNTIME, common::FAIL, "Memcpy: Unsupported input type.");
  }
};

namespace rocm {
ONNX_OPERATOR_KERNEL_EX(
    MemcpyFromHost,
    kOnnxDomain,
    1,
    kRocmExecutionProvider,
    (*KernelDefBuilder::Create())
        .InputMemoryType(OrtMemTypeCPUInput, 0)
        .ExecQueueId(kHipStreamCopyIn)
        .TypeConstraint("T", DataTypeImpl::AllFixedSizeTensorAndSequenceTensorTypes()),
    Memcpy);

ONNX_OPERATOR_KERNEL_EX(
    MemcpyToHost,
    kOnnxDomain,
    1,
    kRocmExecutionProvider,
    (*KernelDefBuilder::Create())
        .OutputMemoryType(OrtMemTypeCPUOutput, 0)
        .ExecQueueId(kHipStreamCopyOut)
        .TypeConstraint("T", DataTypeImpl::AllFixedSizeTensorAndSequenceTensorTypes()),
    Memcpy);

}  // namespace rocm

AllocatorPtr ROCMExecutionProvider::CreateRocmAllocator(OrtDevice::DeviceId device_id, size_t gpu_mem_limit, ArenaExtendStrategy arena_extend_strategy,
                                                        ROCMExecutionProviderExternalAllocatorInfo external_allocator_info, OrtArenaCfg* default_memory_arena_cfg) {
  if (external_allocator_info.UseExternalAllocator()) {
    AllocatorCreationInfo default_memory_info(
        [external_allocator_info](OrtDevice::DeviceId id) {
          return std::make_unique<ROCMExternalAllocator>(id, CUDA, external_allocator_info.alloc, external_allocator_info.free, external_allocator_info.empty_cache);
        },
        device_id,
        false);

    return CreateAllocator(default_memory_info);

  } else {
    AllocatorCreationInfo default_memory_info(
        [](OrtDevice::DeviceId id) {
          return std::make_unique<ROCMAllocator>(id, CUDA);
        },
        device_id,
        true,
        {default_memory_arena_cfg ? *default_memory_arena_cfg
                                  : OrtArenaCfg(gpu_mem_limit, static_cast<int>(arena_extend_strategy), -1, -1, -1)});

    // ROCM malloc/free is expensive so always use an arena
    return CreateAllocator(default_memory_info);
  }
}

ROCMExecutionProvider::PerThreadContext::PerThreadContext(OrtDevice::DeviceId device_id, hipStream_t stream, size_t gpu_mem_limit,
                                                          ArenaExtendStrategy arena_extend_strategy, ROCMExecutionProviderExternalAllocatorInfo external_allocator_info,
                                                          OrtArenaCfg* default_memory_arena_cfg) {
  HIP_CALL_THROW(hipSetDevice(device_id));
  stream_ = stream;

  ROCBLAS_CALL_THROW(rocblas_create_handle(&rocblas_handle_));
  ROCBLAS_CALL_THROW(rocblas_set_stream(rocblas_handle_, stream));

  MIOPEN_CALL_THROW(miopenCreate(&miopen_handle_));
  MIOPEN_CALL_THROW(miopenSetStream(miopen_handle_, stream));

  // ROCM malloc/free is expensive so always use an arena
  allocator_ = CreateRocmAllocator(device_id, gpu_mem_limit, arena_extend_strategy, external_allocator_info, default_memory_arena_cfg);
}

ROCMExecutionProvider::PerThreadContext::~PerThreadContext() {
  // dtor shouldn't throw. if something went wrong earlier (e.g. out of ROCM memory) the handles
  // here may be bad, and the destroy calls can throw.
  // https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#Rc-dtor-noexcept
  try {
    ROCBLAS_CALL(rocblas_destroy_handle(rocblas_handle_));
  } catch (const std::exception& ex) {
    LOGS_DEFAULT(ERROR) << "rocblas_destroy_handle threw:" << ex.what();
  }

  try {
    MIOPEN_CALL(miopenDestroy(miopen_handle_));
  } catch (const std::exception& ex) {
    LOGS_DEFAULT(ERROR) << "miopenDestroy threw:" << ex.what();
  }
}

ROCMExecutionProvider::ROCMExecutionProvider(const ROCMExecutionProviderInfo& info)
    : IExecutionProvider{onnxruntime::kRocmExecutionProvider},
      info_{info} {
  HIP_CALL_THROW(hipSetDevice(info_.device_id));

  // must wait GPU idle, otherwise hipGetDeviceProperties might fail
  HIP_CALL_THROW(hipDeviceSynchronize());
  HIP_CALL_THROW(hipGetDeviceProperties(&device_prop_, info_.device_id));

  // This scenario is not supported.
  ORT_ENFORCE(!(info.has_user_compute_stream && info.external_allocator_info.UseExternalAllocator()));

  if (info.has_user_compute_stream) {
    external_stream_ = true;
    stream_ = static_cast<hipStream_t>(info.user_compute_stream);
  } else {
    if (info.external_allocator_info.UseExternalAllocator()) {
      stream_ = nullptr;
    } else {
      HIP_CALL_THROW(hipStreamCreateWithFlags(&stream_, hipStreamNonBlocking));
    }
  }

  size_t free = 0;
  size_t total = 0;
  HIP_CALL_THROW(hipMemGetInfo(&free, &total));
}

ROCMExecutionProvider::~ROCMExecutionProvider() {
  auto cpu_alloc = GetAllocator(DEFAULT_CPU_ALLOCATOR_DEVICE_ID, OrtMemTypeCPU);
  {
    std::lock_guard<OrtMutex> lock(deferred_release_cpu_ptr_mutex_);
    auto it = deferred_release_cpu_ptr_.begin();
    while (it != deferred_release_cpu_ptr_.end()) {
      auto& e = it->first;
      auto& v = it->second;
      if (v.recorded)
        HIP_CALL_THROW(hipEventSynchronize(e));
      for (auto p : v.cpu_ptrs) {
        cpu_alloc->Free(p);
      }
      HIP_CALL_THROW(hipEventDestroy(e));
      it = deferred_release_cpu_ptr_.erase(it);
    }
  }

  // clean up thread local context caches
  {
    std::lock_guard<OrtMutex> lock(context_state_.mutex);
    for (const auto& cache_weak : context_state_.caches_to_update_on_destruction) {
      const auto cache = cache_weak.lock();
      if (!cache) continue;
      ORT_IGNORE_RETURN_VALUE(cache->erase(this));
    }
  }

  if (!external_stream_ && stream_) {
    HIP_CALL(hipStreamDestroy(stream_));
  }
}

ROCMExecutionProvider::PerThreadContext& ROCMExecutionProvider::GetPerThreadContext() const {
  const auto& per_thread_context_cache = PerThreadContextCache();

  // try to use cached context
  auto cached_context_it = per_thread_context_cache->find(this);
  if (cached_context_it != per_thread_context_cache->end()) {
    auto cached_context = cached_context_it->second.lock();
    ORT_ENFORCE(cached_context);
    return *cached_context;
  }

  // get context and update cache
  std::shared_ptr<PerThreadContext> context;
  {
    std::lock_guard<OrtMutex> lock(context_state_.mutex);

    // get or create a context
    if (context_state_.retired_context_pool.empty()) {
      context = std::make_shared<PerThreadContext>(info_.device_id, static_cast<hipStream_t>(GetComputeStream()), info_.gpu_mem_limit,
                                                   info_.arena_extend_strategy, info_.external_allocator_info, info_.default_memory_arena_cfg);
    } else {
      context = context_state_.retired_context_pool.back();
      context_state_.retired_context_pool.pop_back();
    }

    // insert into active_contexts, should not already be present
    const auto active_contexts_insert_result = context_state_.active_contexts.insert(context);
    ORT_ENFORCE(active_contexts_insert_result.second);

    // insert into caches_to_update_on_destruction, may already be present
    ORT_IGNORE_RETURN_VALUE(context_state_.caches_to_update_on_destruction.insert(per_thread_context_cache));
  }

  per_thread_context_cache->insert(std::make_pair(this, context));

  return *context;
}

void ROCMExecutionProvider::ReleasePerThreadContext() const {
  const auto& per_thread_context_cache = PerThreadContextCache();

  auto cached_context_it = per_thread_context_cache->find(this);
  ORT_ENFORCE(cached_context_it != per_thread_context_cache->end());
  auto cached_context = cached_context_it->second.lock();
  ORT_ENFORCE(cached_context);

  {
    std::lock_guard<OrtMutex> lock(context_state_.mutex);
    context_state_.active_contexts.erase(cached_context);
    context_state_.retired_context_pool.push_back(cached_context);
  }

  per_thread_context_cache->erase(cached_context_it);
}

AllocatorPtr ROCMExecutionProvider::GetAllocator(int id, OrtMemType mem_type) const {
  // Pinned memory allocator is shared between threads, but ROCM memory allocator is per-thread or it may cause result changes
  // A hypothesis is that arena allocator is not aligned with ROCM output cache, and data from different kernel writes may
  // cause cacheline to contain dirty data.
  if (mem_type == OrtMemTypeDefault) {
    return GetPerThreadContext().GetAllocator();
  } else {
    return IExecutionProvider::GetAllocator(id, mem_type);
  }
}

Status ROCMExecutionProvider::Sync() const {
  HIP_RETURN_IF_ERROR(hipDeviceSynchronize());
  return Status::OK();
}

void ROCMExecutionProvider::AddDeferredReleaseCPUPtr(void* p) {
  // when not running in InferenceSession (e.g. Test)
  // it's OK to not remember the deferred release ptr
  // as the actual memory will be cleaned in arena allocator dtor
  auto current_deferred_release_event = GetPerThreadContext().GetCurrentDeferredReleaseEvent();
  if (current_deferred_release_event) {
    std::lock_guard<OrtMutex> lock(deferred_release_cpu_ptr_mutex_);
    auto iter = deferred_release_cpu_ptr_.find(current_deferred_release_event);
    ORT_ENFORCE(iter != deferred_release_cpu_ptr_.end());
    iter->second.cpu_ptrs.push_back(p);
  }
}

Status ROCMExecutionProvider::OnRunStart() {
  // always set ROCM device when session::Run() in case it runs in a worker thread
  HIP_RETURN_IF_ERROR(hipSetDevice(GetDeviceId()));
  auto cpu_alloc = GetAllocator(0, OrtMemTypeCPU);
  // check if hipEvents has passed for deferred release
  // note that we need to take a mutex in case of multi-threaded Run()
  std::lock_guard<OrtMutex> lock(deferred_release_cpu_ptr_mutex_);
  auto it = deferred_release_cpu_ptr_.begin();
  while (it != deferred_release_cpu_ptr_.end()) {
    auto& e = it->first;
    auto& v = it->second;
    // note that hipEventQuery returns rocmSucess before first hipEventRecord
    if (v.recorded) {
      auto event_query_status = hipEventQuery(e);
      if (event_query_status == hipSuccess) {
        for (auto p : v.cpu_ptrs) {
          cpu_alloc->Free(p);
        }
        HIP_RETURN_IF_ERROR(hipEventDestroy(e));
        it = deferred_release_cpu_ptr_.erase(it);
      } else if (event_query_status == hipErrorNotReady) {
        // ignore and clear the error if not ready; void to silence nodiscard
        (void)hipGetLastError();
        it++;
      } else {
        HIP_RETURN_IF_ERROR(event_query_status);
      }
    } else {
      ++it;
    }
  }

  auto& current_deferred_release_event = GetPerThreadContext().GetCurrentDeferredReleaseEvent();
  HIP_RETURN_IF_ERROR(hipEventCreateWithFlags(&current_deferred_release_event, hipEventDisableTiming));
  deferred_release_cpu_ptr_.emplace(current_deferred_release_event, DeferredReleaseCPUPtrs());
  return Status::OK();
}

Status ROCMExecutionProvider::OnRunEnd() {
  // record deferred release event on default stream, and release per_thread_context
  auto current_deferred_release_event = GetPerThreadContext().GetCurrentDeferredReleaseEvent();
  HIP_RETURN_IF_ERROR(hipEventRecord(current_deferred_release_event, static_cast<hipStream_t>(GetComputeStream())));
  HIP_RETURN_IF_ERROR(hipStreamSynchronize(static_cast<hipStream_t>(GetComputeStream())));
  ReleasePerThreadContext();
  std::lock_guard<OrtMutex> lock(deferred_release_cpu_ptr_mutex_);
  deferred_release_cpu_ptr_[current_deferred_release_event].recorded = true;

  return Status::OK();
}

Status ROCMExecutionProvider::SetComputeStream(void* stream) {
  if (stream != stream_) {
    if (stream_) {
      HIP_RETURN_IF_ERROR(hipStreamDestroy(stream_));
    }

    external_stream_ = true;
    stream_ = static_cast<hipStream_t>(stream);
  }
  return Status::OK();
}

namespace rocm {
// opset 1 to 9
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, MemcpyFromHost);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, MemcpyToHost);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, float, Cos);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, double, Cos);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, MLFloat16, Cos);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, float, Sin);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, double, Sin);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, MLFloat16, Sin);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 4, 10, Concat);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, Unsqueeze);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 8, Flatten);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, Squeeze);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, Identity);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 9, Dropout);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, Gather);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, float, Gemm);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, double, Gemm);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, MLFloat16, Gemm);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 10, float, Gemm);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 10, double, Gemm);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 10, MLFloat16, Gemm);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 8, float, MatMul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 8, double, MatMul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 8, MLFloat16, MatMul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, float, MatMul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, double, MatMul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, MLFloat16, MatMul);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, int8_t, MatMulInteger);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, float, Elu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, double, Elu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, MLFloat16, Elu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, float, HardSigmoid);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, double, HardSigmoid);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, MLFloat16, HardSigmoid);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, float, LeakyRelu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, double, LeakyRelu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, MLFloat16, LeakyRelu);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Relu);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Relu);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Relu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, float, Selu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, double, Selu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, MLFloat16, Selu);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Sigmoid);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Sigmoid);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Sigmoid);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, float, Softsign);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, double, Softsign);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, MLFloat16, Softsign);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Tanh);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Tanh);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Tanh);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, float, Softplus);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, double, Softplus);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, MLFloat16, Softplus);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, Softmax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, Softmax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, Softmax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, LogSoftmax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, LogSoftmax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, LogSoftmax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 11, float, Pow);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 11, double, Pow);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 11, MLFloat16, Pow);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, float, PRelu);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, double, PRelu);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, MLFloat16, PRelu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, float, PRelu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, double, PRelu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, MLFloat16, PRelu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, bool, And);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, bool, Or);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, bool, Xor);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 7, Sum);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 8, 12, Sum);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 11, Max);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, Max);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 11, Min);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, Min);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, float, Greater);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, double, Greater);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, MLFloat16, Greater);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 10, bool, Equal);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 10, int32_t, Equal);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 10, int64_t, Equal);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 8, 12, Expand);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int32_t, Greater);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int64_t, Greater);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint32_t, Greater);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint64_t, Greater);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, float, Greater);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, double, Greater);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, MLFloat16, Greater);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, int32_t, GreaterOrEqual);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, int64_t, GreaterOrEqual);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, uint32_t, GreaterOrEqual);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, uint64_t, GreaterOrEqual);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, float, GreaterOrEqual);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, double, GreaterOrEqual);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, MLFloat16, GreaterOrEqual);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, int32_t, LessOrEqual);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, int64_t, LessOrEqual);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, uint32_t, LessOrEqual);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, uint64_t, LessOrEqual);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, float, LessOrEqual);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, double, LessOrEqual);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, MLFloat16, LessOrEqual);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int32_t, Add);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int64_t, Add);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint32_t, Add);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint64_t, Add);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, float, Add);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, double, Add);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, MLFloat16, Add);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int32_t, Sub);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int64_t, Sub);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint32_t, Sub);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint64_t, Sub);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, float, Sub);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, double, Sub);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, MLFloat16, Sub);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int32_t, Mul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int64_t, Mul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint32_t, Mul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint64_t, Mul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, float, Mul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, double, Mul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, MLFloat16, Mul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int32_t, Div);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int64_t, Div);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint32_t, Div);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint64_t, Div);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, float, Div);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, double, Div);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, MLFloat16, Div);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int8_t, Abs);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int16_t, Abs);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int32_t, Abs);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int64_t, Abs);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, uint8_t, Abs);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, uint16_t, Abs);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, uint32_t, Abs);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, uint64_t, Abs);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Abs);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Abs);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Abs);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int8_t, Neg);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int16_t, Neg);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int32_t, Neg);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int64_t, Neg);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Neg);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Neg);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Neg);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Floor);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Floor);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Floor);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Ceil);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Ceil);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Ceil);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 10, float, Clip);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Reciprocal);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Reciprocal);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Reciprocal);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Sqrt);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Sqrt);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Sqrt);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Log);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Log);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Log);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Exp);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Exp);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Exp);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, float, Erf);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, double, Erf);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, MLFloat16, Erf);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, bool, Not);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, float, BatchNormalization);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, double, BatchNormalization);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, MLFloat16, BatchNormalization);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 13, float, BatchNormalization);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 13, double, BatchNormalization);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 13, MLFloat16, BatchNormalization);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, float, LRN);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, double, LRN);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, MLFloat16, LRN);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, Conv);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, Conv);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, Conv);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ConvTranspose);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ConvTranspose);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ConvTranspose);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 9, float, AveragePool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 9, double, AveragePool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 9, MLFloat16, AveragePool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, float, GlobalAveragePool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, double, GlobalAveragePool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, MLFloat16, GlobalAveragePool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 7, float, MaxPool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 7, double, MaxPool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 7, MLFloat16, MaxPool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 8, 9, float, MaxPool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 8, 9, double, MaxPool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 8, 9, MLFloat16, MaxPool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, float, GlobalMaxPool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, double, GlobalMaxPool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, MLFloat16, GlobalMaxPool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ArgMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ArgMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ArgMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ArgMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ArgMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ArgMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceL1);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceL1);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceL1);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int32_t, ReduceL1);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceL2);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceL2);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceL2);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int32_t, ReduceL2);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int32_t, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int64_t, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceMean);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceMean);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceMean);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int32_t, ReduceMean);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int32_t, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceProd);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceProd);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceProd);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int32_t, ReduceProd);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int32_t, ReduceSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int64_t, ReduceSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceLogSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceLogSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceLogSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceSumSquare);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceSumSquare);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceSumSquare);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceLogSumExp);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceLogSumExp);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceLogSumExp);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, float, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, double, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, MLFloat16, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, int8_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, int16_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, int32_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, int64_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, uint8_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, uint16_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, uint32_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, uint64_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, bool, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, float, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, double, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, MLFloat16, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int8_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int16_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int32_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int64_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint8_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint16_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint32_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint64_t, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, bool, Cast);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 2, 10, float, Pad);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 2, 10, double, Pad);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 2, 10, MLFloat16, Pad);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 4, Reshape);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 5, 12, Reshape);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, Shape);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, Size);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, Tile);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Tile);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, Transpose);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, float, InstanceNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, double, InstanceNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, MLFloat16, InstanceNormalization);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, float, RNN);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, double, RNN);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, MLFloat16, RNN);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, float, GRU);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, double, GRU);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, MLFloat16, GRU);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, float, LSTM);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, double, LSTM);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, MLFloat16, LSTM);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 9, int32_t, Slice);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 9, int64_t, Slice);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 9, float, Slice);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 10, Compress);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 10, Flatten);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, float, Upsample);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, double, Upsample);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, MLFloat16, Upsample);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, int32_t, Upsample);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, uint8_t, Upsample);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 9, float, Upsample);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 9, double, Upsample);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 9, MLFloat16, Upsample);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 9, int32_t, Upsample);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 9, uint8_t, Upsample);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 2, 10, Split);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, ConstantOfShape);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, int8_t, Shrink);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, int16_t, Shrink);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, int32_t, Shrink);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, int64_t, Shrink);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, uint8_t, Shrink);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, uint16_t, Shrink);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, uint32_t, Shrink);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, uint64_t, Shrink);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, float, Shrink);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, double, Shrink);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, MLFloat16, Shrink);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, float, Less);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, double, Less);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, MLFloat16, Less);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int32_t, Less);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int64_t, Less);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint32_t, Less);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint64_t, Less);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, float, Less);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, double, Less);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, MLFloat16, Less);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, EyeLike);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 10, Scatter);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, MLFloat16, Where);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, float, Where);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, double_t, Where);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, int32_t, Where);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, int64_t, Where);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, uint8_t, Where);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, bool, NonZero);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint8_t, NonZero);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int32_t, NonZero);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int64_t, NonZero);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, float, NonZero);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 9, TopK);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, If);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 8, 8, Scan);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 10, Scan);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, Loop);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, DepthToSpace);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, SpaceToDepth);

// opset 10
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, float, AveragePool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, double, AveragePool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, MLFloat16, AveragePool);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 11, Dropout);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, float, MaxPool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, double, MaxPool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, MLFloat16, MaxPool);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, NonMaxSuppression);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, float, Resize);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, double, Resize);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, MLFloat16, Resize);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, int32_t, Resize);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, uint8_t, Resize);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, ReverseSequence);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, float, RoiAlign);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, double, RoiAlign);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, int32_t, Slice);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, int64_t, Slice);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, float, Slice);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, float, ThresholdedRelu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, double, ThresholdedRelu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, MLFloat16, ThresholdedRelu);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, TopK);

// opset 11
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, float, ArgMax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, double, ArgMax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, MLFloat16, ArgMax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, float, ArgMin);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, double, ArgMin);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, MLFloat16, ArgMin);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, Compress);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, Concat);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, Flatten);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, Gather);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, GatherElements);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, Gemm);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, Gemm);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, Gemm);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, If);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, Loop);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, NonMaxSuppression);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, Range);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceL1);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceL1);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceL1);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, ReduceL1);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceL2);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceL2);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceL2);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, ReduceL2);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceLogSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceLogSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceLogSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceLogSumExp);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceLogSumExp);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceLogSumExp);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, float, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, double, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, MLFloat16, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, int32_t, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, int64_t, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceMean);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceMean);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceMean);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, ReduceMean);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, float, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, double, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, MLFloat16, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, int32_t, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceProd);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceProd);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceProd);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, ReduceProd);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, ReduceSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int64_t, ReduceSum);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceSumSquare);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceSumSquare);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceSumSquare);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, Scan);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, ScatterElements);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, Slice);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int64_t, Slice);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, Slice);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, Softmax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, Softmax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, Softmax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, LogSoftmax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, LogSoftmax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, LogSoftmax);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, Split);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, Squeeze);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, TopK);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, SequenceAt);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, SequenceConstruct);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, SequenceEmpty);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, SequenceLength);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, ConcatFromSequence);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, SequenceErase);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, SequenceInsert);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, Unsqueeze);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, float, Conv);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, double, Conv);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, MLFloat16, Conv);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, float, ConvTranspose);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, double, ConvTranspose);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, MLFloat16, ConvTranspose);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, float, AveragePool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, double, AveragePool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, MLFloat16, AveragePool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, float, MaxPool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, double, MaxPool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, MLFloat16, MaxPool);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, Resize);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, Resize);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, Resize);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, Resize);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, uint8_t, Resize);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, Clip);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, Pad);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, Pad);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, Pad);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, bool, Equal);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, Equal);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int64_t, Equal);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, uint32_t, Equal);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, uint64_t, Equal);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, Equal);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, Equal);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, Equal);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, float, Round);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, double, Round);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, MLFloat16, Round);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, int8_t, QuantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, uint8_t, QuantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, int8_t, DequantizeLinear);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, uint8_t, DequantizeLinear);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 13, CumSum);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, int64_t_int64_t_int64_t, OneHot);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, int64_t_float_int64_t, OneHot);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, int32_t_float_int32_t, OneHot);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, int64_t_MLFloat16_int64_t, OneHot);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, int32_t_MLFloat16_int32_t, OneHot);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, ScatterND);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, DepthToSpace);

// OpSet 12
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, Clip);

class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, float, MaxPool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, double, MaxPool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, MLFloat16, MaxPool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, int8_t, MaxPool);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, uint8_t, MaxPool);

class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, Pow);

class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, float, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, double, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, MLFloat16, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, int32_t, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, int64_t, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, int8_t, ReduceMax);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, uint8_t, ReduceMax);

class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, float, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, double, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, MLFloat16, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, int32_t, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, int64_t, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, int8_t, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, uint8_t, ReduceMin);

class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, int64_t, GatherND);

class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, Dropout);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, Einsum);

//OpSet 13
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 14, Pow);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int32_t, Add);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int64_t, Add);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint32_t, Add);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint64_t, Add);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, float, Add);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, double, Add);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, MLFloat16, Add);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Clip);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int32_t, Sub);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int64_t, Sub);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint32_t, Sub);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint64_t, Sub);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, float, Sub);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, double, Sub);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, MLFloat16, Sub);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int32_t, Mul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int64_t, Mul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint32_t, Mul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint64_t, Mul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, float, Mul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, double, Mul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, MLFloat16, Mul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int32_t, Div);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int64_t, Div);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint32_t, Div);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint64_t, Div);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, float, Div);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, double, Div);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, MLFloat16, Div);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int8_t, Abs);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int16_t, Abs);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Abs);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, Abs);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint8_t, Abs);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint16_t, Abs);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint32_t, Abs);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint64_t, Abs);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Abs);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Abs);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Abs);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int8_t, Neg);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int16_t, Neg);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Neg);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, Neg);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Neg);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Neg);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Neg);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Floor);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Floor);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Floor);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Ceil);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Ceil);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Ceil);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Reciprocal);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Reciprocal);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Reciprocal);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Sqrt);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Sqrt);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Sqrt);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Log);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Log);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Log);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Exp);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Exp);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Exp);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Erf);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Erf);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Erf);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Expand);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Sum);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Max);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Min);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, bool, Equal);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Equal);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, Equal);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint32_t, Equal);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint64_t, Equal);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Equal);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Equal);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Equal);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Greater);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, Greater);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint32_t, Greater);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint64_t, Greater);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Greater);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Greater);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Greater);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Less);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, Less);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint32_t, Less);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint64_t, Less);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Less);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Less);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Less);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, bool, NonZero);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint8_t, NonZero);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, NonZero);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, NonZero);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, NonZero);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Cast);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Cast);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Cast);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int8_t, Cast);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int16_t, Cast);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Cast);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, Cast);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint8_t, Cast);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint16_t, Cast);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint32_t, Cast);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint64_t, Cast);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, bool, Cast);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, Reshape);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 14, Shape);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Size);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Transpose);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, ScatterElements);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Slice);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, Slice);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Slice);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Softmax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Softmax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Softmax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, LogSoftmax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, LogSoftmax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, LogSoftmax);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Split);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Squeeze);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Unsqueeze);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Concat);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Gather);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, GatherElements);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, MatMul);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, MatMul);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, MatMul);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, float, Relu);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, double, Relu);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, MLFloat16, Relu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Sigmoid);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Sigmoid);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Sigmoid);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Tanh);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Tanh);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Tanh);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Gemm);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Gemm);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Gemm);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceL1);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceL1);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceL1);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, ReduceL1);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceL2);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceL2);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceL2);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, ReduceL2);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceLogSum);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceLogSum);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceLogSum);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceLogSumExp);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceLogSumExp);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceLogSumExp);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceMax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceMax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceMax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, ReduceMax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, ReduceMax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int8_t, ReduceMax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint8_t, ReduceMax);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceMean);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceMean);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceMean);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, ReduceMean);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, float, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, double, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, MLFloat16, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int32_t, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int64_t, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int8_t, ReduceMin);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint8_t, ReduceMin);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceProd);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceProd);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceProd);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, ReduceProd);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceSum);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceSum);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceSum);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, ReduceSum);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, ReduceSum);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceSumSquare);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceSumSquare);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceSumSquare);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, GatherND);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Dropout);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Resize);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Resize);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Resize);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Resize);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint8_t, Resize);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, If);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Loop);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Flatten);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, LRN);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, LRN);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, LRN);
class ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, Identity);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, ScatterND);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Pad);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Pad);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Pad);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, bool, Pad);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, SpaceToDepth);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, DepthToSpace);

// class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, BFloat16, Add);
// class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, BFloat16, Sub);
// class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, BFloat16, Mul);
// class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, BFloat16, Div);
// class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, BFloat16, Cast);
// class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, BFloat16, Softmax);
// class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, BFloat16, MatMul);
// class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, BFloat16, Relu);
// class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, BFloat16, Sigmoid);
// class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, BFloat16, Tanh);
// class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, BFloat16, Gemm);
// class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, BFloat16, ReduceSum);

// OpSet 14
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, CumSum);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, Relu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, Relu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, Relu);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int32_t, Add);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int64_t, Add);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint32_t, Add);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint64_t, Add);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, Add);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, Add);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, Add);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int32_t, Sub);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int64_t, Sub);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint32_t, Sub);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint64_t, Sub);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, Sub);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, Sub);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, Sub);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int32_t, Mul);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int64_t, Mul);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint32_t, Mul);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint64_t, Mul);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, Mul);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, Mul);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, Mul);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int32_t, Div);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int64_t, Div);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint32_t, Div);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint64_t, Div);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, Div);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, Div);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, Div);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, Identity);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, Reshape);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, RNN);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, RNN);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, RNN);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, GRU);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, GRU);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, GRU);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, LSTM);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, LSTM);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, LSTM);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, 14, float, BatchNormalization);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, 14, double, BatchNormalization);
class ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, 14, MLFloat16, BatchNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, ReduceMin);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, ReduceMin);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, ReduceMin);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int32_t, ReduceMin);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int8_t, ReduceMin);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint8_t, ReduceMin);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int64_t, ReduceMin);

// class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, BFloat16, Add);
// class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, BFloat16, Sub);
// class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, BFloat16, Mul);
// class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, BFloat16, Div);
// class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, BFloat16, Relu);

//OpSet 15
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 15, Pow);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 15, float, BatchNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 15, double, BatchNormalization);
class ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 15, MLFloat16, BatchNormalization);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 15, Shape);

template <>
KernelCreateInfo BuildKernelCreateInfo<void>() {
  return {};
}

static Status RegisterRocmKernels(KernelRegistry& kernel_registry) {
  static const BuildKernelCreateInfoFn function_table[] = {
    BuildKernelCreateInfo<void>,  //default entry to avoid the list become empty after ops-reducing
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, MemcpyFromHost)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, MemcpyToHost)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 4, 10, Concat)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, Unsqueeze)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 8, Flatten)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, Squeeze)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, Identity)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 9, Dropout)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, float, Cos)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, double, Cos)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, MLFloat16, Cos)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, float, Sin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, double, Sin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, MLFloat16, Sin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, Gather)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, float, Gemm)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, double, Gemm)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, MLFloat16, Gemm)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 10, float, Gemm)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 10, double, Gemm)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 10, MLFloat16, Gemm)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 8, float, MatMul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 8, double, MatMul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 8, MLFloat16, MatMul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, float, MatMul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, double, MatMul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, MLFloat16, MatMul)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, int8_t, MatMulInteger)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 10, float, Clip)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, float, Elu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, double, Elu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, MLFloat16, Elu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, float, HardSigmoid)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, double, HardSigmoid)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, MLFloat16, HardSigmoid)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, float, LeakyRelu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, double, LeakyRelu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, MLFloat16, LeakyRelu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Relu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Relu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Relu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, float, Selu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, double, Selu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, MLFloat16, Selu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Sigmoid)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Sigmoid)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Sigmoid)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, float, Softsign)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, double, Softsign)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, MLFloat16, Softsign)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Tanh)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Tanh)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Tanh)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, float, Softplus)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, double, Softplus)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, MLFloat16, Softplus)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, Softmax)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, Softmax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, Softmax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, LogSoftmax)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, LogSoftmax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, LogSoftmax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 11, float, Pow)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 11, double, Pow)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 11, MLFloat16, Pow)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, float, PRelu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, double, PRelu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, MLFloat16, PRelu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, float, PRelu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, double, PRelu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, MLFloat16, PRelu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, bool, And)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, bool, Or)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, bool, Xor)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 7, Sum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 8, 12, Sum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 11, Max)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, Max)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 11, Min)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, Min)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, float, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, double, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, MLFloat16, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 10, bool, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 10, int32_t, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 10, int64_t, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 8, 12, Expand)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int32_t, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int64_t, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint32_t, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint64_t, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, float, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, double, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, MLFloat16, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, int32_t, GreaterOrEqual)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, int64_t, GreaterOrEqual)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, uint32_t, GreaterOrEqual)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, uint64_t, GreaterOrEqual)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, float, GreaterOrEqual)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, double, GreaterOrEqual)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, MLFloat16, GreaterOrEqual)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, int32_t, LessOrEqual)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, int64_t, LessOrEqual)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, uint32_t, LessOrEqual)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, uint64_t, LessOrEqual)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, float, LessOrEqual)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, double, LessOrEqual)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, MLFloat16, LessOrEqual)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int32_t, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int64_t, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint32_t, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint64_t, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, float, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, double, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, MLFloat16, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int32_t, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int64_t, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint32_t, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint64_t, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, float, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, double, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, MLFloat16, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int32_t, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int64_t, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint32_t, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint64_t, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, float, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, double, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, MLFloat16, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int32_t, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, int64_t, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint32_t, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, uint64_t, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, float, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, double, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 12, MLFloat16, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int8_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int16_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int32_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int64_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, uint8_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, uint16_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, uint32_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, uint64_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int8_t, Neg)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int16_t, Neg)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int32_t, Neg)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, int64_t, Neg)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Neg)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Neg)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Neg)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Floor)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Floor)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Floor)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Ceil)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Ceil)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Ceil)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Reciprocal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Reciprocal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Reciprocal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Sqrt)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Sqrt)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Sqrt)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Log)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Log)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Log)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, float, Exp)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, double, Exp)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, MLFloat16, Exp)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, float, Erf)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, double, Erf)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, MLFloat16, Erf)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, bool, Not)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, float, BatchNormalization)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, double, BatchNormalization)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, MLFloat16, BatchNormalization)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 13, float, BatchNormalization)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 13, double, BatchNormalization)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 13, MLFloat16, BatchNormalization)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, float, LRN)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, double, LRN)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, MLFloat16, LRN)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, Conv)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, Conv)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, Conv)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ConvTranspose)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ConvTranspose)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ConvTranspose)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 9, float, AveragePool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 9, double, AveragePool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 9, MLFloat16, AveragePool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, float, GlobalAveragePool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, double, GlobalAveragePool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, MLFloat16, GlobalAveragePool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 7, float, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 7, double, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 7, MLFloat16, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 8, 9, float, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 8, 9, double, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 8, 9, MLFloat16, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, float, GlobalMaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, double, GlobalMaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, MLFloat16, GlobalMaxPool)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ArgMax)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ArgMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ArgMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ArgMin)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ArgMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ArgMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceL1)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceL1)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceL1)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int32_t, ReduceL1)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceL2)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceL2)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceL2)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int32_t, ReduceL2)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceMax)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int32_t, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int64_t, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceMean)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceMean)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceMean)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int32_t, ReduceMean)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceMin)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int32_t, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceProd)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceProd)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceProd)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int32_t, ReduceProd)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceSum)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int32_t, ReduceSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, int64_t, ReduceSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceLogSum)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceLogSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceLogSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceSumSquare)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceSumSquare)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceSumSquare)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, float, ReduceLogSumExp)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, double, ReduceLogSumExp)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, MLFloat16, ReduceLogSumExp)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, float, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, double, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, MLFloat16, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, int8_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, int16_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, int32_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, int64_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, uint8_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, uint16_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, uint32_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, uint64_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 8, bool, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, float, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, double, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, MLFloat16, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int8_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int16_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int32_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int64_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint8_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint16_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint32_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint64_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, bool, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 2, 10, float, Pad)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 2, 10, double, Pad)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 2, 10, MLFloat16, Pad)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 4, Reshape)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 5, 12, Reshape)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, Shape)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, Size)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, 12, Tile)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Tile)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, Transpose)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, float, InstanceNormalization)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, double, InstanceNormalization)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 6, MLFloat16, InstanceNormalization)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, float, RNN)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, double, RNN)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, MLFloat16, RNN)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, float, GRU)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, double, GRU)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, MLFloat16, GRU)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, float, LSTM)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, double, LSTM)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 13, MLFloat16, LSTM)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 9, int32_t, Slice)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 9, int64_t, Slice)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 9, float, Slice)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 10, Compress)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 10, Flatten)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, float, Upsample)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, double, Upsample)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, MLFloat16, Upsample)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, int32_t, Upsample)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, uint8_t, Upsample)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 9, float, Upsample)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 9, double, Upsample)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 9, MLFloat16, Upsample)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 9, int32_t, Upsample)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 9, uint8_t, Upsample)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 2, 10, Split)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, ConstantOfShape)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, int8_t, Shrink)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, int16_t, Shrink)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, int32_t, Shrink)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, int64_t, Shrink)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, uint8_t, Shrink)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, uint16_t, Shrink)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, uint32_t, Shrink)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, uint64_t, Shrink)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, float, Shrink)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, double, Shrink)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, MLFloat16, Shrink)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, float, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, double, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 7, 8, MLFloat16, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int32_t, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int64_t, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint32_t, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint64_t, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, float, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, double, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, MLFloat16, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, EyeLike)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 10, Scatter)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, MLFloat16, Where)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, float, Where)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, double_t, Where)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, int32_t, Where)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, int64_t, Where)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, uint8_t, Where)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, bool, NonZero)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, uint8_t, NonZero)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int32_t, NonZero)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, int64_t, NonZero)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 12, float, NonZero)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 9, TopK)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 8, 8, Scan)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 9, 10, Scan)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, Loop)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, DepthToSpace)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 12, SpaceToDepth)>,

    // opset 10
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, float, AveragePool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, double, AveragePool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, MLFloat16, AveragePool)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 11, Dropout)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, float, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, double, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, MLFloat16, MaxPool)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, NonMaxSuppression)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, float, Resize)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, double, Resize)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, MLFloat16, Resize)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, int32_t, Resize)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, uint8_t, Resize)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, ReverseSequence)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, float, RoiAlign)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, double, RoiAlign)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, int32_t, Slice)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, int64_t, Slice)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, float, Slice)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, float, ThresholdedRelu)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, double, ThresholdedRelu)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, MLFloat16, ThresholdedRelu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, 10, TopK)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 1, 10, If)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, int8_t, QuantizeLinear)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, uint8_t, QuantizeLinear)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, int8_t, DequantizeLinear)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 10, uint8_t, DequantizeLinear)>,

    // opset 11
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, float, ArgMax)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, double, ArgMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, MLFloat16, ArgMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, float, ArgMin)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, double, ArgMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, MLFloat16, ArgMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, Compress)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, Concat)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, Flatten)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, Gather)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, GatherElements)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, Gemm)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, Gemm)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, Gemm)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, If)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, Loop)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, NonMaxSuppression)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, Range)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceL1)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceL1)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceL1)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, ReduceL1)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceL2)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceL2)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceL2)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, ReduceL2)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceLogSum)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceLogSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceLogSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceLogSumExp)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceLogSumExp)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceLogSumExp)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, float, ReduceMax)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, double, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, MLFloat16, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, int32_t, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, int64_t, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceMean)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceMean)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceMean)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, ReduceMean)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, float, ReduceMin)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, double, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, MLFloat16, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, int32_t, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceProd)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceProd)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceProd)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, ReduceProd)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceSum)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, ReduceSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int64_t, ReduceSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, ReduceSumSquare)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, ReduceSumSquare)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, ReduceSumSquare)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, Scan)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, ScatterElements)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, Slice)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int64_t, Slice)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, Slice)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, Softmax)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, Softmax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, Softmax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, LogSoftmax)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, LogSoftmax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, LogSoftmax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, Split)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, Squeeze)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, TopK)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, SequenceAt)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, SequenceConstruct)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, SequenceEmpty)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, SequenceLength)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, ConcatFromSequence)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, SequenceErase)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, SequenceInsert)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, Unsqueeze)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, float, Conv)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, double, Conv)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, MLFloat16, Conv)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, float, ConvTranspose)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, double, ConvTranspose)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, MLFloat16, ConvTranspose)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, float, AveragePool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, double, AveragePool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, MLFloat16, AveragePool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, float, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, double, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, MLFloat16, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, Resize)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, Resize)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, Resize)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, Resize)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, uint8_t, Resize)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 11, Clip)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, Pad)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, Pad)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, Pad)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, bool, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int32_t, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, int64_t, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, uint32_t, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, uint64_t, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, float, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, double, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, MLFloat16, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, float, Round)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, double, Round)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, MLFloat16, Round)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 13, CumSum)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, int64_t_int64_t_int64_t, OneHot)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, int64_t_float_int64_t, OneHot)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, int32_t_float_int32_t, OneHot)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, int64_t_MLFloat16_int64_t, OneHot)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, int32_t_MLFloat16_int32_t, OneHot)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, ScatterND)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 11, 12, DepthToSpace)>,

    // OpSet 12
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, Clip)>,

    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, float, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, double, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, MLFloat16, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, int8_t, MaxPool)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, uint8_t, MaxPool)>,

    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, Pow)>,

    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, float, ReduceMax)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, double, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, MLFloat16, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, int32_t, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, int64_t, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, int8_t, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, uint8_t, ReduceMax)>,

    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, float, ReduceMin)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, double, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, MLFloat16, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, int32_t, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, int64_t, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, int8_t, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, uint8_t, ReduceMin)>,

    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, int64_t, GatherND)>,

    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, 12, Dropout)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 12, Einsum)>,

    // OpSet 13
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 14, Pow)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int32_t, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int64_t, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint32_t, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint64_t, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, float, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, double, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Clip)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, MLFloat16, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int32_t, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int64_t, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint32_t, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint64_t, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, float, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, double, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, MLFloat16, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int32_t, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int64_t, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint32_t, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint64_t, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, float, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, double, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, MLFloat16, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int32_t, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int64_t, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint32_t, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint64_t, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, float, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, double, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, MLFloat16, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int8_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int16_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint8_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint16_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint32_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint64_t, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Abs)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int8_t, Neg)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int16_t, Neg)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Neg)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, Neg)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Neg)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Neg)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Neg)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Floor)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Floor)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Floor)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Ceil)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Ceil)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Ceil)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Reciprocal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Reciprocal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Reciprocal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Sqrt)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Sqrt)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Sqrt)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Log)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Log)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Log)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Exp)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Exp)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Exp)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Erf)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Erf)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Erf)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Expand)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Sum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Max)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Min)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, bool, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint32_t, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint64_t, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Equal)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint32_t, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint64_t, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Greater)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint32_t, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint64_t, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Less)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, bool, NonZero)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint8_t, NonZero)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, NonZero)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, NonZero)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, NonZero)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int8_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int16_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint8_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint16_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint32_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint64_t, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, bool, Cast)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, Reshape)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 14, Shape)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Size)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Transpose)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, ScatterElements)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Slice)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, Slice)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Slice)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Softmax)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Softmax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Softmax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, LogSoftmax)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, LogSoftmax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, LogSoftmax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Split)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Squeeze)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Unsqueeze)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Concat)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Gather)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, GatherElements)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, MatMul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, MatMul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, MatMul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, float, Relu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, double, Relu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, MLFloat16, Relu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Sigmoid)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Sigmoid)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Sigmoid)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Tanh)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Tanh)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Tanh)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Gemm)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Gemm)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Gemm)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceL1)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceL1)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceL1)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, ReduceL1)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceL2)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceL2)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceL2)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, ReduceL2)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceLogSum)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceLogSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceLogSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceLogSumExp)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceLogSumExp)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceLogSumExp)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceMax)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int8_t, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint8_t, ReduceMax)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceMean)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceMean)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceMean)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, ReduceMean)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, float, ReduceMin)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, double, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, MLFloat16, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int32_t, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, int8_t, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, uint8_t, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceProd)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceProd)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceProd)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, ReduceProd)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceSum)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, ReduceSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, ReduceSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, ReduceSumSquare)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, ReduceSumSquare)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, ReduceSumSquare)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int64_t, GatherND)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Dropout)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Resize)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Resize)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Resize)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, int32_t, Resize)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, uint8_t, Resize)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, If)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Loop)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, Flatten)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, LRN)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, LRN)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, LRN)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, Identity)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, ScatterND)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, float, Pad)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, double, Pad)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, MLFloat16, Pad)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, bool, Pad)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, SpaceToDepth)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, DepthToSpace)>,

    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, BFloat16, Add)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, BFloat16, Sub)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, BFloat16, Mul)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, BFloat16, Div)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, BFloat16, Cast)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, BFloat16, Softmax)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, BFloat16, MatMul)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, 13, BFloat16, Relu)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, BFloat16, Sigmoid)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, BFloat16, Tanh)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, BFloat16, Gemm)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 13, BFloat16, ReduceSum)>,

    // OpSet 14
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, CumSum)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, Relu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, Relu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, Relu)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int32_t, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int64_t, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint32_t, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint64_t, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, Add)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int32_t, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int64_t, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint32_t, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint64_t, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, Sub)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int32_t, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int64_t, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint32_t, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint64_t, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, Mul)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int32_t, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int64_t, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint32_t, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint64_t, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, Div)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, Identity)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, RNN)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, RNN)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, RNN)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, GRU)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, GRU)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, GRU)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, LSTM)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, LSTM)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, LSTM)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, Reshape)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, 14, float, BatchNormalization)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, 14, double, BatchNormalization)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, 14, MLFloat16, BatchNormalization)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, float, ReduceMin)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, double, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, MLFloat16, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int32_t, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int8_t, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, uint8_t, ReduceMin)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, int64_t, ReduceMin)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, BFloat16, Add)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, BFloat16, Sub)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, BFloat16, Mul)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, BFloat16, Div)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 14, BFloat16, Relu)>,

    // OpSet 15
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 15, Pow)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 15, float, BatchNormalization)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 15, double, BatchNormalization)>,
    // BuildKernelCreateInfo<ONNX_OPERATOR_TYPED_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 15, MLFloat16, BatchNormalization)>,
    BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kRocmExecutionProvider, kOnnxDomain, 15, Shape)>,

  };

  for (auto& function_table_entry : function_table) {
    KernelCreateInfo info = function_table_entry();
    if (info.kernel_def != nullptr) {  // filter disabled entries where type is void
      ORT_RETURN_IF_ERROR(kernel_registry.Register(std::move(info)));
    }
  }

#ifndef DISABLE_CONTRIB_OPS
  ORT_RETURN_IF_ERROR(::onnxruntime::contrib::rocm::RegisterRocmContribKernels(kernel_registry));
#endif

#ifdef ENABLE_TRAINING
  ORT_RETURN_IF_ERROR(::onnxruntime::rocm::RegisterRocmTrainingKernels(kernel_registry));
#endif

  return Status::OK();
}

}  // namespace rocm

static std::shared_ptr<onnxruntime::KernelRegistry> s_kernel_registry;

void Shutdown_DeleteRegistry() {
  s_kernel_registry.reset();
}

std::shared_ptr<KernelRegistry> ROCMExecutionProvider::GetKernelRegistry() const {
  if (!s_kernel_registry) {
    s_kernel_registry = KernelRegistry::Create();
    auto status = rocm::RegisterRocmKernels(*s_kernel_registry);
    if (!status.IsOK())
      s_kernel_registry.reset();
    ORT_THROW_IF_ERROR(status);
  }

  return s_kernel_registry;
}

static bool CastNeedFallbackToCPU(const onnxruntime::Node& node) {
  const auto& node_attributes = node.GetAttributes();
  // Check attributes
  for (auto& attr : node_attributes) {
    auto& attr_name = attr.first;
    auto& attr_value = attr.second;

    // string is not supported
    if ("to" == attr_name && ::ONNX_NAMESPACE::AttributeProto_AttributeType::AttributeProto_AttributeType_INT == attr_value.type()) {
      auto to_type = attr_value.i();
      if (to_type == ::ONNX_NAMESPACE::TensorProto_DataType_STRING)
        return true;
    }
  }

  return false;
}

std::unique_ptr<onnxruntime::IDataTransfer> ROCMExecutionProvider::GetDataTransfer() const {
  return std::make_unique<onnxruntime::GPUDataTransfer>(static_cast<hipStream_t>(GetComputeStream()), info_.do_copy_in_default_stream);
}

std::vector<std::unique_ptr<ComputeCapability>>
ROCMExecutionProvider::GetCapability(const onnxruntime::GraphViewer& graph,
                                     const std::vector<const KernelRegistry*>& kernel_registries) const {
  std::vector<NodeIndex> candidates;
  for (auto& node_index : graph.GetNodesInTopologicalOrder()) {
    const auto* p_node = graph.GetNode(node_index);
    if (p_node == nullptr)
      continue;

    const auto& node = *p_node;
    const KernelCreateInfo* rocm_kernel_def = nullptr;
    if (!node.GetExecutionProviderType().empty()) {
      continue;
    }

    for (auto registry : kernel_registries) {
      auto st = registry->TryFindKernel(node, Type(), &rocm_kernel_def);

      // at least one registry has a ROCM kernel for this node
      if (st.IsOK())
        break;
    }

    // none of the provided registries has a ROCM kernel for this node
    if (rocm_kernel_def == nullptr) {
      LOGS_DEFAULT(INFO) << "ROCM kernel not found in registries for Op type: " << node.OpType() << " node name: " << node.Name();
      continue;
    }

    bool not_supported = false;
    bool force_inside = false;  // for some compute heavy ops, we'll force it to run inside ROCM
    if ("LSTM" == node.OpType() ||
        "RNN" == node.OpType() ||
        "GRU" == node.OpType()) {
      not_supported = true;
      force_inside = !not_supported;
    } else if ("Cast" == node.OpType()) {
      not_supported = CastNeedFallbackToCPU(node);
      // cast is not compute heavy, and may be placed outside
    }

    if (!force_inside && not_supported) {
      if (not_supported) {
        LOGS_DEFAULT(WARNING) << "ROCM kernel not supported. Fallback to CPU execution provider for Op type: " << node.OpType() << " node name: " << node.Name();
      }
    } else {
      candidates.push_back(node.Index());
    }
  }

  // For ROCM EP, exclude the subgraph that is preferred to be placed in CPU
  // These are usually shape related computation subgraphs
  // Following logic can be extended for other EPs
  std::unordered_set<NodeIndex> cpu_nodes = GetCpuPreferredNodes(graph, Type(), kernel_registries, candidates);

  std::vector<std::unique_ptr<ComputeCapability>> result;
  for (auto& node_index : candidates) {
    if (cpu_nodes.count(node_index) > 0)
      continue;

    auto sub_graph = IndexedSubGraph::Create();
    sub_graph->Nodes().push_back(node_index);
    result.push_back(ComputeCapability::Create(std::move(sub_graph)));
  }
  return result;
}

void ROCMExecutionProvider::RegisterAllocator(std::shared_ptr<AllocatorManager> allocator_manager) {
  // Try to get a ROCM allocator from allocator manager first
  // Used to allocate ROCM device memory
  auto rocm_alloc = allocator_manager->GetAllocator(info_.device_id, OrtMemTypeDefault);
  if (nullptr == rocm_alloc) {
    rocm_alloc = CreateRocmAllocator(info_.device_id, info_.gpu_mem_limit, info_.arena_extend_strategy,
                                     info_.external_allocator_info, info_.default_memory_arena_cfg);
    allocator_manager->InsertAllocator(rocm_alloc);
  }
  TryInsertAllocator(rocm_alloc);

  // OrtMemTypeCPUOutput -- allocated by hipHostMalloc, used to copy ROCM device memory to CPU
  // Use pinned memory instead of pageable memory make the data transfer faster
  // Used by node MemcpyToHost only
  auto rocm_pinned_alloc = allocator_manager->GetAllocator(DEFAULT_CPU_ALLOCATOR_DEVICE_ID, OrtMemTypeCPUOutput);
  if (nullptr == rocm_pinned_alloc) {
    AllocatorCreationInfo pinned_memory_info(
        [](OrtDevice::DeviceId device_id) {
          return std::make_unique<ROCMPinnedAllocator>(device_id, CUDA_PINNED);
        },
        DEFAULT_CPU_ALLOCATOR_DEVICE_ID);

    rocm_pinned_alloc = CreateAllocator(pinned_memory_info);
    allocator_manager->InsertAllocator(rocm_pinned_alloc);
  }
  TryInsertAllocator(rocm_pinned_alloc);

  // OrtMemTypeCPUInput -- ROCM op place the input on CPU and will not be accessed by ROCM kernel, no sync issue
  auto rocm_cpu_alloc = allocator_manager->GetAllocator(DEFAULT_CPU_ALLOCATOR_DEVICE_ID, OrtMemTypeCPUInput);
  if (nullptr == rocm_cpu_alloc) {
    // TODO: this is actually used for the rocm kernels which explicitly ask for inputs from CPU.
    // This will be refactored/removed when allocator and execution provider are decoupled.
    // Need to move the OrtMemoryType out of Allocator, that's one thing blocking us to share it with CPU EP
    // CPUAllocator is OrtMemTypeDefault for CPU EP
    AllocatorCreationInfo cpu_memory_info(
        [](int device_id) {
          return std::make_unique<CPUAllocator>(
              OrtMemoryInfo("ROCM_CPU", OrtAllocatorType::OrtDeviceAllocator, OrtDevice(), device_id,
                            OrtMemTypeCPUInput));
        },
        DEFAULT_CPU_ALLOCATOR_DEVICE_ID);

    rocm_cpu_alloc = CreateAllocator(cpu_memory_info);
    allocator_manager->InsertAllocator(rocm_cpu_alloc);
  }
  TryInsertAllocator(rocm_cpu_alloc);
}

}  // namespace onnxruntime
