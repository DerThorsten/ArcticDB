/* Copyright 2023 Man Group Operations Limited
 *
 * Use of this software is governed by the Business Source License 1.1 included in the file licenses/BSL.txt.
 *
 * As of the Change Date specified in that file, in accordance with the Business Source License, use of this software will be governed by the Apache License, version 2.0.
 */

#pragma once

#include <arcticdb/column_store/memory_segment.hpp>
#include <arcticdb/entity/protobufs.hpp>
#include <arcticdb/util/buffer_holder.hpp>

namespace arcticdb {

struct FrameAndDescriptor {
    SegmentInMemory frame_;
    arcticdb::proto::descriptors::TimeSeriesDescriptor desc_;
    std::vector<AtomKey> keys_;
    std::shared_ptr<BufferHolder> buffers_;
};

} //namespace arcticdb
