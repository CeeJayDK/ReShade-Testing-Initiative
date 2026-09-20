#pragma once
#include <vector>
namespace concurrency {
template <typename T> class concurrent_vector : public std::vector<T> {
public:
	using base = std::vector<T>;
	using typename base::iterator;
	iterator push_back(const T &v) { base::push_back(v); return base::end() - 1; }
};
}
