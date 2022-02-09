/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <ATen/ATen.h>
#include <ATen/TypeDefault.h>
#include <ATen/core/op_registration/op_registration.h>
#include <torch/script.h>
#include "fbgemm_gpu/embedding_common.h"

using Tensor = at::Tensor;

namespace {

void bounds_check_indices_cpu(
    Tensor rows_per_table,
    Tensor indices,
    Tensor offsets,
    int64_t bounds_check_mode_,
    Tensor warning) {
  auto bounds_check_mode = static_cast<BoundsCheckMode>(bounds_check_mode_);
  if (bounds_check_mode == BoundsCheckMode::WARNING) {
    warning.zero_();
  }

  int32_t T = rows_per_table.size(0);
  int32_t B = (offsets.size(0) - 1) / T;
  const auto rows_per_table_acc = rows_per_table.accessor<int64_t, 1>();
  auto warning_acc = warning.data_ptr<int64_t>();

  AT_DISPATCH_INDEX_TYPES(indices.scalar_type(), "bounds_check_indices", [&]() {
    auto offsets_acc = offsets.accessor<index_t, 1>();
    auto indices_acc = indices.accessor<index_t, 1>();
    auto num_indices = indices.numel();
    for (auto t = 0; t < T; ++t) {
      auto num_rows = rows_per_table_acc[t];
      for (auto b = 0; b < B; ++b) {
        auto indices_start = offsets_acc[t * B + b];
        auto indices_end = offsets_acc[t * B + b + 1];
        if (bounds_check_mode == BoundsCheckMode::FATAL) {
          TORCH_CHECK(indices_start >= 0);
          TORCH_CHECK(indices_start <= indices_end);
          TORCH_CHECK(indices_end <= num_indices);
        } else if (bounds_check_mode == BoundsCheckMode::WARNING) {
          if (indices_start < 0 || indices_start > indices_end ||
              indices_end > num_indices) {
            if (__sync_fetch_and_add(&warning_acc[0], 1) == 0) {
              LOG(ERROR)
                  << "(at least one) Out of bounds access for batch: " << b
                  << ", table: " << t << ", indices_start: " << indices_start
                  << ", indices_end: " << indices_end
                  << ", num_indices: " << num_indices
                  << ". Setting indices_start and indices_end within the range";
            }
            indices_start = std::max(
                0L, std::min(static_cast<int64_t>(indices_start), num_indices));
            indices_end = std::max(
                static_cast<int64_t>(indices_start),
                std::min(static_cast<int64_t>(indices_end), num_indices));
            offsets_acc[t * B + b] = indices_start;
            offsets_acc[t * B + b + 1] = indices_end;
          }
        } else if (bounds_check_mode == BoundsCheckMode::IGNORE) {
          indices_start = std::max(
              0L, std::min(static_cast<int64_t>(indices_start), num_indices));
          indices_end = std::max(
              static_cast<int64_t>(indices_start),
              std::min(static_cast<int64_t>(indices_end), num_indices));
          offsets_acc[t * B + b] = indices_start;
          offsets_acc[t * B + b + 1] = indices_end;
        }

        auto L = indices_end - indices_start;
        for (auto l = 0; l < L; ++l) {
          auto idx = indices_acc[indices_start + l];
          if (idx == -1) {
            // -1 indicates pruned rows.
            continue;
          }
          if (bounds_check_mode == BoundsCheckMode::FATAL) {
            TORCH_CHECK(idx >= 0);
            TORCH_CHECK(idx < num_rows);
          } else if (bounds_check_mode == BoundsCheckMode::WARNING) {
            if (idx < 0 || idx >= num_rows) {
              if (__sync_fetch_and_add(&warning_acc[0], 1) == 0) {
                LOG(ERROR) << "(at least one) Out of bounds access for batch: "
                           << b << ", table: " << t << ", bag element: " << l
                           << ", idx: " << idx << ", num_rows: " << num_rows
                           << ". Setting idx to zero.";
              }
              indices_acc[indices_start + l] = 0;
            }
          } else if (bounds_check_mode == BoundsCheckMode::IGNORE) {
            if (idx < 0 || idx >= num_rows) {
              indices_acc[indices_start + l] = 0;
            }
          }
        }
      }
    }
  });
}
} // namespace

TORCH_LIBRARY_FRAGMENT(fb, m) {
  // The (a!) tells PyTorch this is an impure operation and so cannot be CSE'd
  // or DCE'd, etc.
  m.def(
      "bounds_check_indices(Tensor rows_per_table, Tensor(a!) indices, Tensor(a!) offsets, int bounds_check_mode, Tensor(a!) warning) -> ()");
  m.impl(
      "bounds_check_indices",
      torch::dispatch(
          c10::DispatchKey::CPU, TORCH_FN(bounds_check_indices_cpu)));
}

TORCH_LIBRARY_FRAGMENT(fbgemm, m) {
  // The (a!) tells PyTorch this is an impure operation and so cannot be CSE'd
  // or DCE'd, etc.
  m.def(
      "bounds_check_indices(Tensor rows_per_table, Tensor(a!) indices, Tensor(a!) offsets, int bounds_check_mode, Tensor(a!) warning) -> ()");
  m.impl(
      "bounds_check_indices",
      torch::dispatch(
          c10::DispatchKey::CPU, TORCH_FN(bounds_check_indices_cpu)));
}
