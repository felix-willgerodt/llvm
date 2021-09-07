// RUN: %clangxx -fsycl -fsycl-targets=%sycl_triple %s -o %t.out
// RUN: %HOST_RUN_PLACEHOLDER %t.out
// RUN: %CPU_RUN_PLACEHOLDER %t.out
// RUNx: %GPU_RUN_PLACEHOLDER %t.out
// RUN: %ACC_RUN_PLACEHOLDER %t.out
//==------- attributes.cpp - SYCL sub_group attributes test ----*- C++ -*---==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "helper.hpp"

#include <CL/sycl.hpp>

#define KERNEL_FUNCTOR_WITH_SIZE(SIZE)                                         \
  class KernelFunctor##SIZE {                                                  \
  public:                                                                      \
    [[intel::reqd_sub_group_size(SIZE)]] void                                  \
    operator()(cl::sycl::nd_item<1> Item) const {                              \
      const auto GID = Item.get_global_id();                                   \
    }                                                                          \
  };

KERNEL_FUNCTOR_WITH_SIZE(1);
KERNEL_FUNCTOR_WITH_SIZE(2);
KERNEL_FUNCTOR_WITH_SIZE(4);
KERNEL_FUNCTOR_WITH_SIZE(8);
KERNEL_FUNCTOR_WITH_SIZE(16);
KERNEL_FUNCTOR_WITH_SIZE(32);
KERNEL_FUNCTOR_WITH_SIZE(64);

#undef KERNEL_FUNCTOR_WITH_SIZE

inline uint32_t flp2(uint32_t X) {
  X = X | (X >> 1);
  X = X | (X >> 2);
  X = X | (X >> 4);
  X = X | (X >> 8);
  X = X | (X >> 16);
  return X - (X >> 1);
}

template <typename Fn> inline void submit(cl::sycl::queue &Q) {
  Q.submit([](cl::sycl::handler &cgh) {
    Fn F;
    cgh.parallel_for(cl::sycl::nd_range<1>{64, 16}, F);
  });
}

int main() {
  queue Queue;
  device Device = Queue.get_device();

  // According to specification, this kernel query requires `cl_khr_subgroups`
  // or `cl_intel_subgroups`, and also `cl_intel_required_subgroup_size`
  auto Vec = Device.get_info<info::device::extensions>();
  if (std::find(Vec.begin(), Vec.end(), "cl_intel_subgroups") ==
              std::end(Vec) &&
          std::find(Vec.begin(), Vec.end(), "cl_khr_subgroups") ==
              std::end(Vec) ||
      std::find(Vec.begin(), Vec.end(), "cl_intel_required_subgroup_size") ==
          std::end(Vec)) {
    std::cout << "Skipping test\n";
    return 0;
  }

  try {
    const auto SGSizes = Device.get_info<info::device::sub_group_sizes>();

    for (const auto SGSize : SGSizes) {
      // Get the previous power of 2
      auto ReqdSize = flp2(SGSize);

      cl::sycl::program Prog(Queue.get_context());

      // Store the `cl::sycl::kernel` into a vector because `cl::sycl::kernel`
      // doesn't have default constructor
      std::vector<cl::sycl::kernel> TheKernel;

      switch (ReqdSize) {
      case 64:
        Prog.build_with_kernel_type<KernelFunctor64>();
        TheKernel.push_back(Prog.get_kernel<KernelFunctor64>());
        submit<KernelFunctor64>(Queue);
        break;
      case 32:
        Prog.build_with_kernel_type<KernelFunctor32>();
        TheKernel.push_back(Prog.get_kernel<KernelFunctor32>());
        submit<KernelFunctor32>(Queue);
        break;
      case 16:
        Prog.build_with_kernel_type<KernelFunctor16>();
        TheKernel.push_back(Prog.get_kernel<KernelFunctor16>());
        submit<KernelFunctor16>(Queue);
        break;
      case 8:
        Prog.build_with_kernel_type<KernelFunctor8>();
        TheKernel.push_back(Prog.get_kernel<KernelFunctor8>());
        submit<KernelFunctor8>(Queue);
        break;
      case 4:
        Prog.build_with_kernel_type<KernelFunctor4>();
        TheKernel.push_back(Prog.get_kernel<KernelFunctor4>());
        submit<KernelFunctor4>(Queue);
        break;
      case 2:
        Prog.build_with_kernel_type<KernelFunctor2>();
        TheKernel.push_back(Prog.get_kernel<KernelFunctor2>());
        submit<KernelFunctor2>(Queue);
        break;
      case 1:
        Prog.build_with_kernel_type<KernelFunctor1>();
        TheKernel.push_back(Prog.get_kernel<KernelFunctor1>());
        submit<KernelFunctor1>(Queue);
        break;
      default:
        throw feature_not_supported("sub-group size is not supported",
                                    PI_INVALID_OPERATION);
      }

      auto Kernel = TheKernel[0];

      auto Res = Kernel.get_sub_group_info<
          cl::sycl::info::kernel_sub_group::compile_sub_group_size>(Device);

      exit_if_not_equal<size_t>(Res, ReqdSize, "compile_sub_group_size");
    }
  } catch (exception e) {
    std::cout << "SYCL exception caught: " << e.what();
    return 1;
  }

  std::cout << "Test passed.\n";
  return 0;
}
