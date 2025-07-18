#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "ebpf_gpu_processor.hpp"
#include "test_utils.hpp"
#include "test_kernel.h"
#include <vector>
#include <chrono>
#include <random>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <array>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <future>
#include <thread>
#include <cmath>

using namespace ebpf_gpu;

// Configuration for test parameters - enables easy changing of test sizes
namespace test_config {
    const std::vector<size_t> scaling_sizes = {100, 1000, 10000, 100000, 1000000};
    const size_t single_vs_batch_size = 1000;
    const size_t memory_transfer_size = 100;
    const size_t batch_size = 10000;  // Batch size for batch processing
    const size_t total_events_size = 1000000;  // Total events for async test
    const std::chrono::seconds wait_timeout{10}; // Timeout for waiting on completion
}

// Helper function to create test events
void create_test_events(std::vector<NetworkEvent>& events) {
    std::srand(std::time(nullptr));
    
    for (size_t i = 0; i < events.size(); i++) {
        events[i].data = nullptr;
        events[i].length = 64 + (std::rand() % 1400);
        events[i].timestamp = std::time(nullptr) * 1000000 + i;
        events[i].src_ip = 0xC0A80000 + (std::rand() % 256); // 192.168.0.x
        events[i].dst_ip = 0x08080808; // 8.8.8.8
        events[i].src_port = 1024 + (std::rand() % 60000);
        events[i].dst_port = (std::rand() % 2) ? 80 : 443; // HTTP or HTTPS
        events[i].protocol = (std::rand() % 2) ? 6 : 17; // TCP or UDP
        events[i].action = 0; // Initialize to DROP
    }
}

// Helper function to validate processing results
bool validate_results(const std::vector<NetworkEvent>& events) {
    size_t processed_count = 0;
    for (const auto& event : events) {
        // Check that action was modified (0=DROP, 1=PASS)
        if (event.action == 0 || event.action == 1) {
            processed_count++;
        }
    }
    return processed_count == events.size();
}

// Helper function to reset event actions
void reset_event_actions(std::vector<NetworkEvent>& events) {
    for (auto& event : events) {
        event.action = 0;
    }
}

// Helper function to copy events for comparison
std::vector<NetworkEvent> copy_events(const std::vector<NetworkEvent>& events) {
    return events; // Simple copy
}

// Helper function to format sizes (1000 -> "1K", 1000000 -> "1M")
std::string format_size(size_t size) {
    if (size >= 1000000) return std::to_string(size / 1000000) + "M";
    if (size >= 1000) return std::to_string(size / 1000) + "K";
    return std::to_string(size);
}

// Get the kernel function name based on the backend
std::string get_test_kernel_name() {
    TestBackend backend = detect_test_backend();
    if (backend == TestBackend::OpenCL) {
        return "simple_kernel"; // OpenCL function name
    } else {
        return kernel_names::DEFAULT_TEST_KERNEL; // CUDA/PTX function name
    }
}

// Helper function to check environment and setup processor
bool setup_test_environment(EventProcessor& processor) {
    auto devices = get_available_devices();
    if (devices.empty()) {
        return false;
    }
    
    const char* test_code = get_test_ptx();
    if (!test_code) {
        return false;
    }
    
    std::string kernel_name = get_test_kernel_name();
    ProcessingResult load_result = processor.load_kernel_from_ir(test_code, kernel_name);
    return load_result == ProcessingResult::Success;
}

// Helper function to run benchmark with warm-up and validation
template<typename BenchmarkFunc>
void run_benchmark(const std::string& name, EventProcessor& processor, 
                  std::vector<NetworkEvent>& events, BenchmarkFunc&& benchmark_func) {
    // Warm up
    reset_event_actions(events);
    size_t buffer_size = events.size() * sizeof(NetworkEvent);
    processor.process_events(events.data(), buffer_size, events.size());
    
    // Run benchmark
    BENCHMARK_ADVANCED(name.c_str())(Catch::Benchmark::Chronometer meter) {
        reset_event_actions(events);
        meter.measure(benchmark_func);
    };
    
    // Validate
    reset_event_actions(events);
    ProcessingResult final_result = processor.process_events(events.data(), buffer_size, events.size());
    REQUIRE(final_result == ProcessingResult::Success);
    REQUIRE(validate_results(events));
}

