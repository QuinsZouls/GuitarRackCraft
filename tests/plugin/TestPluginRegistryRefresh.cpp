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
 * Contract of PluginRegistry::refreshAll(), the entry point behind
 * nativeRefreshPluginRegistry: it has to reach IPluginFactory::refresh() (not
 * initialize()) and rebuild the "FORMAT:id" cache from the factories' current
 * enumeration, so an import is visible without restarting the engine.
 */

#include "plugin/IPluginFactory.h"
#include "plugin/PluginRegistry.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

using guitarrackcraft::IPluginFactory;
using guitarrackcraft::PluginInfo;
using guitarrackcraft::PluginRegistry;

class FakeFactory : public IPluginFactory {
public:
    explicit FakeFactory(std::string format, bool initResult = true, bool refreshResult = true)
        : format_(std::move(format)), initResult_(initResult), refreshResult_(refreshResult) {}

    std::string getFormat() const override { return format_; }

    bool initialize() override {
        ++initializeCalls;
        return initResult_;
    }

    bool refresh() override {
        ++refreshCalls;
        return refreshResult_;
    }

    std::vector<PluginInfo> enumeratePlugins() override { return plugins; }

    std::unique_ptr<guitarrackcraft::IPlugin> createPlugin(const std::string& pluginId) override {
        lastCreatedId = pluginId;
        return nullptr;
    }

    void addPlugin(const std::string& id) {
        PluginInfo info;
        info.id = id;
        info.name = "name-" + id;
        info.format = format_;
        plugins.push_back(info);
    }

    void setInitResult(bool value) { initResult_ = value; }
    void setRefreshResult(bool value) { refreshResult_ = value; }

    int initializeCalls = 0;
    int refreshCalls = 0;
    std::string lastCreatedId;
    std::vector<PluginInfo> plugins;

private:
    std::string format_;
    bool initResult_;
    bool refreshResult_;
};

TEST(PluginRegistryRefreshTest, RefreshUsesRefreshNotInitialize) {
    auto factory = std::make_unique<FakeFactory>("LV2");
    FakeFactory* raw = factory.get();
    raw->addPlugin("http://example.org/a");

    PluginRegistry registry;
    registry.registerFactory(std::move(factory));

    ASSERT_TRUE(registry.initializeAll());
    EXPECT_EQ(raw->initializeCalls, 1);
    EXPECT_EQ(raw->refreshCalls, 0);

    raw->addPlugin("http://example.org/b");
    ASSERT_TRUE(registry.refreshAll());
    EXPECT_EQ(raw->initializeCalls, 1);
    EXPECT_EQ(raw->refreshCalls, 1);

    EXPECT_EQ(registry.getAllPlugins().size(), 2u);
}

TEST(PluginRegistryRefreshTest, RefreshPicksUpNewAndDroppedPlugins) {
    auto factory = std::make_unique<FakeFactory>("LV2");
    FakeFactory* raw = factory.get();
    raw->addPlugin("http://example.org/a");
    raw->addPlugin("http://example.org/b");

    PluginRegistry registry;
    registry.registerFactory(std::move(factory));
    ASSERT_TRUE(registry.initializeAll());
    ASSERT_EQ(registry.getAllPlugins().size(), 2u);
    EXPECT_EQ(registry.getPluginInfo("LV2:http://example.org/b").name, "name-http://example.org/b");

    raw->plugins.clear();
    raw->addPlugin("http://example.org/c");
    ASSERT_TRUE(registry.refreshAll());

    const auto plugins = registry.getAllPlugins();
    ASSERT_EQ(plugins.size(), 1u);
    EXPECT_EQ(plugins.front().id, "http://example.org/c");
    EXPECT_EQ(registry.getPluginInfo("LV2:http://example.org/a").id, std::string())
        << "dropped plugins must leave the cache";
}

TEST(PluginRegistryRefreshTest, RefreshKeepsRoutingAndCacheKeyFormat) {
    auto lv2 = std::make_unique<FakeFactory>("LV2");
    FakeFactory* rawLv2 = lv2.get();
    rawLv2->addPlugin("http://example.org/a");

    auto vst = std::make_unique<FakeFactory>("VST2");
    FakeFactory* rawVst = vst.get();
    rawVst->addPlugin("uuid-1");

    PluginRegistry registry;
    registry.registerFactory(std::move(lv2));
    registry.registerFactory(std::move(vst));
    ASSERT_TRUE(registry.initializeAll());
    ASSERT_TRUE(registry.refreshAll());

    EXPECT_EQ(registry.getPluginInfo("LV2:http://example.org/a").format, "LV2");
    EXPECT_EQ(registry.getPluginInfo("VST2:uuid-1").format, "VST2");

    EXPECT_EQ(registry.createPlugin("VST2:uuid-1").get(), nullptr);
    EXPECT_EQ(rawVst->lastCreatedId, "uuid-1");
    EXPECT_EQ(registry.createPlugin("LV2:http://example.org/a").get(), nullptr);
    EXPECT_EQ(rawLv2->lastCreatedId, "http://example.org/a");

    EXPECT_EQ(registry.createPlugin("CLAP:unknown").get(), nullptr);
    EXPECT_EQ(registry.createPlugin("no-format-prefix").get(), nullptr);
}

TEST(PluginRegistryRefreshTest, FailingRefreshIsReportedAndLeavesThatFactoryOut) {
    auto factory = std::make_unique<FakeFactory>("LV2");
    FakeFactory* raw = factory.get();
    raw->addPlugin("http://example.org/a");

    PluginRegistry registry;
    registry.registerFactory(std::move(factory));
    ASSERT_TRUE(registry.initializeAll());

    raw->addPlugin("http://example.org/b");
    raw->setRefreshResult(false);

    EXPECT_FALSE(registry.refreshAll());
    EXPECT_EQ(raw->refreshCalls, 1);
    EXPECT_TRUE(registry.getAllPlugins().empty())
        << "same rule as initializeAll: a factory that cannot refresh contributes nothing";

    raw->setRefreshResult(true);
    EXPECT_TRUE(registry.refreshAll());
    EXPECT_EQ(registry.getAllPlugins().size(), 2u);
}

TEST(PluginRegistryRefreshTest, RefreshWithoutFactoriesSucceedsWithEmptyCache) {
    PluginRegistry registry;
    EXPECT_TRUE(registry.refreshAll());
    EXPECT_TRUE(registry.getAllPlugins().empty());
}

} // namespace
