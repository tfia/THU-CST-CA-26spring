#include <sched.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

typedef std::chrono::nanoseconds ns_t;

const int MAX_SIZE = 16 * 1024 * 1024; // 16MB
const int MIN_SIZE = 1024; // 1KB
const int LINE_SIZE_PROBE_SIZE = 256 * 1024 * 1024; // 256MB
const int MAX_STRIDE = 256; // measured in int elements
const int DEFAULT_STRIDE = 16; // measured in int elements
const int DEFAULT_ASSOC_CACHE_SIZE_KB = 48; // 48KB
const long long MIN_ACCESS_COUNT = 1LL << 20;
const long long LINE_ACCESS_COUNT = 1LL << 24;
const long long ASSOC_ACCESS_COUNT = 1LL << 24;
const long long L1D_CACHELINE_COUNT = 48 * 1024 / 64; // 48KB L1D cache, 64B cache line

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

long long measure_line_latency_ns(int* buffer, int size, int stride) {
    int slot_cnt = size / (stride * static_cast<int>(sizeof(int)));
    int index = 0;
    int sum = 0;
    int warmup_cnt = std::max(slot_cnt / 16, 1 << 14);

    for (int i = 0; i < warmup_cnt; i++) {
        sum ^= buffer[index];
        index += stride;
        if (index >= slot_cnt * stride) {
            index = 0;
        }
    }

    auto start = std::chrono::steady_clock::now();
    for (long long i = 0; i < LINE_ACCESS_COUNT; i++) {
        sum ^= buffer[index];
        index += stride;
        if (index >= slot_cnt * stride) {
            index = 0;
        }
    }
    auto end = std::chrono::steady_clock::now();

    global_tmp = sum;
    return std::chrono::duration_cast<ns_t>(end - start).count();
}

long long measure_assoc_1_latency_ns(int* buffer, int size, int block_cnt, std::mt19937& rng) {
    int block_size = size / block_cnt;
    int block_size_int = block_size / static_cast<int>(sizeof(int));
    int odd_block_cnt = block_cnt / 2;
    std::vector<int> order(odd_block_cnt);

    for (int i = 0; i < odd_block_cnt; i++) {
        order[i] = i * 2 + 1;
    }
    std::shuffle(order.begin(), order.end(), rng);

    for (int i = 0; i < odd_block_cnt; i++) {
        int cur = order[i] * block_size_int;
        int nxt = order[(i + 1) % odd_block_cnt] * block_size_int;
        buffer[cur] = nxt;
    }

    int index = order[0] * block_size_int;
    int warmup_cnt = std::max(odd_block_cnt * 1024, 1 << 14);
    for (int i = 0; i < warmup_cnt; i++) {
        index = buffer[index];
    }

    auto start = std::chrono::steady_clock::now();
    for (long long i = 0; i < ASSOC_ACCESS_COUNT; i++) {
        index = buffer[index];
    }
    auto end = std::chrono::steady_clock::now();

    global_tmp = index;
    return std::chrono::duration_cast<ns_t>(end - start).count();
}

