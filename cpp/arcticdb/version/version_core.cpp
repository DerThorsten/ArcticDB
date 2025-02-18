/* Copyright 2023 Man Group Operations Limited
 *
 * Use of this software is governed by the Business Source License 1.1 included in the file licenses/BSL.txt.
 *
 * As of the Change Date specified in that file, in accordance with the Business Source License, use of this software will be governed by the Apache License, version 2.0.
 */

#include <arcticdb/version/version_core.hpp>
#include <arcticdb/pipeline/write_options.hpp>
#include <arcticdb/stream/index.hpp>
#include <arcticdb/pipeline/query.hpp>
#include <arcticdb/pipeline/read_pipeline.hpp>
#include <arcticdb/async/task_scheduler.hpp>
#include <arcticdb/async/tasks.hpp>
#include <arcticdb/util/key_utils.hpp>
#include <arcticdb/util/optional_defaults.hpp>
#include <arcticdb/stream/append_map.hpp>
#include <arcticdb/pipeline/pipeline_context.hpp>
#include <arcticdb/pipeline/read_frame.hpp>
#include <arcticdb/pipeline/read_options.hpp>
#include <arcticdb/stream/segment_aggregator.hpp>
#include <arcticdb/stream/stream_sink.hpp>
#include <arcticdb/stream/stream_writer.hpp>
#include <arcticdb/entity/type_utils.hpp>
#include <arcticdb/stream/schema.hpp>
#include <arcticdb/pipeline/index_writer.hpp>
#include <arcticdb/entity/metrics.hpp>
#include <arcticdb/pipeline/index_utils.hpp>
#include <arcticdb/util/composite.hpp>
#include <arcticdb/pipeline/column_mapping.hpp>
#include <arcticdb/version/schema_checks.hpp>

