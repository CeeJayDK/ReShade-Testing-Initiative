#pragma once
#include <unordered_map>
namespace concurrency { template <typename K, typename V> using concurrent_unordered_map = std::unordered_map<K, V>; }