long long measure_assoc_2_latency_ns(int* buffer, int size, int n, std::mt19937& rng) {
    int cache_line_size = size / 2 / static_cast<int>(L1D_CACHELINE_COUNT);
    int elem_cnt = size / static_cast<int>(sizeof(int));
    int stride_cacheline = static_cast<int>(L1D_CACHELINE_COUNT / n);
    int stride = stride_cacheline * cache_line_size / static_cast<int>(sizeof(int));
    int slot_cnt = elem_cnt / std::gcd(elem_cnt, stride);
    std::vector<int> order(slot_cnt);

    assert(n > 0);
    assert(stride > 0);

    int index = 0;
    for (int i = 0; i < slot_cnt; i++) {
        order[i] = index;
        index += stride;
        if (index >= elem_cnt) {
            index %= elem_cnt;
        }
    }
    std::shuffle(order.begin(), order.end(), rng);

    for (int i = 0; i < slot_cnt; i++) {
        buffer[order[i]] = order[(i + 1) % slot_cnt];
    }

    index = order[0];
    int warmup_cnt = std::max(slot_cnt * 16, 1 << 14);
    for (int i = 0; i < warmup_cnt; i++) {
        index = buffer[index];
    }

    auto start = std::chrono::steady_clock::now();
    for (long long i = 0; i < ASSOC_ACCESS_COUNT; i++) {
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

void probe_cache_line_size() {
    void* raw = nullptr;
    assert(posix_memalign(&raw, 64, LINE_SIZE_PROBE_SIZE) == 0);
    int* buffer = static_cast<int*>(raw);
    int int_cnt = LINE_SIZE_PROBE_SIZE / static_cast<int>(sizeof(int));
    std::iota(buffer, buffer + int_cnt, 0);

    for (int stride = 1; stride <= MAX_STRIDE; stride *= 2) {
        int slot_cnt = LINE_SIZE_PROBE_SIZE / (stride * static_cast<int>(sizeof(int)));
        if (slot_cnt < 2) {
            break;
        }

        long long elapsed_ns = measure_line_latency_ns(buffer, LINE_SIZE_PROBE_SIZE, stride);
        double ns_per_access = static_cast<double>(elapsed_ns) / LINE_ACCESS_COUNT;
        std::cout << "Size: " << LINE_SIZE_PROBE_SIZE / 1024 << " KB, Latency: "
                  << ns_per_access << " ns/access"
                  << ", Stride: " << stride * sizeof(int) << " B" << std::endl;
    }

    free(raw);
}

void probe_cache_associativity_1(int cache_size_kb) {
    int cache_size = cache_size_kb * 1024;
    int size = cache_size * 2;
    void* raw = nullptr;
    assert(cache_size > 0);
    assert(posix_memalign(&raw, 64, size) == 0);
    int* buffer = static_cast<int*>(raw);
    std::mt19937 rng(2023012163);

    for (int n = 2; ; n++) {
        int block_cnt = 1 << n;
        int block_size = size / block_cnt;
        if (block_size < 64) {
            break;
        }

        long long elapsed_ns = measure_assoc_1_latency_ns(buffer, size, block_cnt, rng);
        double ns_per_access = static_cast<double>(elapsed_ns) / ASSOC_ACCESS_COUNT;
        std::cout << "Size: " << size / 1024 << " KB, Latency: "
                  << ns_per_access << " ns/access"
                  << ", Blocks: " << block_cnt
                  << ", Ways: " << (1 << (n - 2)) << std::endl;
    }

    free(raw);
}

void probe_cache_associativity_2(int cache_size_kb) {
    int cache_size = cache_size_kb * 1024;
    int size = cache_size * 2;
    void* raw = nullptr;
    assert(cache_size > 0);
    assert(posix_memalign(&raw, 64, size) == 0);
    int* buffer = static_cast<int*>(raw);
    int int_cnt = size / static_cast<int>(sizeof(int));
    std::iota(buffer, buffer + int_cnt, 0);
    std::mt19937 rng(2023012163);

    for (int n = 1; n <= 32 && n <= L1D_CACHELINE_COUNT; n++) {
        long long elapsed_ns = measure_assoc_2_latency_ns(buffer, size, n, rng);
        double ns_per_access = static_cast<double>(elapsed_ns) / ASSOC_ACCESS_COUNT;
        int stride = static_cast<int>(L1D_CACHELINE_COUNT / n) * (size / 2 / static_cast<int>(L1D_CACHELINE_COUNT));
        std::cout << "Size: " << size / 1024 << " KB, Latency: "
                  << ns_per_access << " ns/access"
                  << ", Stride: " << stride << " B"
                  << ", Ways: " << n << std::endl;
    }

    free(raw);
}

int main(int argc, char* argv[]) {
    // Bind to CPU 0
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(0, &mask);
    assert(sched_setaffinity(0, sizeof(mask), &mask) >= 0);

    int stride = DEFAULT_STRIDE;
    if (argc >= 2) {
        if (std::string(argv[1]) == "line") {
            probe_cache_line_size();
            return 0;
        }
        if (std::string(argv[1]) == "assoc_1") {
            int cache_size_kb = DEFAULT_ASSOC_CACHE_SIZE_KB;
            if (argc >= 3) {
                cache_size_kb = std::atoi(argv[2]);
                assert(cache_size_kb > 0);
            }

            probe_cache_associativity_1(cache_size_kb);
            return 0;
        }
        if (std::string(argv[1]) == "assoc_2" || std::string(argv[1]) == "assoc") {
            int cache_size_kb = DEFAULT_ASSOC_CACHE_SIZE_KB;
            if (argc >= 3) {
                cache_size_kb = std::atoi(argv[2]);
                assert(cache_size_kb > 0);
            }

            probe_cache_associativity_2(cache_size_kb);
            return 0;
        }
        
        stride = std::atoi(argv[1]);
        assert(stride > 0);

        probe_cache_size(stride);
    }
    
    return 0;
}