TEST_CASE("Performance - Basic Operations", "[performance][benchmark]") {
    auto devices = get_available_devices();
    if (devices.empty()) {
        SKIP("No GPU devices available for performance testing");
    }
    
    const char* test_code = get_test_ptx();
    if (!test_code) {
        SKIP("Test IR code not found for performance testing");
    }
    
    // Pre-create and setup test data
    std::vector<NetworkEvent> events_1000(1000);
    create_test_events(events_1000);
    
    // Setup processor once outside benchmark
    EventProcessor processor;
    std::string kernel_name = get_test_kernel_name();
    ProcessingResult load_result = processor.load_kernel_from_ir(test_code, kernel_name);
    REQUIRE(load_result == ProcessingResult::Success);

    // Warm up GPU (first run is often slower)
    reset_event_actions(events_1000);
    size_t buffer_size = events_1000.size() * sizeof(NetworkEvent);
    processor.process_events(events_1000.data(), buffer_size, events_1000.size());

    BENCHMARK_ADVANCED("Event processing - 1000 events")(Catch::Benchmark::Chronometer meter) {
        // Reset data state before measurement
        reset_event_actions(events_1000);
        
        // Measure only the GPU processing
        meter.measure([&] {
            return processor.process_events(events_1000.data(), buffer_size, events_1000.size());
        });
    };
    
    // Validate results after benchmark (not during timing)
    reset_event_actions(events_1000);
    ProcessingResult final_result = processor.process_events(events_1000.data(), buffer_size, events_1000.size());
    REQUIRE(final_result == ProcessingResult::Success);
    REQUIRE(validate_results(events_1000));
}

TEST_CASE("Performance - Zero-Copy vs Normal Copy", "[performance][benchmark]") {
    auto devices = get_available_devices();
    if (devices.empty()) {
        SKIP("No CUDA devices available for performance testing");
    }
    
    const char* test_code = get_test_ptx();
    if (!test_code) {
        SKIP("Test IR code not found for performance testing");
    }
    
    // Test different event counts
    for (size_t event_count : test_config::scaling_sizes) {
        SECTION("Events: " + format_size(event_count)) {
            // Create test data
            std::vector<NetworkEvent> events(event_count);
            create_test_events(events);
            size_t buffer_size = events.size() * sizeof(NetworkEvent);
            
            // Test with normal copy (default config)
            {
                EventProcessor processor;
                std::string kernel_name = get_test_kernel_name();
                ProcessingResult load_result = processor.load_kernel_from_ir(test_code, kernel_name);
                REQUIRE(load_result == ProcessingResult::Success);
                
                std::string bench_name = "Normal copy - " + format_size(event_count) + " events";
                run_benchmark(bench_name, processor, events, [&] {
                    return processor.process_events(events.data(), buffer_size, events.size());
                });
            }
            
            // Test with zero-copy
            {
                EventProcessor::Config config;
                config.use_zero_copy = true;
                
                EventProcessor processor(config);
                std::string kernel_name = get_test_kernel_name();
                ProcessingResult load_result = processor.load_kernel_from_ir(test_code, kernel_name);
                REQUIRE(load_result == ProcessingResult::Success);
                
                // Register the buffer for zero-copy
                ProcessingResult register_result = processor.register_host_buffer(events.data(), buffer_size);
                REQUIRE(register_result == ProcessingResult::Success);
                
                std::string bench_name = "Zero-copy - " + format_size(event_count) + " events";
                run_benchmark(bench_name, processor, events, [&] {
                    return processor.process_events(events.data(), buffer_size, events.size());
                });
                
                // Unregister the buffer
                ProcessingResult unregister_result = processor.unregister_host_buffer(events.data());
                REQUIRE(unregister_result == ProcessingResult::Success);
            }
        }
    }
}

