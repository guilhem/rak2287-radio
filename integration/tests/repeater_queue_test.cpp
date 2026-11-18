#include "repeater_queue.h"
#include <helpers/StaticPoolPacketManager.h>
#include <cassert>

int main() {
    StaticPoolPacketManager native(3);
    RepeaterQueue queue(native);
    auto *middle = queue.allocNew();
    auto *high = queue.allocNew();
    auto *low = queue.allocNew();
    assert(middle && high && low);
    queue.queueOutbound(middle, 7, 0);
    queue.queueOutbound(high, 1, 50);
    queue.queueOutbound(low, 9, 0);

    assert(queue.getNextOutbound(10) == middle);
    queue.defer(middle, 60);
    queue.free(middle);  // Dispatcher releases a send that the driver vetoed.
    assert(queue.getFreeCount() == 0 && queue.getOutboundCount(59) == 2);
    assert(queue.getNextOutbound(60) == high);
    queue.free(high);
    assert(queue.getNextOutbound(60) == middle);
    queue.free(middle);
    assert(queue.getNextOutbound(60) == low);
    queue.free(low);

    // Pointers recycled by the native pool must use their new priorities.
    assert(queue.getFreeCount() == 3);
    auto *reuse = queue.allocNew();
    auto *other = queue.allocNew();
    assert(reuse == high && other == middle);
    queue.queueOutbound(reuse, 10, 0);
    assert(queue.getNextOutbound(0) == reuse);
    queue.defer(reuse, 20);
    queue.free(reuse);
    queue.queueOutbound(other, 5, 0);
    assert(queue.getNextOutbound(20) == other);
    queue.free(other);
    assert(queue.getNextOutbound(20) == reuse);
    queue.free(reuse);
    assert(queue.getFreeCount() == 3 && queue.getNextOutbound(20) == nullptr);
}
