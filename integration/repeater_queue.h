#pragma once

#include <Dispatcher.h>
#include <unordered_map>

// The native dispatcher releases its selected packet after startSendRaw(false).
// Requeue that exact allocation and its original priority only for a driver
// veto before RF0 started. All pool storage and scheduling stay in MeshCore.
class RepeaterQueue final : public mesh::PacketManager {
    mesh::PacketManager& pool;
    std::unordered_map<mesh::Packet*, uint8_t> priorities;
    mesh::Packet* retry = nullptr;
    uint32_t retryAt = 0;

public:
    explicit RepeaterQueue(mesh::PacketManager& base) : pool(base) {}
    void defer(mesh::Packet* p, uint32_t when) { retry = p; retryAt = when; }

    mesh::Packet* allocNew() override { return pool.allocNew(); }
    void free(mesh::Packet* p) override {
        if (retry && p == retry) {
            retry = nullptr;
            pool.queueOutbound(p, priorities.at(p), retryAt);
        } else {
            priorities.erase(p);
            pool.free(p);
        }
    }
    void queueOutbound(mesh::Packet* p, uint8_t priority, uint32_t when) override {
        priorities[p] = priority;
        pool.queueOutbound(p, priority, when);
    }
    mesh::Packet* getNextOutbound(uint32_t now) override { return pool.getNextOutbound(now); }
    int getOutboundCount(uint32_t now) const override { return pool.getOutboundCount(now); }
    int getFreeCount() const override { return pool.getFreeCount(); }
    mesh::Packet* getOutboundByIdx(int i) override { return pool.getOutboundByIdx(i); }
    mesh::Packet* removeOutboundByIdx(int i) override {
        auto* p = pool.removeOutboundByIdx(i);
        priorities.erase(p);
        return p;
    }
    void queueInbound(mesh::Packet* p, uint32_t when) override { pool.queueInbound(p, when); }
    mesh::Packet* getNextInbound(uint32_t now) override { return pool.getNextInbound(now); }
};
