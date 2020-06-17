#pragma once
#include <ostream>
#include <vector>
#include <algorithm>

template<typename T>
class Statistic
{
public:
  void reserve(uint32_t size) { vec.reserve(size); }

  size_t size() { return vec.size(); }

  void add(T v) { vec.push_back(v); }

  void print(std::ostream& os) {
    size_t n = vec.size();
    os << "cnt: " << n << std::endl;
    if (n == 0) return;
    T first = vec[0];
    std::sort(vec.begin(), vec.end());
    T sum = std::accumulate(vec.begin(), vec.end(), 0);
    T mean = sum / n;
    T var = 0;
    for (T v : vec) {
      var += (v - mean) * (v - mean);
    }
    var /= n;
    os << "min: " << vec.front() << std::endl;
    os << "max: " << vec.back() << std::endl;
    os << "first:  " << first << std::endl;
    os << "mean: " << mean << std::endl;
    os << "sd: " << sqrt(var) << std::endl;
    os << "1%: " << vec[n * 1 / 100] << std::endl;
    os << "10%: " << vec[n * 10 / 100] << std::endl;
    os << "50%: " << vec[n * 50 / 100] << std::endl;
    os << "90%: " << vec[n * 90 / 100] << std::endl;
    os << "99%: " << vec[n * 99 / 100] << std::endl;
  }

private:
  std::vector<T> vec;
};

