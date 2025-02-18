/* Copyright 2023 Man Group Operations Limited
 *
 * Use of this software is governed by the Business Source License 1.1 included in the file licenses/BSL.txt.
 *
 * As of the Change Date specified in that file, in accordance with the Business Source License, use of this software will be governed by the Apache License, version 2.0.
 */

#pragma once

#include <memory>
#include <arcticdb/storage/store.hpp>
#include <arcticdb/util/variant.hpp>
#include <arcticdb/stream/stream_reader.hpp>
#include <arcticdb/stream/stream_utils.hpp>

namespace arcticdb {

template<class Predicate>
inline void delete_keys_of_type_if(const std::shared_ptr<Store>& store, Predicate&& predicate, KeyType key_type, const std::string& prefix = std::string(), bool continue_on_error = false) {
    static const size_t delete_object_limit = ConfigsMap::instance()->get_int("Storage.DeleteBatchSize", 1000);
    std::vector<VariantKey> keys{};
    try {
        store->iterate_type(key_type, [predicate=std::forward<Predicate>(predicate), store=store, &keys](VariantKey &&key) {
            if(predicate(key))
                keys.emplace_back(std::move(key));

            if(keys.size() == delete_object_limit) {
                store->remove_keys(keys).get();
                keys.clear();
            }
        }, prefix);

        if(!keys.empty())
            store->remove_keys(keys).get();
    }
    catch(const std::exception& ex) {
        if(continue_on_error)
            log::storage().warn("Caught exception {} trying to delete key, continuing", ex.what());
        else
            throw;
    }
}

inline void delete_keys_of_type_for_stream(const std::shared_ptr<Store>& store, const StreamId& stream_id, KeyType key_type, bool continue_on_error = false) {
    auto prefix = std::holds_alternative<StringId>(stream_id) ? std::get<StringId>(stream_id) : std::string();
    auto match_stream_id =  [&stream_id](const VariantKey & k){ return variant_key_id(k) == stream_id; };
    delete_keys_of_type_if(store, std::move(match_stream_id), key_type, prefix, continue_on_error);
}

inline void delete_all_keys_of_type(KeyType key_type, const std::shared_ptr<Store>& store, bool continue_on_error) {
    auto match_stream_id = [](const VariantKey &){ return true; };
    delete_keys_of_type_if(store, std::move(match_stream_id), key_type, std::string{}, continue_on_error);
}

inline void delete_all_for_stream(const std::shared_ptr<Store>& store, const StreamId& stream_id, bool continue_on_error = false) {
    foreach_key_type([&store, &stream_id, continue_on_error] (KeyType key_type) { delete_keys_of_type_for_stream(store, stream_id, key_type, continue_on_error); });
}

inline void delete_all(const std::shared_ptr<Store>& store, bool continue_on_error) {
    foreach_key_type([&store, continue_on_error] (KeyType key_type) {
        ARCTICDB_DEBUG(log::version(), "Deleting keys of type {}", key_type);
        delete_all_keys_of_type(key_type, store, continue_on_error);
    });
}

template<typename KeyContainer, typename = std::enable_if<std::is_base_of_v<AtomKey, typename KeyContainer::value_type>>>
inline std::vector<AtomKey> get_data_keys(
    const std::shared_ptr<stream::StreamSource>& store,
    const KeyContainer& keys,
    storage::ReadKeyOpts opts) {
    using KeySupplier = folly::Function<KeyContainer()>;
    using StreamReader = arcticdb::stream::StreamReader<AtomKey, KeySupplier, SegmentInMemory::Row>;
    auto gen = [&keys]() { return keys; };
    StreamReader stream_reader(std::move(gen), store, opts);
    return stream_reader.generate_data_keys() | folly::gen::as<std::vector>();
}

inline std::vector<AtomKey> get_data_keys(
    const std::shared_ptr<stream::StreamSource>& store,
    const AtomKey& key,
    storage::ReadKeyOpts opts) {
    const std::vector<AtomKey> keys{key};
    return get_data_keys(store, keys, opts);
}

template<typename KeyContainer, typename = std::enable_if<std::is_base_of_v<AtomKey, typename KeyContainer::value_type>>>
inline std::unordered_set<AtomKey> get_data_keys_set(
    const std::shared_ptr<stream::StreamSource>& store,
    const KeyContainer& keys,
    storage::ReadKeyOpts opts) {
    auto vec = get_data_keys(store, keys, opts);
    return {vec.begin(), vec.end()};
}

std::unordered_set<AtomKey> recurse_segment(const std::shared_ptr<stream::StreamSource>& store,
                                            SegmentInMemory segment,
                                            std::optional<VersionId> version_id);

/* Given a [multi-]index key, returns a set containing the top level [multi-]index key itself, and all of the
 * multi-index, index, and data keys referenced by this [multi-]index key.
 * If the version_id argument is provided, the returned set will only contain keys matching that version_id. */
inline std::unordered_set<AtomKey> recurse_index_key(const std::shared_ptr<stream::StreamSource>& store,
                                                     const IndexTypeKey& index_key,
                                                     std::optional<VersionId> version_id=std::nullopt) {
    auto segment = store->read_sync(index_key).second;
    auto res = recurse_segment(store, segment, version_id);
    res.emplace(index_key);
    return res;
}

inline std::unordered_set<AtomKey> recurse_segment(const std::shared_ptr<stream::StreamSource>& store,
                                                   SegmentInMemory segment,
                                                   std::optional<VersionId> version_id) {
    std::unordered_set<AtomKey> res;
    for (size_t idx = 0; idx < segment.row_count(); idx++) {
        auto key = stream::read_key_row(segment, idx);
        if ((version_id && key.version_id() == *version_id) || !version_id) {
            switch (key.type()) {
                case KeyType::TABLE_DATA:
                    res.emplace(std::move(key));
                    break;
                case KeyType::TABLE_INDEX:
                case KeyType::MULTI_KEY:
                    res.merge(recurse_index_key(store, key, version_id));
                    break;
                default:
                    break;
            }
        }
    }
    return res;
}

inline VersionId get_next_version_from_key(const AtomKey& prev) {
    auto version = prev.version_id();
    return ++version;
}

inline VersionId get_next_version_from_key(std::optional<AtomKey> maybe_prev) {
    VersionId version = 0;
    if (maybe_prev) {
       version = get_next_version_from_key(maybe_prev.value());
    }

    return version;
}

inline AtomKey in_memory_key(KeyType key_type, const StreamId& stream_id, VersionId version_id) {
    return atom_key_builder().version_id(version_id).build(stream_id, key_type);
}

template<class Predicate, class Function>
inline void iterate_keys_of_type_if(const std::shared_ptr<Store>& store, Predicate&& predicate, KeyType key_type, const std::string& prefix, Function&& function) {
    std::vector<folly::Future<entity::VariantKey>> fut_vec;
    store->iterate_type(key_type, [predicate=std::forward<Predicate>(predicate), function=std::forward<Function>(function)](const VariantKey &&key) {
        if(predicate(key)) {
           function(key);
        }
    }, prefix);
}

template <class Function>
inline void iterate_keys_of_type_for_stream(
    std::shared_ptr<Store> store, KeyType key_type, const StreamId& stream_id, Function&& function
    ) {
    auto prefix = std::holds_alternative<StringId>(stream_id) ? std::get<StringId>(stream_id) : std::string();
    auto match_stream_id =  [&stream_id](const VariantKey & k){ return variant_key_id(k) == stream_id; };
    iterate_keys_of_type_if(store, match_stream_id, key_type, prefix, std::forward<Function>(function));
}

} //namespace arcticdb