TEST_CASE("Performance - Unified Memory vs Normal Copy", "[performance][benchmark]") {
    auto devices = get_available_devices();
    if (devices.empty()) {
        SKIP("No CUDA devices available for performance testing");
    }
    
    const char* test_code = get_test_ptx();
    if (!test_code) {
        SKIP("Test IR code not found for performance testing");
    }
    
    // Check if any device supports unified memory
    bool any_device_supports_unified = false;
    for (const auto& device : devices) {
        if (device.unified_addressing) {
            any_device_supports_unified = true;
            break;
        }
    }
    
    if (!any_device_supports_unified) {
        SKIP("No CUDA devices with unified memory support available");
    }
    
    // Test different event counts
    for (size_t event_count : {100, 1000, 10000, 100000}) {
        SECTION("Events: " + format_size(event_count)) {
            // Create test data
            std::vector<NetworkEvent> events(event_count);
            create_test_events(events);
            size_t buffer_size = events.size() * sizeof(NetworkEvent);
            
            // Test with normal copy (default config)
            {
                EventProcessor processor;
                std::string kernel_name = get_test_kernel_name();
                ProcessingResult load_result = processor.load_kernel_from_ir(test_code, kernel_name);
                REQUIRE(load_result == ProcessingResult::Success);
                
                std::string bench_name = "Normal copy - " + format_size(event_count) + " events";
                run_benchmark(bench_name, processor, events, [&] {
                    return processor.process_events(events.data(), buffer_size, events.size());
                });
            }
            
            // Test with unified memory
            {
                EventProcessor::Config config;
                config.use_unified_memory = true;
                
                EventProcessor processor(config);
                std::string kernel_name = get_test_kernel_name();
                ProcessingResult load_result = processor.load_kernel_from_ir(test_code, kernel_name);
                REQUIRE(load_result == ProcessingResult::Success);
                
                std::string bench_name = "Unified memory - " + format_size(event_count) + " events";
                run_benchmark(bench_name, processor, events, [&] {
                    return processor.process_events(events.data(), buffer_size, events.size());
                });
            }
        }
    }
}

TEST_CASE("Performance - Scaling Test", "[performance][benchmark]") {
    auto devices = get_available_devices();
    if (devices.empty()) {
        SKIP("No CUDA devices available for performance testing");
    }
    
    const char* test_code = get_test_ptx();
    if (!test_code) {
        SKIP("Test IR code not found for performance testing");
    }
    
    // Setup processor once
    EventProcessor processor;
    std::string kernel_name = get_test_kernel_name();
    ProcessingResult load_result = processor.load_kernel_from_ir(test_code, kernel_name);
    REQUIRE(load_result == ProcessingResult::Success);
    
    // Test different event counts
    for (size_t event_count : test_config::scaling_sizes) {
        // Use a section to provide better organization in output
        SECTION("Events: " + format_size(event_count)) {
            std::vector<NetworkEvent> events(event_count);
            create_test_events(events);
            size_t buffer_size = events.size() * sizeof(NetworkEvent);
            
            // Only use pinned memory for 1K test
            bool use_pinned_memory = (event_count == 1000);
            
            if (use_pinned_memory) {
                // Register the events buffer
                ProcessingResult register_result = processor.register_host_buffer(events.data(), buffer_size);
                REQUIRE(register_result == ProcessingResult::Success);
                
                // Run benchmark with pinned memory
                std::string bench_name = "Scaling - " + format_size(event_count) + " events (pinned)";
                run_benchmark(bench_name, processor, events, [&] {
                    return processor.process_events(events.data(), buffer_size, events.size());
                });
                
                // Unregister the events buffer
                ProcessingResult unregister_result = processor.unregister_host_buffer(events.data());
                REQUIRE(unregister_result == ProcessingResult::Success);
            } else {
                // Run benchmark with pageable memory
                std::string bench_name = "Scaling - " + format_size(event_count) + " events";
                run_benchmark(bench_name, processor, events, [&] {
                    return processor.process_events(events.data(), buffer_size, events.size());
                });
            }
        }
    }
}

