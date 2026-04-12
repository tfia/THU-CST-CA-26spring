#include <sched.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

typedef std::chrono::nanoseconds ns_t;

const int MAX_SIZE = 16 * 1024 * 1024; // 16MB
const int MIN_SIZE = 1024; // 1KB
const int DEFAULT_STRIDE = 16; // measured in int elements
const long long MIN_ACCESS_COUNT = 1LL << 20;

// To prevent compiler optimization
volatile int global_tmp = 0;

long long measure_latency_ns(int* buffer, int size, int stride, std::mt19937& rng) {
    int slot_cnt = size / (stride * static_cast<int>(sizeof(int)));

    // Generate a random access pattern
    std::vector<int> order(slot_cnt);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), rng);

    // "Linked list" in buffer
    for (int i = 0; i < slot_cnt; i++) {
        int cur = order[i] * stride;
        int nxt = order[(i + 1) % slot_cnt] * stride;
        buffer[cur] = nxt;
    }

    int index = order[0] * stride;
    int warmup_cnt = std::max(slot_cnt * 16, 1 << 14);
    for (int i = 0; i < warmup_cnt; i++) {
        index = buffer[index];
    }

    long long access_cnt = std::max(MIN_ACCESS_COUNT, 1LL * slot_cnt * 256);
    auto start = std::chrono::steady_clock::now();
    for (long long i = 0; i < access_cnt; i++) {
        index = buffer[index];
    }
    auto end = std::chrono::steady_clock::now();

    global_tmp = index;
    return std::chrono::duration_cast<ns_t>(end - start).count();
}

void probe_cache_size(int stride) {
    void* raw = nullptr;
    assert(stride > 0);
    assert(posix_memalign(&raw, stride, MAX_SIZE) == 0);
    int* buffer = static_cast<int*>(raw);
    std::mt19937 rng(2023012163);

    int size = MIN_SIZE;

    // for (int size = MIN_SIZE; size <= MAX_SIZE; size *= 2) {
    while(size <= MAX_SIZE) {
        int slot_cnt = size / (stride * static_cast<int>(sizeof(int)));
        if (slot_cnt < 2) {
            std::cout << "Size: " << size / 1024 << " KB, skipped" << std::endl;
            continue;
        }

        long long elapsed_ns = measure_latency_ns(buffer, size, stride, rng);
        long long access_cnt = std::max(MIN_ACCESS_COUNT, 1LL * slot_cnt * 256);
        double ns_per_access = static_cast<double>(elapsed_ns) / access_cnt;
        std::cout << "Size: " << size / 1024 << " KB, Latency: "
                  << ns_per_access << " ns/access"
                  << ", Stride: " << stride * sizeof(int) << " B" << std::endl;

        if (size < 128 * 1024) {
            size += (((size / 1024) & 1) ? 15 : 16) * 1024;
        } else {
            size *= 2;
        }
    }

    free(raw);
}

int main(int argc, char* argv[]) {
    int stride = DEFAULT_STRIDE;
    if (argc >= 2) {
        stride = std::atoi(argv[1]);
        assert(stride > 0);
    }

    // Bind to CPU 0
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(0, &mask);
    assert(sched_setaffinity(0, sizeof(mask), &mask) >= 0);

    probe_cache_size(stride);
    
    return 0;
}