namespace arcticdb::version_store {

void modify_descriptor(const std::shared_ptr<pipelines::PipelineContext>& pipeline_context, const ReadOptions& read_options) {

    if (opt_false(read_options.force_strings_to_object_) || opt_false(read_options.force_strings_to_fixed_))
        pipeline_context->orig_desc_ = pipeline_context->desc_;

    auto& desc = *pipeline_context->desc_;
    if (opt_false(read_options.force_strings_to_object_)) {
        auto& fields = desc.mutable_fields();
        std::transform(
            std::begin(fields),
            std::end(fields),
            std::begin(fields),
            [](auto& field_desc) {
                if (data_type_from_proto(field_desc.type_desc()) == DataType::ASCII_FIXED64)
                    set_data_type(DataType::ASCII_DYNAMIC64, *field_desc.mutable_type_desc());

                if (data_type_from_proto(field_desc.type_desc()) == DataType::UTF_FIXED64)
                    set_data_type(DataType::UTF_DYNAMIC64, *field_desc.mutable_type_desc());

                return field_desc;
            });
    }
    else if (opt_false(read_options.force_strings_to_fixed_)) {
        auto& fields = desc.mutable_fields();
        std::transform(
            std::begin(fields),
            std::end(fields),
            std::begin(fields),
            [](auto& field_desc) {
                if (data_type_from_proto(field_desc.type_desc()) == DataType::ASCII_DYNAMIC64)
                    set_data_type(DataType::ASCII_FIXED64, *field_desc.mutable_type_desc());

                if (data_type_from_proto(field_desc.type_desc()) == DataType::UTF_DYNAMIC64)
                    set_data_type(DataType::UTF_FIXED64, *field_desc.mutable_type_desc());

                return field_desc;
            });
    }
}

VersionedItem write_dataframe_impl(
    const std::shared_ptr<Store>& store,
    VersionId version_id,
    pipelines::InputTensorFrame&& frame,
    const WriteOptions& options,
    const std::shared_ptr<DeDupMap>& de_dup_map,
    bool sparsify_floats,
    bool validate_index
    ) {
    auto atom_key_fut = async_write_dataframe_impl(store, version_id, std::move(frame), options, de_dup_map, sparsify_floats, validate_index);

    ARCTICDB_SUBSAMPLE_DEFAULT(WaitForWriteCompletion)
    auto atom_key = std::move(atom_key_fut).get();
    auto versioned_item = VersionedItem(std::move(atom_key));
    ARCTICDB_DEBUG(log::version(), "write_dataframe_impl stream_id: {} , version_id: {}, {} rows", frame.desc.id(), version_id, frame.num_rows);
    return versioned_item;
}

folly::Future<entity::AtomKey> async_write_dataframe_impl(
    const std::shared_ptr<Store>& store,
    VersionId version_id,
    InputTensorFrame&& frame,
    const WriteOptions& options,
    const std::shared_ptr<DeDupMap> &de_dup_map,
    bool sparsify_floats,
    bool validate_index
    ) {
    ARCTICDB_SAMPLE(DoWrite, 0)

    // Slice the frame according to the write options
    frame.set_bucketize_dynamic(options.bucketize_dynamic);
    auto slicing_arg = get_slicing_policy(options, frame);
    auto partial_key = IndexPartialKey{frame.desc.id(), version_id};
    sorting::check<ErrorCode::E_UNSORTED_DATA>(!validate_index || frame.desc.get_sorted() == arcticdb::proto::descriptors::SortedValue::ASCENDING || !std::holds_alternative<stream::TimeseriesIndex>(frame.index),
                "When calling write with validate_index enabled, input data must be sorted.");
    return write_frame(partial_key, std::move(frame), slicing_arg, store, de_dup_map, sparsify_floats);
}

namespace {
IndexDescriptor::Proto check_index_match(const arcticdb::stream::Index& index, const IndexDescriptor::Proto& desc) {
    if (std::holds_alternative<stream::TimeseriesIndex>(index))
        util::check(desc.kind() == IndexDescriptor::TIMESTAMP,
                    "Index mismatch, cannot update a non-timeseries-indexed frame with a timeseries");
    else
        util::check(desc.kind() == IndexDescriptor::ROWCOUNT,
                    "Index mismatch, cannot update a timeseries with a non-timeseries-indexed frame");

    return desc;
}
}

folly::Future<AtomKey> async_append_impl(
    const std::shared_ptr<Store>& store,
    const UpdateInfo& update_info,
    InputTensorFrame&& frame,
    const WriteOptions& options,
    bool validate_index) {

    util::check(update_info.previous_index_key_.has_value(), "Cannot append as there is no previous index key to append to");
    const StreamId stream_id = frame.desc.id();
    ARCTICDB_DEBUG(log::version(), "append stream_id: {} , version_id: {}", stream_id, update_info.next_version_id_);
    auto index_segment_reader = index::get_index_reader(*(update_info.previous_index_key_), store);
    bool bucketize_dynamic = index_segment_reader.bucketize_dynamic();
    auto row_offset = index_segment_reader.tsd().total_rows();
    util::check_rte(!index_segment_reader.is_pickled(), "Cannot append to pickled data");
    sorting::check<ErrorCode::E_UNSORTED_DATA>(!validate_index || (frame.desc.get_sorted() == arcticdb::proto::descriptors::SortedValue::ASCENDING && index_segment_reader.tsd().stream_descriptor().sorted() == arcticdb::proto::descriptors::SortedValue::ASCENDING) || 
        !std::holds_alternative<stream::TimeseriesIndex>(frame.index),
        "validate_index set but input data index is not sorted.");

    frame.set_offset(static_cast<ssize_t>(row_offset));
    fix_descriptor_mismatch_or_throw(APPEND, options.dynamic_schema, index_segment_reader, frame);

    frame.set_bucketize_dynamic(bucketize_dynamic);
    auto slicing_arg = get_slicing_policy(options, frame);
    return append_frame(IndexPartialKey{stream_id, update_info.next_version_id_}, std::move(frame), slicing_arg, index_segment_reader, store, options.dynamic_schema, options.ignore_sort_order);
}

VersionedItem append_impl(
    const std::shared_ptr<Store>& store,
    const UpdateInfo& update_info,
    InputTensorFrame&& frame,
    const WriteOptions& options,
    bool validate_index) {

    ARCTICDB_SUBSAMPLE_DEFAULT(WaitForWriteCompletion)
    auto version_key_fut = async_append_impl(store,
                                             update_info,
                                             std::move(frame),
                                             options,
                                             validate_index);
    auto version_key = std::move(version_key_fut).get();
    auto versioned_item = VersionedItem(to_atom(std::move(version_key)));
    ARCTICDB_DEBUG(log::version(), "write_dataframe_impl stream_id: {} , version_id: {}", versioned_item.symbol(), update_info.next_version_id_);
    return versioned_item;
}

namespace {
bool is_before(const IndexRange& a, const IndexRange& b) {
    return a.start_ < b.start_;
}

bool is_after(const IndexRange& a, const IndexRange& b) {
    return a.end_ > b.end_;
}

template <class KeyContainer>
    void ensure_keys_line_up(const KeyContainer& slice_and_keys) {
    std::optional<size_t> start;
    std::optional<size_t> end;
    SliceAndKey prev{};
    for(const auto& sk : slice_and_keys) {
        util::check(!start || sk.slice_.row_range.first == end.value(),
                    "Can't update as there is a sorting mismatch at key {} relative to previous key {} - expected index {} got {}",
                    sk, prev, end.value(), start.value());

        start = sk.slice_.row_range.first;
        end = sk.slice_.row_range.second;
        prev = sk;
    }
}

inline std::pair<std::vector<SliceAndKey>, std::vector<SliceAndKey>> intersecting_segments(
    const std::vector<SliceAndKey>& affected_keys,
    const IndexRange& front_range,
    const IndexRange& back_range,
    VersionId version_id,
    const std::shared_ptr<Store>& store) {
    std::vector<SliceAndKey> intersect_before;
    std::vector<SliceAndKey> intersect_after;

    for (const auto& affected_slice_and_key : affected_keys) {
        const auto& affected_range = affected_slice_and_key.key().index_range();
        if (intersects(affected_range, front_range) && !overlaps(affected_range, front_range)
        && is_before(affected_range, front_range)) {
            auto front_overlap_key = rewrite_partial_segment(affected_slice_and_key, front_range, version_id, false, store);
            if (front_overlap_key)
                intersect_before.push_back(front_overlap_key.value());
        }

        if (intersects(affected_range, back_range) && !overlaps(affected_range, back_range)
        && is_after(affected_range, back_range)) {
            auto back_overlap_key = rewrite_partial_segment(affected_slice_and_key, back_range, version_id, true, store);
            if (back_overlap_key)
                intersect_after.push_back(back_overlap_key.value());
        }
    }
    return std::make_pair(std::move(intersect_before), std::move(intersect_after));
}

} // namespace

VersionedItem delete_range_impl(
    const std::shared_ptr<Store>& store,
    const AtomKey& prev,
    const UpdateQuery& query,
    const WriteOptions&& ,
    bool dynamic_schema) {

    const StreamId& stream_id = prev.id();
    auto version_id = get_next_version_from_key(prev);
    util::check(std::holds_alternative<IndexRange>(query.row_filter), "Delete range requires index range argument");
    const auto& index_range = std::get<IndexRange>(query.row_filter);
    ARCTICDB_DEBUG(log::version(), "Delete range in versioned dataframe for stream_id: {} , version_id = {}", stream_id, version_id);

    auto index_segment_reader = index::get_index_reader(prev, store);
    util::check_rte(!index_segment_reader.is_pickled(), "Cannot delete date range of pickled data");
    auto index = index_type_from_descriptor(index_segment_reader.tsd().stream_descriptor());
    util::check(std::holds_alternative<TimeseriesIndex>(index), "Delete in range will not work as expected with a non-timeseries index");

    std::vector<FilterQuery<index::IndexSegmentReader>> queries =
            build_update_query_filters<index::IndexSegmentReader>(query.row_filter, index, index_range, dynamic_schema, index_segment_reader.bucketize_dynamic());
    auto combined = combine_filter_functions(queries);
    auto affected_keys = filter_index(index_segment_reader, std::move(combined));
    std::vector<SliceAndKey> unaffected_keys;
    std::set_difference(std::begin(index_segment_reader),
                        std::end(index_segment_reader),
                        std::begin(affected_keys),
                        std::end(affected_keys),
                        std::back_inserter(unaffected_keys));

    auto [intersect_before, intersect_after] = intersecting_segments(affected_keys, index_range, index_range, version_id, store);

    auto orig_filter_range = std::holds_alternative<std::monostate>(query.row_filter) ? get_query_index_range(index, index_range) : query.row_filter;

    size_t row_count = 0;
    auto flattened_slice_and_keys = flatten_and_fix_rows(std::vector<std::vector<SliceAndKey>>{
        strictly_before(orig_filter_range, unaffected_keys),
        std::move(intersect_before),
        std::move(intersect_after),
        strictly_after(orig_filter_range, unaffected_keys)},
                                                         row_count
                                                         );

    std::sort(std::begin(flattened_slice_and_keys), std::end(flattened_slice_and_keys));
    bool bucketize_dynamic = index_segment_reader.bucketize_dynamic();
    auto time_series = make_descriptor(row_count, std::move(index_segment_reader), std::nullopt, bucketize_dynamic);
    auto version_key_fut = util::variant_match(index, [&time_series, &flattened_slice_and_keys, &stream_id, &version_id, &store] (auto idx) {
        using IndexType = decltype(idx);
        return pipelines::index::write_index<IndexType>(std::move(time_series), std::move(flattened_slice_and_keys), IndexPartialKey{stream_id, version_id}, store);
    });
    auto version_key = std::move(version_key_fut).get();
    auto versioned_item = VersionedItem(to_atom(std::move(version_key)));
    ARCTICDB_DEBUG(log::version(), "updated stream_id: {} , version_id: {}", stream_id, version_id);
    return versioned_item;
}

VersionedItem update_impl(
    const std::shared_ptr<Store>& store,
    const UpdateInfo& update_info,
    const UpdateQuery& query,
    InputTensorFrame&& frame,
    const WriteOptions&& options,
    bool dynamic_schema) {
    util::check(update_info.previous_index_key_.has_value(), "Cannot update as there is no previous index key to update into");
    const StreamId stream_id = frame.desc.id();
    ARCTICDB_DEBUG(log::version(), "Update versioned dataframe for stream_id: {} , version_id = {}", stream_id, update_info.previous_index_key_->version_id());

    auto index_segment_reader = index::get_index_reader(*(update_info.previous_index_key_), store);
    util::check_rte(!index_segment_reader.is_pickled(), "Cannot update pickled data");
    auto index_desc = check_index_match(frame.index, index_segment_reader.tsd().stream_descriptor().index());
    util::check(index_desc.kind() == IndexDescriptor::TIMESTAMP, "Update not supported for non-timeseries indexes");
    sorting::check<ErrorCode::E_UNSORTED_DATA>(frame.desc.get_sorted() == arcticdb::proto::descriptors::SortedValue::ASCENDING && std::holds_alternative<stream::TimeseriesIndex>(frame.index) && index_segment_reader.mutable_tsd().stream_descriptor().sorted() == arcticdb::proto::descriptors::SortedValue::ASCENDING,
        "When calling update on sorted data, the input data must be sorted.");
    bool bucketize_dynamic = index_segment_reader.bucketize_dynamic();
    (void)check_and_mark_slices(index_segment_reader, dynamic_schema, false, std::nullopt, bucketize_dynamic);

    fix_descriptor_mismatch_or_throw(UPDATE, dynamic_schema, index_segment_reader, frame);

    std::vector<FilterQuery<index::IndexSegmentReader>> queries =
        build_update_query_filters<index::IndexSegmentReader>(query.row_filter, frame.index, frame.index_range, dynamic_schema, index_segment_reader.bucketize_dynamic());
    auto combined = combine_filter_functions(queries);
    auto affected_keys = filter_index(index_segment_reader, std::move(combined));
    std::vector<SliceAndKey> unaffected_keys;
    std::set_difference(std::begin(index_segment_reader),
                        std::end(index_segment_reader),
                        std::begin(affected_keys),
                        std::end(affected_keys),
                        std::back_inserter(unaffected_keys));

    frame.set_bucketize_dynamic(bucketize_dynamic);
    auto slicing_arg = get_slicing_policy(options, frame);

    auto fut_slice_keys = slice_and_write(frame, slicing_arg, get_partial_key_gen(frame, IndexPartialKey{stream_id, update_info.next_version_id_}), store);
    auto new_slice_and_keys = folly::collect(fut_slice_keys).wait().value();
    std::sort(std::begin(new_slice_and_keys), std::end(new_slice_and_keys));

    IndexRange orig_filter_range;
    auto[intersect_before, intersect_after] = util::variant_match(query.row_filter,
                        [&](std::monostate) {
                            util::check(std::holds_alternative<TimeseriesIndex>(frame.index), "Update with row count index is not permitted");
                            orig_filter_range = frame.index_range;
                            auto front_range = new_slice_and_keys.begin()->key().index_range();
                            auto back_range = new_slice_and_keys.rbegin()->key().index_range();
                            back_range.adjust_open_closed_interval();
                            return intersecting_segments(affected_keys, front_range, back_range, update_info.next_version_id_, store);
                        },
                        [&](const IndexRange& idx_range) {
                            orig_filter_range = idx_range;
                            return intersecting_segments(affected_keys, idx_range, idx_range, update_info.next_version_id_, store);
                        },
                        [](const RowRange&) {
                            util::raise_rte("Unexpected row_range in update query");
                            return std::pair(std::vector<SliceAndKey>{}, std::vector<SliceAndKey>{});  // unreachable
                        }
    );

    size_t row_count = 0;
    auto flattened_slice_and_keys = flatten_and_fix_rows(std::vector<std::vector<SliceAndKey>>{
        strictly_before(orig_filter_range, unaffected_keys),
        std::move(intersect_before),
        std::move(new_slice_and_keys),
        std::move(intersect_after),
        strictly_after(orig_filter_range, unaffected_keys)},
                                                         row_count
                                                         );

    std::sort(std::begin(flattened_slice_and_keys), std::end(flattened_slice_and_keys));
    auto desc = stream::merge_descriptors(StreamDescriptor{std::move(*index_segment_reader.mutable_tsd().mutable_stream_descriptor())}, { frame.desc.fields() }, {});
    // At this stage the updated data must be sorted
    desc.set_sorted(arcticdb::entity::SortedValue::ASCENDING);
    auto time_series = make_descriptor(row_count, std::move(desc), frame.norm_meta, std::move(frame.user_meta), std::nullopt, bucketize_dynamic);
    auto index = index_type_from_descriptor(time_series.stream_descriptor());

    auto version_key_fut = util::variant_match(index, [&time_series, &flattened_slice_and_keys, &stream_id, &update_info, &store] (auto idx) {
        using IndexType = decltype(idx);
        return index::write_index<IndexType>(std::move(time_series), std::move(flattened_slice_and_keys), IndexPartialKey{stream_id, update_info.next_version_id_}, store);
    });
    auto version_key = std::move(version_key_fut).get();
    auto versioned_item = VersionedItem(to_atom(std::move(version_key)));
    ARCTICDB_DEBUG(log::version(), "updated stream_id: {} , version_id: {}", stream_id, update_info.next_version_id_);
    return versioned_item;
}

FrameAndDescriptor read_multi_key(
    const std::shared_ptr<Store>& store,
    const SegmentInMemory& index_key_seg) {
    const auto& multi_index_seg = index_key_seg;
    arcticdb::proto::descriptors::TimeSeriesDescriptor tsd;
    multi_index_seg.metadata()->UnpackTo(&tsd);
    std::vector<AtomKey> keys;
    for (size_t idx = 0; idx < index_key_seg.row_count(); idx++) {
        keys.push_back(stream::read_key_row(index_key_seg, static_cast<ssize_t>(idx)));
    }

    AtomKey dup{keys[0]};
    ReadQuery read_query;
    auto res = read_dataframe_impl(store, VersionedItem{std::move(dup)}, read_query, {});

    arcticdb::proto::descriptors::TimeSeriesDescriptor multi_key_desc{tsd};
    multi_key_desc.mutable_normalization()->CopyFrom(res.desc_.normalization());
    return {res.frame_, multi_key_desc, keys, std::shared_ptr<BufferHolder>{}};
}

/*
 * Processes the slices in the given pipeline_context.
 *
 * Slices are processed in order, with slices corresponding to the same row group collected into a single
 * Composite<SliceAndKey>. Slices contained within a single Composite<SliceAndKey> are processed within a single thread.
 *
 * The processing of a Composite<SliceAndKey> is scheduled via the Async Store. Within a single thread, the
 * segments will be retrieved from storage and decompressed before being passed to a MemSegmentProcessingTask which
 * will process all clauses up until a reducing clause.
 */
std::vector<SliceAndKey> read_and_process(
    const std::shared_ptr<Store>& store,
    const std::shared_ptr<PipelineContext>& pipeline_context,
    const ReadQuery& read_query,
    const ReadOptions& read_options,
    size_t start_from
    ) {
    std::vector<Composite<SliceAndKey>> rows;
    if(!read_query.query_->empty()) {
        if(auto execution_context = read_query.query_->begin()->execution_context(); execution_context) {
            execution_context->set_descriptor(pipeline_context->descriptor());
            execution_context->set_norm_meta_descriptor(pipeline_context->norm_meta_);
        }

        for(auto& clause : *read_query.query_) {
            if(auto execution_context = clause.execution_context(); execution_context)
                execution_context->set_dynamic_schema(opt_false(read_options.dynamic_schema_));
        }
    }

    std::sort(std::begin(pipeline_context->slice_and_keys_), std::end(pipeline_context->slice_and_keys_), [] (const SliceAndKey& left, const SliceAndKey& right) {
        return std::tie(left.slice().row_range.first, left.slice().col_range.first) < std::tie(right.slice().row_range.first, right.slice().col_range.first);
    });

    auto sk_it = std::begin(pipeline_context->slice_and_keys_);
    std::advance(sk_it, start_from);
    while(sk_it != std::end(pipeline_context->slice_and_keys_)) {
        RowRange row_range{sk_it->slice().row_range};
        auto sk = Composite{std::move(*sk_it)};
        // Iterate through all SliceAndKeys that contain data for the same RowRange - i.e., iterate along column segments
        // for same row group
        while(++sk_it != std::end(pipeline_context->slice_and_keys_) && sk_it->slice().row_range == row_range) {
            sk.push_back(std::move(*sk_it));
        }

        util::check(!sk.empty(), "Should not push empty slice/key pairs to the pipeline");
        rows.emplace_back(std::move(sk));
    }
    std::shared_ptr<std::unordered_set<std::string>> filter_columns;
    if(pipeline_context->overall_column_bitset_) {
        filter_columns = std::make_shared<std::unordered_set<std::string>>();
        auto en = pipeline_context->overall_column_bitset_->first();
        auto en_end = pipeline_context->overall_column_bitset_->end();
        while (en < en_end) {
            filter_columns->insert(pipeline_context->desc_->field(*en++).name());
        }
    }

    auto parallel_output = store->batch_read_uncompressed(std::move(rows), read_query.query_, pipeline_context->descriptor(), filter_columns, BatchReadArgs{});

    size_t clause_index = 0;
    for (const auto& clause : *read_query.query_) {
        if (clause.requires_repartition()) {
            std::vector<Composite<ProcessingSegment>> composites = clause.repartition(std::move(parallel_output)).value();

            std::vector<folly::Future<Composite<ProcessingSegment>>> batch;
            std::vector<Composite<ProcessingSegment>> res;
            res.reserve(composites.size());
            for (auto&& val : composites){
                auto clause_copy = std::make_shared<std::vector<Clause>>(read_query.query_->begin() + clause_index + 1, read_query.query_->end());
                batch.emplace_back(
                        async::submit_cpu_task(
                                async::MemSegmentPassthroughProcessingTask(store, clause_copy, std::move(val))
                        )
                );
            }

            if(!batch.empty()) {
                auto segments = folly::collect(batch).get();
                res.insert(std::end(res), std::make_move_iterator(std::begin(segments)), std::make_move_iterator(std::end(segments)));
            }

            parallel_output = std::move(res);
        }

        clause_index++;
    }

    //TODO split pipeline context into load_context and output_context
        for(auto clause  = read_query.query_->rbegin(); clause != read_query.query_->rend(); ++clause ) {
        if(clause->execution_context() && clause->execution_context()->output_descriptor_)
            pipeline_context->set_descriptor(clause->execution_context()->output_descriptor_.value());
    }

    auto merged = merge_composites(std::move(parallel_output));
    return collect_segments(std::move(merged));
}

SegmentInMemory read_direct(const std::shared_ptr<Store>& store,
                            const std::shared_ptr<PipelineContext>& pipeline_context,
                            std::shared_ptr<BufferHolder> buffers,
                            const ReadOptions& read_options) {
    ARCTICDB_DEBUG(log::version(), "Allocating frame");
    ARCTICDB_SAMPLE_DEFAULT(ReadDirect)
    auto frame = allocate_frame(pipeline_context);
    util::print_total_mem_usage(__FILE__, __LINE__, __FUNCTION__);

    ARCTICDB_DEBUG(log::version(), "Fetching frame data");
    fetch_data(frame, pipeline_context, store, opt_false(read_options.dynamic_schema_), buffers).get();
    util::print_total_mem_usage(__FILE__, __LINE__, __FUNCTION__);
    return frame;
}

void add_index_columns_to_query(const ReadQuery& read_query, const arcticdb::proto::descriptors::TimeSeriesDescriptor& desc) {
    if(!read_query.columns.empty()) {
        auto index_columns = stream::get_index_columns_from_descriptor(desc);
        if(index_columns.empty())
            return;

        std::vector<std::string> index_columns_to_add;
        for(const auto& index_column : index_columns) {
            if(std::find(std::begin(read_query.columns), std::end(read_query.columns), index_column) == std::end(read_query.columns))
                index_columns_to_add.push_back(index_column);
        }
        read_query.columns.insert(std::begin(read_query.columns), std::begin(index_columns_to_add), std::end(index_columns_to_add));
    }
}

FrameAndDescriptor read_index_impl(
    const std::shared_ptr<Store>& store,
    const VersionedItem& version) {
    auto fut_index = store->read(version.key_);
    auto [index_key, index_seg] = std::move(fut_index).get();
    arcticdb::proto::descriptors::TimeSeriesDescriptor tsd;
    tsd.set_total_rows(index_seg.row_count());
    tsd.mutable_stream_descriptor()->CopyFrom(index_seg.descriptor().proto());
    return {SegmentInMemory(std::move(index_seg)), tsd, {}, {}};
}

namespace {

void ensure_norm_meta(arcticdb::proto::descriptors::NormalizationMetadata& norm_meta, const StreamId& stream_id, bool set_tz) {
    if(norm_meta.input_type_case() == arcticdb::proto::descriptors::NormalizationMetadata::INPUT_TYPE_NOT_SET)
        norm_meta.CopyFrom(make_timeseries_norm_meta(stream_id));

    if(set_tz && norm_meta.df().common().index().tz().empty())
        norm_meta.mutable_df()->mutable_common()->mutable_index()->set_tz("UTC");
}

void check_column_and_date_range_filterable(const pipelines::index::IndexSegmentReader& index_segment_reader, const ReadQuery& read_query) {
    util::check(!index_segment_reader.is_pickled()
    || (read_query.columns.empty() && std::holds_alternative<std::monostate>(read_query.row_filter)),
    "The data for this symbol is pickled and does not support date_range, row_range, or column queries");
    util::check(index_segment_reader.has_timestamp_index() || !std::holds_alternative<IndexRange>(read_query.row_filter),
            "Cannot apply date range filter to symbol with non-timestamp index");
    sorting::check<ErrorCode::E_UNSORTED_DATA>(index_segment_reader.tsd().stream_descriptor().sorted() == arcticdb::proto::descriptors::SortedValue::UNKNOWN ||
        index_segment_reader.tsd().stream_descriptor().sorted() == arcticdb::proto::descriptors::SortedValue::ASCENDING ||
        !std::holds_alternative<IndexRange>(read_query.row_filter),
            "When filtering data using date_range, the symbol must be sorted in ascending order. ArcticDB believes it is not sorted in ascending order and cannot therefore filter the data using date_range.");
}
}

std::optional<pipelines::index::IndexSegmentReader> get_index_segment_reader(
    const std::shared_ptr<Store>& store,
    const std::shared_ptr<PipelineContext>& pipeline_context,
    const VersionedItem& version_info) {
    std::pair<entity::VariantKey, SegmentInMemory> index_key_seg;
    try {
        index_key_seg = store->read_sync(version_info.key_);
    } catch (const std::exception& ex) {
        ARCTICDB_DEBUG(log::version(), "Key not found from versioned item {}: {}", version_info.key_, ex.what());
        throw storage::NoDataFoundException(version_info.key_.id());
    }

    if (variant_key_type(index_key_seg.first) == KeyType::MULTI_KEY) {
        pipeline_context->multi_key_ = index_key_seg.second;
        return std::nullopt;
    }
    return std::make_optional<pipelines::index::IndexSegmentReader>(std::move(index_key_seg.second));
}

void read_indexed_keys_to_pipeline(
    const std::shared_ptr<Store>& store,
    const std::shared_ptr<PipelineContext>& pipeline_context,
    const VersionedItem& version_info,
    ReadQuery& read_query,
    const ReadOptions& read_options
    ) {
    auto maybe_reader = get_index_segment_reader(store, pipeline_context, version_info);
    if(!maybe_reader)
        return;

    auto index_segment_reader = std::move(maybe_reader.value());
    ARCTICDB_DEBUG(log::version(), "Read index segment with {} keys", index_segment_reader.size());
    check_column_and_date_range_filterable(index_segment_reader, read_query);

    add_index_columns_to_query(read_query, index_segment_reader.tsd());

    read_query.calculate_row_filter(static_cast<int64_t>(index_segment_reader.tsd().total_rows()));
    bool bucketize_dynamic = index_segment_reader.bucketize_dynamic();
    pipeline_context->desc_ = StreamDescriptor{std::move(*index_segment_reader.mutable_tsd().mutable_stream_descriptor())};

    bool dynamic_schema = opt_false(read_options.dynamic_schema_);
    auto queries = get_column_bitset_and_query_functions<index::IndexSegmentReader>(
        read_query,
        pipeline_context,
        dynamic_schema,
        bucketize_dynamic);

    pipeline_context->slice_and_keys_ = filter_index(index_segment_reader, combine_filter_functions(queries));
    pipeline_context->total_rows_ = pipeline_context->calc_rows();
    pipeline_context->norm_meta_ = std::make_shared<arcticdb::proto::descriptors::NormalizationMetadata>(std::move(*index_segment_reader.mutable_tsd().mutable_normalization()));
    pipeline_context->user_meta_ = std::make_unique<arcticdb::proto::descriptors::UserDefinedMetadata>(std::move(*index_segment_reader.mutable_tsd().mutable_user_meta()));
    pipeline_context->bucketize_dynamic_ = bucketize_dynamic;
}

void read_incompletes_to_pipeline(
    const std::shared_ptr<Store>& store,
    std::shared_ptr<PipelineContext>& pipeline_context,
    const ReadQuery& read_query,
    const ReadOptions& read_options,
    bool convert_int_to_float,
    bool via_iteration,
    bool sparsify) {

    auto incomplete_segments = stream::get_incomplete(
        store,
        pipeline_context->stream_id_,
        read_query.row_filter,
        pipeline_context->last_slice_row(),
        via_iteration,
        false);

    if(incomplete_segments.empty())
        return;

    // Mark the start point of the incompletes, so we know that there is no column slicing after this point
    pipeline_context->incompletes_after_ = pipeline_context->slice_and_keys_.size();

    // If there are only incompletes we need to add the index here
    if(pipeline_context->slice_and_keys_.empty()) {
        auto tsd = timeseries_descriptor_from_any(*incomplete_segments.begin()->segment(store).metadata());
        add_index_columns_to_query(read_query, tsd);
    }

    auto first_seg = incomplete_segments.begin()->segment(store);
    if (!pipeline_context->desc_)
        pipeline_context->desc_ = first_seg.descriptor();

    if (!pipeline_context->norm_meta_) {
        pipeline_context->norm_meta_ = std::make_shared<arcticdb::proto::descriptors::NormalizationMetadata>();
        auto segment_tsd = timeseries_descriptor_from_segment(first_seg);
        pipeline_context->norm_meta_->CopyFrom(segment_tsd.normalization());
        ensure_norm_meta(*pipeline_context->norm_meta_, pipeline_context->stream_id_, sparsify);
    }

    pipeline_context->desc_ = stream::merge_descriptors(pipeline_context->descriptor(), incomplete_segments, read_query.columns);
    modify_descriptor(pipeline_context, read_options);
    if (convert_int_to_float) {
        stream::convert_descriptor_types(*pipeline_context->desc_);
    }

    generate_filtered_field_descriptors(pipeline_context, read_query.columns);
    pipeline_context->slice_and_keys_.insert(std::end(pipeline_context->slice_and_keys_), std::begin(incomplete_segments),  std::end(incomplete_segments));
    pipeline_context->total_rows_ = pipeline_context->calc_rows();
}

void copy_frame_data_to_buffer(const SegmentInMemory& destination, size_t target_index, SegmentInMemory& source, size_t source_index, const RowRange& row_range) {
    auto num_rows = row_range.diff();
    if (num_rows == 0) {
        return;
    }
    auto& src_column = source.column(static_cast<position_t>(source_index));
    auto& dst_column = destination.column(static_cast<position_t>(target_index));
    auto& buffer = dst_column.data().buffer();
    auto dst_rawtype_size = sizeof_datatype(dst_column.type());
    auto offset = dst_rawtype_size * (row_range.first - destination.offset());
    auto total_size = dst_rawtype_size * num_rows;
    buffer.assert_size(offset + total_size);

    auto src_ptr = src_column.data().buffer().data();
    auto dst_ptr = buffer.data() + offset;

    auto type_promotion_error_msg = fmt::format("Can't promote type {} to type {} in field {}",
                                                src_column.type(), dst_column.type(), destination.field(target_index).name());

    if (trivially_compatible_types(src_column.type(), dst_column.type())) {
        memcpy(dst_ptr, src_ptr, total_size);
    } else if (has_valid_type_promotion(src_column.type(), dst_column.type())) {
        dst_column.type().visit_tag([&src_ptr, &dst_ptr, &src_column, &type_promotion_error_msg, num_rows] (auto dest_desc_tag) {
            using DestinationType =  typename decltype(dest_desc_tag)::DataTypeTag::raw_type;
            src_column.type().visit_tag([&src_ptr, &dst_ptr, &type_promotion_error_msg, num_rows] (auto src_desc_tag ) {
                using SourceType =  typename decltype(src_desc_tag)::DataTypeTag::raw_type;
                if constexpr(std::is_arithmetic_v<SourceType> && std::is_arithmetic_v<DestinationType>) {
                    auto typed_src_ptr = reinterpret_cast<SourceType *>(src_ptr);
                    auto typed_dst_ptr = reinterpret_cast<DestinationType *>(dst_ptr);
                    for (auto i = 0u; i < num_rows; ++i) {
                        *typed_dst_ptr++ = static_cast<DestinationType>(*typed_src_ptr++);
                    }
                } else {
                    util::raise_rte(type_promotion_error_msg.c_str());
                }
            });
        });
    } else {
        util::raise_rte(type_promotion_error_msg.c_str());
    }
}

void copy_segments_to_frame(const std::shared_ptr<Store>& store, const std::shared_ptr<PipelineContext>& pipeline_context, const SegmentInMemory& frame) {
    for (auto context_row : folly::enumerate(*pipeline_context)) {
        auto& segment = context_row->slice_and_key().segment(store);
        const auto index_field_count = get_index_field_count(frame);
        for (auto idx = 0u; idx < index_field_count && context_row->fetch_index(); ++idx) {
            copy_frame_data_to_buffer(frame, idx, segment, idx, context_row->slice_and_key().slice_.row_range);
        }

        auto field_count = context_row->slice_and_key().slice_.col_range.diff() + index_field_count;
        for (auto field_col = index_field_count; field_col < field_count; ++field_col) {
            const auto& field_name = context_row->descriptor().fields(field_col).name();
            auto frame_loc_opt = frame.column_index(field_name);
            if (!frame_loc_opt)
                continue;

            copy_frame_data_to_buffer(frame, *frame_loc_opt, segment, field_col, context_row->slice_and_key().slice_.row_range);
        }
    }
}

SegmentInMemory prepare_output_frame(std::vector<SliceAndKey>&& items, const std::shared_ptr<PipelineContext>& pipeline_context, const std::shared_ptr<Store>& store, const ReadOptions& read_options) {
    pipeline_context->clear_vectors();
    pipeline_context->slice_and_keys_ = std::move(items);
    std::sort(std::begin(pipeline_context->slice_and_keys_), std::end(pipeline_context->slice_and_keys_));
    adjust_slice_rowcounts(pipeline_context);
    const auto dynamic_schema = opt_false(read_options.dynamic_schema_);
    mark_index_slices(pipeline_context, dynamic_schema, pipeline_context->bucketize_dynamic_);
    pipeline_context->ensure_vectors();

    for(auto row : *pipeline_context) {
        row.set_compacted(false);
        row.set_descriptor(row.slice_and_key().segment(store).descriptor_ptr());
        row.set_string_pool(row.slice_and_key().segment(store).string_pool_ptr());
    }

    auto frame = allocate_frame(pipeline_context);
    copy_segments_to_frame(store, pipeline_context, frame);

    return frame;
}

FrameAndDescriptor read_dataframe_impl(
    const std::shared_ptr<Store>& store,
    const std::variant<VersionedItem, StreamId>& version_info,
    ReadQuery& read_query,
    const ReadOptions& read_options
    ) {
    using namespace arcticdb::pipelines;
    auto pipeline_context = std::make_shared<PipelineContext>();

    if(std::holds_alternative<StreamId>(version_info)) {
        pipeline_context->stream_id_ = std::get<StreamId>(version_info);
    } else {
        pipeline_context->stream_id_ = std::get<VersionedItem>(version_info).key_.id();
        read_indexed_keys_to_pipeline(store, pipeline_context, std::get<VersionedItem>(version_info), read_query, read_options);
    }

    if(pipeline_context->multi_key_)
        return read_multi_key(store, *pipeline_context->multi_key_);

    if(opt_false(read_options.incompletes_)) {
        util::check(std::holds_alternative<IndexRange>(read_query.row_filter), "Streaming read requires date range filter");
        const auto& query_range = std::get<IndexRange>(read_query.row_filter);
        const auto existing_range = pipeline_context->index_range();
        if(!existing_range.specified_ || query_range.end_ > existing_range.end_)
            read_incompletes_to_pipeline(store, pipeline_context, read_query, read_options, false, false, false);
    }

    modify_descriptor(pipeline_context, read_options);
    generate_filtered_field_descriptors(pipeline_context, read_query.columns);
    ARCTICDB_DEBUG(log::version(), "Fetching data to frame");
    SegmentInMemory frame;
    auto buffers = std::make_shared<BufferHolder>();
    if(!read_query.query_->empty()) {
        ARCTICDB_SAMPLE(RunPipelineAndOutput, 0)
        util::check_rte(!pipeline_context->is_pickled(),"Cannot filter pickled data");
        auto segs = read_and_process(store, pipeline_context, read_query, read_options, 0u);

        frame = prepare_output_frame(std::move(segs), pipeline_context, store, read_options);
    } else {
        ARCTICDB_SAMPLE(MarkAndReadDirect, 0)
        util::check_rte(!(pipeline_context->is_pickled() && std::holds_alternative<RowRange>(read_query.row_filter)), "Cannot use head/tail/row_range with pickled data, use plain read instead");
        mark_index_slices(pipeline_context, opt_false(read_options.dynamic_schema_), pipeline_context->bucketize_dynamic_);
        frame = read_direct(store, pipeline_context, buffers, read_options);
    }

    ARCTICDB_DEBUG(log::version(), "Reduce and fix columns");
    reduce_and_fix_columns(pipeline_context, frame, read_options);
    return {frame, make_descriptor(pipeline_context, {}, pipeline_context->bucketize_dynamic_), {}, buffers};
}

VersionedItem collate_and_write(
    const std::shared_ptr<Store>& store,
    const std::shared_ptr<PipelineContext>& pipeline_context,
    const std::vector<FrameSlice>& slices,
    std::vector<VariantKey> keys,
    size_t append_after,
    const std::optional<arcticdb::proto::descriptors::UserDefinedMetadata>& user_meta
    ) {
    util::check(keys.size() == slices.size(), "Mismatch between slices size and key size");
    arcticdb::proto::descriptors::TimeSeriesDescriptor tsd;
    tsd.set_total_rows(pipeline_context->total_rows_);
    //TODO eliminate copies
    tsd.mutable_stream_descriptor()->CopyFrom(pipeline_context->descriptor().proto());
    tsd.mutable_normalization()->CopyFrom(*pipeline_context->norm_meta_);
    if(user_meta)
        tsd.mutable_user_meta()->CopyFrom(*user_meta);

    auto index = stream::index_type_from_descriptor(pipeline_context->descriptor());
    return util::variant_match(index, [&store, &pipeline_context, &slices, &keys, &append_after, &tsd] (auto idx) {
        using IndexType = decltype(idx);
        index::IndexWriter<IndexType> writer(store, IndexPartialKey{pipeline_context->stream_id_, pipeline_context->version_id_}, std::move(tsd));
        auto end = std::begin(pipeline_context->slice_and_keys_);
        std::advance(end, append_after);
        ARCTICDB_DEBUG(log::version(), "Adding {} existing keys and {} new keys: ", std::distance(std::begin(pipeline_context->slice_and_keys_), end), keys.size());
        for(auto sk = std::begin(pipeline_context->slice_and_keys_); sk < end; ++sk)
            writer.add(sk->key(), sk->slice());

        for (auto key : folly::enumerate(keys)) {
            writer.add(to_atom(*key), slices[key.index]);
        }
        auto index_key =  writer.commit();
        return VersionedItem{to_atom(std::move(index_key).get())};
    });
}

VersionedItem sort_merge_impl(
    const std::shared_ptr<Store>& store,
    const StreamId& stream_id,
    const std::optional<arcticdb::proto::descriptors::UserDefinedMetadata>& norm_meta,
    const UpdateInfo& update_info,
    bool append,
    bool convert_int_to_float,
    bool via_iteration,
    bool sparsify
    ) {
    auto pipeline_context = std::make_shared<PipelineContext>();
    pipeline_context->stream_id_ = stream_id;
    pipeline_context->version_id_ = update_info.next_version_id_;
    ReadQuery read_query;

    if(append && update_info.previous_index_key_.has_value())
        read_indexed_keys_to_pipeline(store, pipeline_context, *(update_info.previous_index_key_), read_query, ReadOptions{});

    auto num_versioned_rows = pipeline_context->total_rows_;

    read_incompletes_to_pipeline(store, pipeline_context, read_query, ReadOptions{}, convert_int_to_float, via_iteration, sparsify);

    std::vector<entity::VariantKey> delete_keys;
    for(auto sk = pipeline_context->incompletes_begin(); sk != pipeline_context->end(); ++sk) {
        util::check(sk->slice_and_key().key().type() == KeyType::APPEND_DATA, "Deleting incorrect key type {}", sk->slice_and_key().key().type());
        delete_keys.emplace_back(sk->slice_and_key().key());
    }

    std::vector<FrameSlice> slices;
    std::vector<folly::Future<VariantKey>> fut_vec;
    std::optional<SegmentInMemory> last_indexed;
    auto index = stream::index_type_from_descriptor(pipeline_context->descriptor());
    util::variant_match(index,
        [&](const stream::TimeseriesIndex &timeseries_index) {
        read_query.query_->emplace_back(SortClause{timeseries_index.name()});
            ExecutionContext remove_column_partition_context{};
            remove_column_partition_context.set_descriptor(pipeline_context->descriptor());
            read_query.query_->emplace_back(RemoveColumnPartitioningClause{std::make_shared<ExecutionContext>(std::move(remove_column_partition_context))});
            const auto split_size = ConfigsMap::instance()->get_int("Split.RowCount", 10000);
            read_query.query_->emplace_back(SplitClause{static_cast<size_t>(split_size)});
            ExecutionContext merge_clause_context{};
            merge_clause_context.set_descriptor(pipeline_context->descriptor());
            read_query.query_->emplace_back(MergeClause{timeseries_index, DenseColumnPolicy{}, stream_id, std::make_shared<ExecutionContext>(std::move(merge_clause_context))});
            auto segments = read_and_process(store, pipeline_context, read_query, ReadOptions{}, pipeline_context->incompletes_after());
            pipeline_context->total_rows_ = num_versioned_rows + get_slice_rowcounts(segments);

            auto index = index_type_from_descriptor(pipeline_context->descriptor());
            stream::SegmentAggregator<TimeseriesIndex, DynamicSchema, RowCountSegmentPolicy, SparseColumnPolicy>
            aggregator{
                [&slices](FrameSlice slice) {
                    slices.emplace_back(std::move(slice));
                    },
                    DynamicSchema{pipeline_context->descriptor(), index},
                    [pipeline_context=pipeline_context, &fut_vec, &store](SegmentInMemory &&segment) {
                    auto local_index_start = TimeseriesIndex::start_value_for_segment(segment);
                    auto local_index_end = TimeseriesIndex::end_value_for_segment(segment);
                    stream::StreamSink::PartialKey
                    pk{KeyType::TABLE_DATA, pipeline_context->version_id_, pipeline_context->stream_id_, local_index_start, local_index_end};
                    fut_vec.emplace_back(store->write(pk, std::move(segment)));
                }};

            for(auto sk = segments.begin(); sk != segments.end(); ++sk) {
                aggregator.add_segment(
                    std::move(sk->segment(store)),
                    sk->slice(),
                    convert_int_to_float);

                sk->unset_segment();
            }
            aggregator.commit();
        },
        [&](const auto &) {
            util::raise_rte("Not supported index type for sort merge implementation");
        }
        );

    auto keys = folly::collect(fut_vec).get();
    auto vit = collate_and_write(
        store,
        pipeline_context,
        slices,
        keys,
        pipeline_context->incompletes_after(),
        norm_meta);

    store->remove_keys(delete_keys).get();
    return vit;
}


template <typename IndexType, typename SchemaType, typename SegmentationPolicy, typename DensityPolicy>
void do_compact(
    const std::shared_ptr<PipelineContext>& pipeline_context,
    std::vector<folly::Future<VariantKey>>& fut_vec,
    std::vector<FrameSlice>& slices,
    const std::shared_ptr<Store>& store,
    bool convert_int_to_float) {
    stream::SegmentAggregator<IndexType, SchemaType, SegmentationPolicy, DensityPolicy>
    aggregator{
        [&slices](FrameSlice slice) {
            slices.emplace_back(std::move(slice));
            },
            SchemaType{pipeline_context->descriptor(), index_type_from_descriptor(pipeline_context->descriptor())},
            [&fut_vec, &store, &pipeline_context](SegmentInMemory &&segment) {
            auto local_index_start = IndexType::start_value_for_segment(segment);
            auto local_index_end = IndexType::end_value_for_segment(segment);
            stream::StreamSink::PartialKey
            pk{KeyType::TABLE_DATA, pipeline_context->version_id_, pipeline_context->stream_id_, local_index_start, local_index_end};
            fut_vec.emplace_back(store->write(pk, std::move(segment)));
            },
            SegmentationPolicy{}
    };

    for(auto sk = pipeline_context->incompletes_begin(); sk != pipeline_context->end(); ++sk) {
        aggregator.add_segment(
            std::move(sk->slice_and_key().segment(store)),
            sk->slice_and_key().slice(),
            convert_int_to_float);

        sk->slice_and_key().unset_segment();
    }
    aggregator.commit();
}

VersionedItem compact_incomplete_impl(
    const std::shared_ptr<Store>& store,
    const StreamId& stream_id,
    const std::optional<arcticdb::proto::descriptors::UserDefinedMetadata>& user_meta,
    const UpdateInfo& update_info,
    bool append,
    bool convert_int_to_float,
    bool via_iteration,
    bool sparsify) {

    auto pipeline_context = std::make_shared<PipelineContext>();
    pipeline_context->stream_id_ = stream_id;
    pipeline_context->version_id_ = update_info.next_version_id_;
    ReadQuery read_query;
    ReadOptions read_options;
    read_options.set_dynamic_schema(true);

    std::optional<SegmentInMemory> last_indexed;
    if(append && update_info.previous_index_key_.has_value())
        read_indexed_keys_to_pipeline(store, pipeline_context, *(update_info.previous_index_key_), read_query, read_options);

    auto prev_size = pipeline_context->slice_and_keys_.size();
    read_incompletes_to_pipeline(store, pipeline_context, ReadQuery{}, ReadOptions{}, convert_int_to_float, via_iteration, sparsify);
    if (pipeline_context->slice_and_keys_.size() == prev_size) {
        util::raise_rte("No incomplete segments found for {}", stream_id);
    }
    const auto& first_seg = pipeline_context->slice_and_keys_.begin()->segment(store);

    std::vector<entity::VariantKey> delete_keys;
    for(auto sk = pipeline_context->incompletes_begin(); sk != pipeline_context->end(); ++sk) {
        util::check(sk->slice_and_key().key().type() == KeyType::APPEND_DATA, "Deleting incorrect key type {}", sk->slice_and_key().key().type());
        delete_keys.emplace_back(sk->slice_and_key().key());
    }

    std::vector<folly::Future<VariantKey>> fut_vec;
    std::vector<FrameSlice> slices;
    auto index = index_type_from_descriptor(first_seg.descriptor());

    util::variant_match(index, [
        &fut_vec, &slices, sparsify, pipeline_context=pipeline_context, &store, convert_int_to_float] (auto idx) {
        using IndexType = decltype(idx);

        if(sparsify) {
            do_compact<IndexType, DynamicSchema, RowCountSegmentPolicy, SparseColumnPolicy>(
                pipeline_context,
                fut_vec,
                slices,
                store,
                convert_int_to_float);
        } else {
            do_compact<IndexType, FixedSchema, RowCountSegmentPolicy, DenseColumnPolicy>(
                pipeline_context,
                fut_vec,
                slices,
                store,
                convert_int_to_float);
        }
    });

    auto keys = folly::collect(fut_vec).get();
    auto vit = collate_and_write(
        store,
        pipeline_context,
        slices,
        keys,
        pipeline_context->incompletes_after(),
        user_meta
        );

    store->remove_keys(delete_keys).get();
    return vit;
}

} //namespace arcticdb::version_store