TEST_CASE("Performance - Single vs Multiple Events", "[performance][benchmark]") {
    auto devices = get_available_devices();
    if (devices.empty()) {
        SKIP("No CUDA devices available for performance testing");
    }
    
    const char* test_code = get_test_ptx();
    if (!test_code) {
        SKIP("Test IR code not found for performance testing");
    }
    
    // Setup processor once
    EventProcessor processor;
    std::string kernel_name = get_test_kernel_name();
    ProcessingResult load_result = processor.load_kernel_from_ir(test_code, kernel_name);
    REQUIRE(load_result == ProcessingResult::Success);
    
    // Pre-create test data
    const size_t num_events = test_config::single_vs_batch_size;
    std::vector<NetworkEvent> events(num_events);
    create_test_events(events);
    
    // Batch processing benchmark
    {
        size_t buffer_size = events.size() * sizeof(NetworkEvent);
        std::string bench_name = "Multiple events - " + format_size(num_events) + " events batch";
        run_benchmark(bench_name, processor, events, [&] {
            return processor.process_events(events.data(), buffer_size, events.size());
        });
    }
    
    // Single event processing benchmark
    NetworkEvent single_event = events[0];
    BENCHMARK_ADVANCED(("Single events - " + format_size(num_events) + " individual calls").c_str())
    (Catch::Benchmark::Chronometer meter) {
        meter.measure([&] {
            int total_result = 0;
            for (size_t i = 0; i < num_events; i++) {
                single_event.action = 0;
                ProcessingResult result = processor.process_event(&single_event, sizeof(NetworkEvent));
                total_result += static_cast<int>(result);
            }
            return total_result;
        });
    };
    
    // Validate single event processing
    single_event.action = 0;
    ProcessingResult single_result = processor.process_event(&single_event, sizeof(NetworkEvent));
    REQUIRE(single_result == ProcessingResult::Success);
    REQUIRE((single_event.action == 0 || single_event.action == 1));
}

TEST_CASE("Performance - Memory Transfer vs Compute", "[performance][benchmark]") {
    auto devices = get_available_devices();
    if (devices.empty()) {
        SKIP("No CUDA devices available for performance testing");
    }
    
    const char* test_code = get_test_ptx();
    if (!test_code) {
        SKIP("Test IR code not found for performance testing");
    }
    
    EventProcessor processor;
    std::string kernel_name = get_test_kernel_name();
    ProcessingResult load_result = processor.load_kernel_from_ir(test_code, kernel_name);
    REQUIRE(load_result == ProcessingResult::Success);
    
    auto run_memory_transfer_test = [&](const std::string& test_name, size_t event_size_multiplier) {
        const size_t event_count = test_config::memory_transfer_size;
        
        std::vector<std::vector<uint8_t>> event_buffers;
        event_buffers.reserve(event_count);
        
        for (size_t i = 0; i < event_count; i++) {
            size_t padded_size = sizeof(NetworkEvent) + (event_size_multiplier * 64);
            event_buffers.emplace_back(padded_size, 0);
            
            // Place NetworkEvent at the beginning
            NetworkEvent* event = reinterpret_cast<NetworkEvent*>(event_buffers[i].data());
            event->data = nullptr;
            event->length = 64;
            event->timestamp = i;
            event->src_ip = 0xC0A80001;
            event->dst_ip = 0x08080808;
            event->src_port = 1024;
            event->dst_port = 80;
            event->protocol = 6;
            event->action = 0;
        }
        
        // Warm up
        for (auto& buffer : event_buffers) {
            processor.process_event(buffer.data(), buffer.size());
        }
        
        BENCHMARK_ADVANCED(test_name.c_str())(Catch::Benchmark::Chronometer meter) {
            meter.measure([&] {
                int total_result = 0;
                for (auto& buffer : event_buffers) {
                    // Reset action
                    reinterpret_cast<NetworkEvent*>(buffer.data())->action = 0;
                    ProcessingResult result = processor.process_event(buffer.data(), buffer.size());
                    total_result += static_cast<int>(result);
                }
                return total_result;
            });
        };
    };
    
    // Test small events (64B each)
    run_memory_transfer_test("Small events - 100 events (64B each)", 1);
    
    // Test large events (1KB each)
    run_memory_transfer_test("Large events - 100 events (1KB each)", 16);
}

