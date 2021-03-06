/**
 * Copyright 2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/runtime/kernel/arm/base/switch.h"
#include "src/kernel_registry.h"
#include "include/errorcode.h"

using mindspore::lite::KernelRegistrar;
using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;
using mindspore::schema::PrimitiveType_Switch;

namespace mindspore::kernel {
int SwitchCPUKernel::PostProcess() {
  auto bool_tensor = in_tensors_.front();
  MS_ASSERT(bool_tensor != nullptr);
  MS_ASSERT(bool_tensor->data_type() == kNumberTypeBool);
  MS_ASSERT(bool_tensor->shape().size() == 1);
  MS_ASSERT(bool_tensor->shape().front() == 1);
  auto *active = static_cast<bool *>(bool_tensor->data_c());
  if (active == nullptr) {
    MS_LOG(ERROR) << "data of bool tensor is nullptr";
    return lite::RET_NULL_PTR;
  }
  size_t in_index = 1;
  size_t out_index = (*active) ? 0 : (out_tensors_.size() / 2);
  while (in_index < in_tensors_.size()) {
    in_index++;
    auto out_tensor = out_tensors_.at(out_index++);
    out_tensor->ResetRefCount();
  }
  return FreeInWorkTensor();
}

int SwitchCPUKernel::Init() { return RET_OK; }

int SwitchCPUKernel::ReSize() { return RET_ERROR; }

// inputs: bool*1 data*n
// output: true-data*n, false-data*n
int SwitchCPUKernel::Run() {
  MS_ASSERT(in_tensors_.size() >= 2);
  MS_ASSERT(out_tensors_.size() == 2 * in_tensors_.size());
  auto bool_tensor = in_tensors_.front();
  MS_ASSERT(bool_tensor != nullptr);
  MS_ASSERT(bool_tensor->data_type() == kNumberTypeBool);
  MS_ASSERT(bool_tensor->shape().size() == 1);
  MS_ASSERT(bool_tensor->shape().front() == 1);
  auto active = static_cast<bool *>(bool_tensor->data_c());
  if (active == nullptr) {
    MS_LOG(ERROR) << "data of bool tensor is nullptr";
    return lite::RET_NULL_PTR;
  }
  size_t in_index = 1;
  size_t out_index = (*active) ? 0 : (out_tensors_.size() / 2);
  while (in_index < in_tensors_.size()) {
    auto in_tensor = in_tensors_.at(in_index++);
    auto out_tensor = out_tensors_.at(out_index++);
    MS_ASSERT(in_tensor != nullptr);
    MS_ASSERT(out_tensor != nullptr);
    auto input = reinterpret_cast<float *>(in_tensor->data_c());
    auto output = reinterpret_cast<float *>(out_tensor->data_c());
    MS_ASSERT(in_tensor->Size() == out_tensor->Size());
    if (input == nullptr || output == nullptr) {
      MS_LOG(ERROR) << "input tensor or output tensor have not been malloced";
      return lite::RET_NULL_PTR;
    }
    memcpy(output, input, in_tensor->Size());
  }
  return RET_OK;
}

kernel::LiteKernel *CpuSwitchKernelCreator(const std::vector<lite::Tensor *> &inputs,
                                           const std::vector<lite::Tensor *> &outputs, OpParameter *parameter,
                                           const lite::InnerContext *ctx, const KernelKey &desc,
                                           const mindspore::lite::PrimitiveC *primitive) {
  if (parameter == nullptr) {
    MS_LOG(ERROR) << "parameter is nullptr";
    return nullptr;
  }
  if (desc.type != PrimitiveType_Switch) {
    MS_LOG(ERROR) << "type in desc is not Switch";
    free(parameter);
    return nullptr;
  }
  if (ctx == nullptr) {
    MS_LOG(ERROR) << "ctx is nullptr";
    free(parameter);
    return nullptr;
  }
  auto *kernel = new (std::nothrow) SwitchCPUKernel(parameter, inputs, outputs, ctx, primitive);
  if (kernel == nullptr) {
    MS_LOG(ERROR) << "Create kernel failed, name: " << parameter->name_;
    free(parameter);
    return nullptr;
  }
  return kernel;
}

REG_KERNEL(kCPU, kNumberTypeFloat32, PrimitiveType_Switch, CpuSwitchKernelCreator)
REG_KERNEL(kCPU, kNumberTypeBool, PrimitiveType_Switch, CpuSwitchKernelCreator)
}  // namespace mindspore::kernel
