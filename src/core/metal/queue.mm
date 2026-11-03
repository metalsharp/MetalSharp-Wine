#include "core/metal/queue.h"

#include <atomic>
#include <chrono>

#include "core/metal/device.h"
#include "core/stats.h"
#include "core/util/log.h"

namespace wmgl {

namespace {

const char* commandBufferErrorName(NSInteger code) {
    switch (code) {
    case MTLCommandBufferErrorInternal: return "internal error";
    case MTLCommandBufferErrorTimeout: return "timed out";
    case MTLCommandBufferErrorPageFault: return "GPU page fault";
    case MTLCommandBufferErrorAccessRevoked: return "access revoked";
    case MTLCommandBufferErrorNotPermitted: return "not permitted";
    case MTLCommandBufferErrorOutOfMemory: return "out of memory";
    case MTLCommandBufferErrorInvalidResource: return "invalid resource";
    case MTLCommandBufferErrorMemoryless: return "memoryless storage exhausted";
    case MTLCommandBufferErrorDeviceRemoved: return "device removed";
    case MTLCommandBufferErrorStackOverflow: return "stack overflow";
    default: return "error";
    }
}

const char* encoderStateName(MTLCommandEncoderErrorState state) {
    switch (state) {
    case MTLCommandEncoderErrorStateCompleted: return "completed";
    case MTLCommandEncoderErrorStateAffected: return "affected";
    case MTLCommandEncoderErrorStatePending: return "pending";
    case MTLCommandEncoderErrorStateFaulted: return "FAULTED";
    default: return "unknown";
    }
}

// Each encoder's state is recorded (at some cost) when GPU errors are logged
// in detail: WINEMETALGL_LOG=gpu:2.
bool detailedErrors() {
    static const bool detailed = logEnabled(LogLevel::Info, LogCategory::Gpu);
    return detailed;
}

// The first failures are logged, then one in 256: a hung GPU fails every buffer.
std::atomic<uint64_t> g_failures{0};

void logFailure(id<MTLCommandBuffer> done, uint64_t serial) {
    const uint64_t n = g_failures.fetch_add(1);
    if (n >= 16 && n % 256) return;
    NSError* error = done.error;
    WMGL_ERR(Gpu, "command buffer %llu failed: %s (%s %ld: %s)%s", static_cast<unsigned long long>(serial),
             commandBufferErrorName(error.code), error.domain.UTF8String, static_cast<long>(error.code),
             error.localizedDescription.UTF8String, n >= 16 ? " [failures are now logged one in 256]" : "");
    NSArray* infos = error.userInfo[MTLCommandBufferEncoderInfoErrorKey];
    for (id<MTLCommandBufferEncoderInfo> info in infos) {
        NSString* signposts = info.debugSignposts.count ? [info.debugSignposts componentsJoinedByString:@" > "] : @"";
        WMGL_ERR(Gpu, "  encoder \"%s\": %s%s%s", info.label.UTF8String ?: "", encoderStateName(info.errorState),
                 signposts.length ? ", signposts " : "", signposts.UTF8String);
    }
}

}  // namespace

Queue::Queue() {
    queue_ = [Device::get().mtl() newCommandQueueWithMaxCommandBufferCount:256];
    queue_.label = @"WineMetalGL";
}

id<MTLCommandBuffer> Queue::create(uint64_t* serial) {
    id<MTLCommandBuffer> buffer;
    if (detailedErrors()) {
        MTLCommandBufferDescriptor* desc = [MTLCommandBufferDescriptor new];
        desc.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
        buffer = [queue_ commandBufferWithDescriptor:desc];
    } else {
        buffer = [queue_ commandBuffer];
    }
    if (!buffer) logOnce(LogLevel::Error, LogCategory::Gpu, "no-command-buffer", "Metal gave no command buffer");
    std::lock_guard<std::mutex> lock(mutex_);
    *serial = next_++;
    reserved_.insert(*serial);
    return buffer;
}

void Queue::commit(id<MTLCommandBuffer> buffer, uint64_t serial) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reserved_.erase(serial);
        inflight_.insert(serial);
    }
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (stats::g_enabled && done.GPUEndTime > done.GPUStartTime) {
            stats::addGpuNanos(static_cast<uint64_t>((done.GPUEndTime - done.GPUStartTime) * 1e9));
            stats::addGpuInterval(done.GPUStartTime, done.GPUEndTime);
        }
        if (done.status == MTLCommandBufferStatusError) logFailure(done, serial);
        // Notify under the lock: once a waiter sees the queue idle it may
        // destroy the queue, so nothing may touch it after the unlock.
        std::lock_guard<std::mutex> lock(mutex_);
        inflight_.erase(serial);
        completed_.notify_all();
    }];
    [buffer commit];
}

void Queue::abandon(uint64_t serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    reserved_.erase(serial);
    completed_.notify_all();  // fence waiters (waitSignaled) stop waiting for it
}

bool Queue::isComplete(uint64_t serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    return !inflight_.count(serial) && !reserved_.count(serial);
}

bool Queue::idle() {
    std::lock_guard<std::mutex> lock(mutex_);
    return inflight_.empty();
}

bool Queue::isCommitted(uint64_t serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    return !reserved_.count(serial);
}

void Queue::wait(uint64_t serial) {
    std::unique_lock<std::mutex> lock(mutex_);
    completed_.wait(lock, [&] { return !inflight_.count(serial); });
}

namespace {

// Waits until `done`, or `timeoutNs` (UINT64_MAX: no timeout). A timeout past the clock's range is a
// wait without one too: wined3d waits ~0 >> 1 ns, and now plus that overflowed into a deadline in the
// past, so each of its fence waits timed out at once.
template <typename Done>
bool waitUpTo(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, uint64_t timeoutNs, Done done) {
    const auto now = std::chrono::steady_clock::now();
    const auto room =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::time_point::max() - now);
    if (timeoutNs == UINT64_MAX || timeoutNs >= static_cast<uint64_t>(room.count())) {
        cv.wait(lock, done);
        return true;
    }
    return cv.wait_until(lock, now + std::chrono::nanoseconds(timeoutNs), done);
}

}  // namespace

bool Queue::wait(uint64_t serial, uint64_t timeoutNs) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto done = [&] { return !inflight_.count(serial); };
    return waitUpTo(completed_, lock, timeoutNs, done);
}

bool Queue::waitSignaled(uint64_t serial, uint64_t timeoutNs) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto done = [&] { return !inflight_.count(serial) && !reserved_.count(serial); };
    return waitUpTo(completed_, lock, timeoutNs, done);
}

void Queue::waitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    completed_.wait(lock, [&] { return inflight_.empty(); });
}

uint64_t Queue::lastSerial() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_ - 1;
}

}  // namespace wmgl