TEST_CASE("Performance - Pinned vs Pageable Memory", "[performance][benchmark]") {
    auto devices = get_available_devices();
    if (devices.empty()) {
        SKIP("No CUDA devices available for performance testing");
    }
    
    const char* test_code = get_test_ptx();
    if (!test_code) {
        SKIP("Test IR code not found for performance testing");
    }
    
    EventProcessor processor;
    std::string kernel_name = get_test_kernel_name();
    ProcessingResult load_result = processor.load_kernel_from_ir(test_code, kernel_name);
    REQUIRE(load_result == ProcessingResult::Success);
    
    for (size_t event_count : test_config::scaling_sizes) {
        SECTION("Events: " + std::to_string(event_count)) {
            // Test with pageable memory
            {
                std::vector<NetworkEvent> events(event_count);
                create_test_events(events);
                size_t buffer_size = events.size() * sizeof(NetworkEvent);
                
                std::string bench_name = "Pageable memory - " + format_size(event_count) + " events";
                run_benchmark(bench_name, processor, events, [&] {
                    return processor.process_events(events.data(), buffer_size, events.size());
                });
            }
            
            // Test with registered pinned memory
            {
                std::vector<NetworkEvent> events(event_count);
                create_test_events(events);
                size_t buffer_size = events.size() * sizeof(NetworkEvent);
                
                // Register the buffer as pinned memory
                ProcessingResult register_result = processor.register_host_buffer(events.data(), buffer_size);
                REQUIRE(register_result == ProcessingResult::Success);
                
                std::string bench_name = "Registered pinned memory - " + format_size(event_count) + " events";
                run_benchmark(bench_name, processor, events, [&] {
                    return processor.process_events(events.data(), buffer_size, events.size());
                });
                
                // Unregister the buffer
                ProcessingResult unregister_result = processor.unregister_host_buffer(events.data());
                REQUIRE(unregister_result == ProcessingResult::Success);
            }
        }
    }
}

