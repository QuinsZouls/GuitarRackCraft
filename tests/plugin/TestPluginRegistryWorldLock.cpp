/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Lock protocol of PluginRegistry::initializeAll()/refreshAll() against the LV2
 * world (see plugin/lv2/LV2WorldLock.h).
 *
 * The bug being pinned: refreshAll() mutated the shared LilvWorld
 * (lilv_world_load_bundle) while JNI entry points instantiated plugins and read
 * their info from the same world. The fix serializes the two with one
 * process-wide mutex: both the rebuilds and the readers take it exclusively,
 * because lilv has no read-only calls (see LV2WorldLock.h). These tests drive
 * the registry (not lilv) and assert the lock is really held for the whole
 * rebuild, from another thread that would otherwise get in.
 */

#include "plugin/IPluginFactory.h"
#include "plugin/PluginRegistry.h"
#include "plugin/lv2/LV2WorldLock.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using guitarrackcraft::IPluginFactory;
using guitarrackcraft::PluginInfo;
using guitarrackcraft::PluginRegistry;
namespace lv2world = guitarrackcraft::lv2world;

using namespace std::chrono_literals;

/** Which registry entry point parks inside the factory. */
enum class Gate {
    Initialize,
    Refresh,
};

/** Factory that blocks inside initialize()/refresh() until the test releases it,
 *  which keeps the world lock it holds observable from other threads. */
class GatedFactory : public IPluginFactory {
public:
    explicit GatedFactory(Gate gate) : gate_(gate) {}

    std::string getFormat() const override { return "LV2"; }

    bool initialize() override {
        if (gate_ == Gate::Initialize) {
            park();
        }
        return true;
    }

    bool refresh() override {
        if (gate_ == Gate::Refresh) {
            park();
        }
        return true;
    }

    std::vector<PluginInfo> enumeratePlugins() override { return {}; }

    std::unique_ptr<guitarrackcraft::IPlugin> createPlugin(const std::string&) override {
        return nullptr;
    }

    bool waitUntilParked(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return parked_; });
    }

    void release() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    void park() {
        std::unique_lock<std::mutex> lock(mutex_);
        parked_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return released_; });
    }

    Gate gate_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool parked_ = false;
    bool released_ = false;
};

/** Tries to take the world lock, as a JNI entry point would. */
bool tryLockTheWorld() {
    if (!lv2world::lock().try_lock()) {
        return false;
    }
    lv2world::lock().unlock();
    return true;
}

} // namespace

TEST(PluginRegistryWorldLockTest, RefreshHoldsTheWorldForWriting) {
    auto factory = std::make_unique<GatedFactory>(Gate::Refresh);
    GatedFactory* raw = factory.get();
    PluginRegistry registry;
    registry.registerFactory(std::move(factory));

    std::atomic<bool> refreshed{false};
    std::thread rebuilder([&registry, &refreshed] {
        registry.refreshAll();
        refreshed = true;
    });

    ASSERT_TRUE(raw->waitUntilParked(5s)) << "refresh() never started";

    // While the rebuild runs, no reader may hold the world...
    EXPECT_FALSE(tryLockTheWorld());
    // ...and a reader (plugin creation, getInfo, activation) has to wait.
    std::atomic<bool> readerDone{false};
    std::thread reader([&readerDone] {
        lv2world::Guard worldLock = lv2world::guard();
        readerDone = true;
    });
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(readerDone) << "a world user got in during a rebuild";

    raw->release();
    rebuilder.join();
    reader.join();

    EXPECT_TRUE(refreshed);
    EXPECT_TRUE(readerDone);
    EXPECT_TRUE(tryLockTheWorld()) << "the world lock must be released after a rebuild";
}

TEST(PluginRegistryWorldLockTest, RefreshWaitsForReaders) {
    auto factory = std::make_unique<GatedFactory>(Gate::Refresh);
    GatedFactory* raw = factory.get();
    PluginRegistry registry;
    registry.registerFactory(std::move(factory));

    lv2world::Guard heldByTest = lv2world::guard();

    std::thread rebuilder([&registry] { registry.refreshAll(); });

    // A rebuild that ignored the world lock would enter the factory anyway.
    EXPECT_FALSE(raw->waitUntilParked(150ms))
        << "refresh() ran while a reader was inside the world";

    heldByTest.unlock();
    ASSERT_TRUE(raw->waitUntilParked(5s)) << "refresh() did not start after the reader left";

    raw->release();
    rebuilder.join();
}

TEST(PluginRegistryWorldLockTest, InitializeHoldsTheWorldForWriting) {
    auto factory = std::make_unique<GatedFactory>(Gate::Initialize);
    GatedFactory* raw = factory.get();
    PluginRegistry registry;
    registry.registerFactory(std::move(factory));

    std::thread initializer([&registry] { registry.initializeAll(); });

    ASSERT_TRUE(raw->waitUntilParked(5s)) << "initialize() never started";
    EXPECT_FALSE(tryLockTheWorld())
        << "initializeAll() loads the world, so it must exclude other world users";

    raw->release();
    initializer.join();
}