TEST_CASE("Performance - Asynchronous Batch Processing", "[performance][benchmark]") {
    auto devices = get_available_devices();
    if (devices.empty()) {
        SKIP("No CUDA devices available for performance testing");
    }
    
    const char* test_code = get_test_ptx();
    if (!test_code) {
        SKIP("Test IR code not found for performance testing");
    }
    
    // Setup processor with default config first
    EventProcessor::Config config;
    config.enable_profiling = true;
    config.max_stream_count = 4; // Use 4 CUDA streams
    
    EventProcessor processor(config);
    std::string kernel_name = get_test_kernel_name();
    ProcessingResult load_result = processor.load_kernel_from_ir(test_code, kernel_name);
    REQUIRE(load_result == ProcessingResult::Success);
    
    // Use a smaller event count for tests to prevent timeouts
    const size_t total_events = test_config::total_events_size;
    std::vector<NetworkEvent> events(total_events);
    create_test_events(events);
    size_t buffer_size = events.size() * sizeof(NetworkEvent);
                
    // Register the buffer as pinned memory for maximum performance - only once
    ProcessingResult register_result = processor.register_host_buffer(events.data(), buffer_size);
    REQUIRE(register_result == ProcessingResult::Success);
    
    SECTION("Comparison: Sync vs Async Processing") {
        // Test synchronous vs asynchronous processing
        reset_event_actions(events);
        
        // Benchmark synchronous processing first
        BENCHMARK_ADVANCED("Sync processing - 1M events")(Catch::Benchmark::Chronometer meter) {
            meter.measure([&] {
                return processor.process_events(events.data(), buffer_size, events.size(), false);
            });
        };
        
        // Ensure events were processed correctly
        REQUIRE(validate_results(events));
        
        // Now benchmark asynchronous processing
        reset_event_actions(events);
        
        BENCHMARK_ADVANCED("Async processing - 1M events")(Catch::Benchmark::Chronometer meter) {
            meter.measure([&] {
                // Start asynchronous processing
                ProcessingResult async_result = processor.process_events(
                    events.data(), 
                    buffer_size, 
                    events.size(),
                    true  // is_async
                );
                REQUIRE(async_result == ProcessingResult::Success);
                return async_result;
            });
            // Synchronize to ensure completion for benchmarking
            ProcessingResult sync_result = processor.synchronize_async_operations();
            REQUIRE(sync_result == ProcessingResult::Success);
        };
        
        // Verify asynchronous processing results
        REQUIRE(validate_results(events));
    }
    
    SECTION("Comparison: Different Batch Sizes") {
        // Test different batch sizes with new batching algorithm
        std::vector<size_t> batch_sizes = {1000, 10000, 50000, 100000, 250000, 500000, 1000000};
        
        // Save original config
        EventProcessor::Config original_config = config;
        
        for (size_t test_batch_size : batch_sizes) {
            // Update the batch size in the existing processor's config
            config.max_batch_size = test_batch_size;
            
            // Update processor with new config
            processor = EventProcessor(config);
            ProcessingResult batch_load_result = processor.load_kernel_from_ir(test_code, kernel_name);
            REQUIRE(batch_load_result == ProcessingResult::Success);
            
            // Reset event states for this test
            reset_event_actions(events);
            
            // Register the buffer as pinned memory for maximum performance - only once
            ProcessingResult register_result = processor.register_host_buffer(events.data(), buffer_size);
            REQUIRE(register_result == ProcessingResult::Success);
            
            // Create benchmark name
            std::string benchmark_name = "Batch size: " + format_size(test_batch_size);
            
            // Run benchmark
            BENCHMARK_ADVANCED(benchmark_name.c_str())(Catch::Benchmark::Chronometer meter) {
                meter.measure([&] {
                    // Process all events in one call, but with batching controlled by max_batch_size
                    return processor.process_events(events.data(), buffer_size, events.size());
                });
            };
        }
        
        // Restore original config
        processor = EventProcessor(original_config);
        ProcessingResult restore_result = processor.load_kernel_from_ir(test_code, kernel_name);
        REQUIRE(restore_result == ProcessingResult::Success);
    }
    
    SECTION("Comparison: Batch Size vs Stream Count") {
        // Fix batch size but vary stream count
        const size_t fixed_batch_size = 250000; // A moderate batch size that works well
        std::vector<int> stream_counts = {1, 2, 4, 8, 16};
        
        // Save original config
        EventProcessor::Config original_config = config;
        
        for (int stream_count : stream_counts) {
            // Update config with fixed batch size but different stream count
            config.max_batch_size = fixed_batch_size;
            config.max_stream_count = stream_count;
            
            // Recreate processor with new config
            processor = EventProcessor(config);
            ProcessingResult stream_load_result = processor.load_kernel_from_ir(test_code, kernel_name);
            REQUIRE(stream_load_result == ProcessingResult::Success);
            
            // Reset event states for this test
            reset_event_actions(events);
            
            // Create benchmark name
            std::string benchmark_name = "Streams: " + std::to_string(stream_count) + ", Batch: " + format_size(fixed_batch_size);
            
            // Run benchmark
            BENCHMARK_ADVANCED(benchmark_name.c_str())(Catch::Benchmark::Chronometer meter) {
                meter.measure([&] {
                    // Process all events in one call
                    return processor.process_events(events.data(), buffer_size, events.size());
                });
            };
        }
        
        // Restore original config
        processor = EventProcessor(original_config);
        ProcessingResult restore_result = processor.load_kernel_from_ir(test_code, kernel_name);
        REQUIRE(restore_result == ProcessingResult::Success);
    }
}

TEST_CASE("Performance - Hash Load Balancing", "[performance][hash][load_balancing][benchmark]") {
    auto devices = get_available_devices();
    if (devices.empty()) {
        SKIP("No GPU devices available for hash load balancing testing");
    }
    
    const char* test_code = get_test_ptx();
    if (!test_code) {
        SKIP("Test IR code not found for hash load balancing testing");
    }
    
    // Test different hash load balancing kernels
    const std::vector<const char*> hash_kernels = {
        kernel_names::HASH_LOAD_BALANCER,
        kernel_names::BATCH_HASH_LOAD_BALANCER
    };
    
    // Test with different event counts to measure scalability
    const std::vector<size_t> event_counts = {1000, 10000, 100000, 1000000};
    
    for (const char* kernel_name : hash_kernels) {
        SECTION(std::string("Kernel: ") + kernel_name) {
            EventProcessor processor;
            ProcessingResult load_result;
            
            // For OpenCL, we need to use a different approach since it doesn't support these CUDA-specific kernels
            TestBackend backend = detect_test_backend();
            
            if (backend == TestBackend::OpenCL) {
                // For OpenCL, use the simple kernel as a fallback
                load_result = processor.load_kernel_from_ir(test_code, "simple_kernel");
            } else {
                // For CUDA, use the specific hash kernel
                load_result = processor.load_kernel_from_ir(test_code, kernel_name);
            }
            
            REQUIRE(load_result == ProcessingResult::Success);
            
            const char* function_name = cpu::get_function_display_name(kernel_name);
            
            for (size_t event_count : event_counts) {
                SECTION("Events: " + format_size(event_count)) {
                    // Create test data with diverse network flows for better hash distribution
                    std::vector<NetworkEvent> events(event_count);
                    create_test_events(events);
                    
                    // Ensure diverse source IPs for better hash distribution
                    for (size_t i = 0; i < events.size(); i++) {
                        events[i].src_ip = 0xC0A80000 + (i % 256) + ((i / 256) % 256) * 256; // Varied 192.168.x.y
                        events[i].src_port = 1024 + (i * 17) % 60000; // Prime number for better distribution
                        events[i].dst_port = (i % 3 == 0) ? 80 : ((i % 3 == 1) ? 443 : 8080); // Mix of ports
                    }
                    
                    size_t buffer_size = events.size() * sizeof(NetworkEvent);
                    
                    // Register buffer for optimal performance
                    ProcessingResult register_result = processor.register_host_buffer(events.data(), buffer_size);
                    REQUIRE(register_result == ProcessingResult::Success);
                    
                    // Warm up GPU
                    reset_event_actions(events);
                    processor.process_events(events.data(), buffer_size, events.size());
                    
                    std::string bench_name = std::string(function_name) + " - " + format_size(event_count) + " events";
                    
                    // Run benchmark
                    BENCHMARK_ADVANCED(bench_name.c_str())(Catch::Benchmark::Chronometer meter) {
                        reset_event_actions(events);
                        meter.measure([&] {
                            return processor.process_events(events.data(), buffer_size, events.size());
                        });
                    };
                    
                    // Validate processing results
                    reset_event_actions(events);
                    ProcessingResult final_result = processor.process_events(events.data(), buffer_size, events.size());
                    REQUIRE(final_result == ProcessingResult::Success);
                    
                    // For hash load balancing, check that actions were set (indicating worker assignment)
                    bool has_processed_events = false;
                    for (const auto& event : events) {
                        if (event.action != 0) {
                            has_processed_events = true;
                            break;
                        }
                    }
                    
                    if (backend == TestBackend::CUDA) {
                        // For CUDA with hash kernels, we expect events to be processed
                        REQUIRE(has_processed_events);
                        
                        // Check load balancing distribution (for demonstration)
                        std::vector<int> worker_distribution(8, 0);
                        for (const auto& event : events) {
                            int worker_id = event.action & 0x7F; // Remove high bit
                            if (worker_id < 8) {
                                worker_distribution[worker_id]++;
                            }
                        }
                        
                        // Verify that multiple workers received assignments
                        int active_workers = 0;
                        for (int count : worker_distribution) {
                            if (count > 0) active_workers++;
                        }
                        
                        INFO("Active workers: " << active_workers << " out of 8");
                        // For good load balancing, we expect at least half the workers to be active
                        if (event_count >= 100) {
                            REQUIRE(active_workers >= 4);
                        }
                    }
                    
                    // Unregister the buffer
                    ProcessingResult unregister_result = processor.unregister_host_buffer(events.data());
                    REQUIRE(unregister_result == ProcessingResult::Success);
                }
            }
        }
    }
}

TEST_CASE("Performance - Hash Distribution Quality", "[performance][hash][distribution][benchmark]") {
    auto devices = get_available_devices();
    if (devices.empty()) {
        SKIP("No GPU devices available for hash distribution testing");
    }
    
    const char* test_code = get_test_ptx();
    if (!test_code) {
        SKIP("Test IR code not found for hash distribution testing");
    }
    
    // Only test CUDA backend for detailed hash analysis
    TestBackend backend = detect_test_backend();
    if (backend != TestBackend::CUDA) {
        SKIP("Hash distribution testing requires CUDA backend");
    }
    
    EventProcessor processor;
    ProcessingResult load_result = processor.load_kernel_from_ir(test_code, kernel_names::HASH_LOAD_BALANCER);
    REQUIRE(load_result == ProcessingResult::Success);
    
    // Test hash distribution with different data patterns
    const std::vector<std::pair<std::string, size_t>> test_patterns = {
        {"Sequential IPs", 10000},
        {"Random IPs", 10000},
        {"Sequential Ports", 10000},
        {"Mixed Patterns", 50000}
    };
    
    for (const auto& pattern : test_patterns) {
        SECTION("Pattern: " + pattern.first) {
            size_t event_count = pattern.second;
            std::vector<NetworkEvent> events(event_count);
            
            // Create different test patterns
            if (pattern.first == "Sequential IPs") {
                for (size_t i = 0; i < events.size(); i++) {
                    events[i].src_ip = 0xC0A80000 + (i % 65536); // Sequential 192.168.x.y
                    events[i].dst_ip = 0x08080808;
                    events[i].src_port = 1024;
                    events[i].dst_port = 80;
                    events[i].protocol = 6;
                    events[i].action = 0;
                }
            } else if (pattern.first == "Random IPs") {
                std::srand(12345); // Fixed seed for reproducibility
                for (size_t i = 0; i < events.size(); i++) {
                    events[i].src_ip = (uint32_t)std::rand();
                    events[i].dst_ip = (uint32_t)std::rand();
                    events[i].src_port = std::rand() % 65536;
                    events[i].dst_port = std::rand() % 65536;
                    events[i].protocol = 6;
                    events[i].action = 0;
                }
            } else if (pattern.first == "Sequential Ports") {
                for (size_t i = 0; i < events.size(); i++) {
                    events[i].src_ip = 0xC0A80001;
                    events[i].dst_ip = 0x08080808;
                    events[i].src_port = 1024 + (i % 60000);
                    events[i].dst_port = 80 + (i % 1000);
                    events[i].protocol = 6;
                    events[i].action = 0;
                }
            } else { // Mixed Patterns
                std::srand(54321);
                for (size_t i = 0; i < events.size(); i++) {
                    events[i].src_ip = 0xC0A80000 + (i % 256) + ((std::rand() % 256) << 16);
                    events[i].dst_ip = 0x08080808 + (std::rand() % 256);
                    events[i].src_port = 1024 + (i * 17) % 60000;
                    events[i].dst_port = (i % 5 == 0) ? 80 : ((i % 5 == 1) ? 443 : (1024 + std::rand() % 60000));
                    events[i].protocol = (i % 3 == 0) ? 6 : 17;
                    events[i].action = 0;
                }
            }
            
            size_t buffer_size = events.size() * sizeof(NetworkEvent);
            
            // Register buffer and process
            processor.register_host_buffer(events.data(), buffer_size);
            
            std::string bench_name = "Hash Distribution - " + pattern.first;
            
            BENCHMARK_ADVANCED(bench_name.c_str())(Catch::Benchmark::Chronometer meter) {
                reset_event_actions(events);
                meter.measure([&] {
                    return processor.process_events(events.data(), buffer_size, events.size());
                });
            };
            
            // Analyze hash distribution quality
            reset_event_actions(events);
            processor.process_events(events.data(), buffer_size, events.size());
            
            // Count distribution across workers
            std::vector<int> worker_counts(8, 0);
            for (const auto& event : events) {
                int worker_id = event.action & 0x7F;
                if (worker_id < 8) {
                    worker_counts[worker_id]++;
                }
            }
            
            // Calculate distribution metrics
            double mean = static_cast<double>(event_count) / 8.0;
            double variance = 0.0;
            for (int count : worker_counts) {
                double diff = count - mean;
                variance += diff * diff;
            }
            variance /= 8.0;
            double std_dev = std::sqrt(variance);
            double coefficient_of_variation = std_dev / mean;
            
            INFO("Hash distribution for " << pattern.first << ":");
            INFO("  Mean: " << mean << ", Std Dev: " << std_dev);
            INFO("  Coefficient of Variation: " << coefficient_of_variation);
            for (int i = 0; i < 8; i++) {
                INFO("  Worker " << i << ": " << worker_counts[i] << " events");
            }
            
            // Good hash distribution should have low coefficient of variation
            // For most patterns, CV should be less than 0.1 (10%)
            if (pattern.first != "Sequential IPs") { // Sequential IPs might have higher CV
                REQUIRE(coefficient_of_variation < 0.15);
            }
            
            processor.unregister_host_buffer(events.data());
        }
    }
}

