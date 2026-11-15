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
 * The race the world lock exists for, against the real lilv stack: an import
 * (lilv_world_load_bundle + load_specifications + load_plugin_classes) running
 * while the rack instantiates plugins and reads their info out of the same
 * world. Both sides go through guitarrackcraft::lv2world exactly like
 * PluginRegistry::refreshAll() (write) and the JNI entry points (read).
 *
 * Run as-is this test checks the functional invariants (the plugin that is
 * already loaded is always findable and instantiable, every new bundle really
 * became visible). Built with ThreadSanitizer it is the race detector for the
 * lilv/sord model itself:
 *
 *    cmake -S tests -B build/host-tests-tsan -DCMAKE_BUILD_TYPE=Debug \
 *          -DGRC_HOST_TSAN=ON
 *    cmake --build build/host-tests-tsan -j8
 *    ./build/host-tests-tsan/lv2_world_lock_tests
 *
 * Without the world lock TSan reports data races between load_bundle() and
 * lilv_plugin_get_name()/instantiate() in this test.
 */

#include "plugin/lv2/LV2WorldLock.h"

#include <gtest/gtest.h>

#include <lilv/lilv.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef GRC_LV2_TEST_PLUGIN
#error "GRC_LV2_TEST_PLUGIN must point at the compiled fixture plugin binary"
#endif

namespace {

namespace fs = std::filesystem;
namespace lv2world = guitarrackcraft::lv2world;

using namespace std::chrono_literals;

/** URI of the fixture binary itself (tests/lv2/TestPluginBinary.c): the reader
 *  instantiates this one, and lilv only instantiates a plugin whose URI matches
 *  a descriptor the .so exports. */
constexpr const char* kStablePlugin = "http://guitarrackcraft.org/test/plugin-bin";

class LV2WorldLockStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("grc-lv2-world-lock-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                 "-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root_);

        world_ = lilv_world_new();
        ASSERT_NE(world_, nullptr);

        createBundle("Stable.lv2", kStablePlugin);
        loadIntoWorld("Stable.lv2");
        {
            lv2world::Guard worldLock = lv2world::guard();
            lilv_world_load_specifications(world_);
            lilv_world_load_plugin_classes(world_);
        }
        ASSERT_NE(findPlugin(kStablePlugin), nullptr);
    }

    void TearDown() override {
        if (world_) {
            lilv_world_free(world_);
        }
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    /** <root>/<relativeDir>/ with a manifest, a plugin description and the
     *  fixture binary. */
    void createBundle(const std::string& relativeDir, const std::string& pluginUri) const {
        const fs::path bundle = root_ / relativeDir;
        fs::create_directories(bundle);

        std::ofstream manifest(bundle / "manifest.ttl");
        manifest << "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
                 << "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                 << "\n"
                 << "<" << pluginUri << ">\n"
                 << "    a lv2:Plugin ;\n"
                 << "    lv2:binary <plugin.so> ;\n"
                 << "    rdfs:seeAlso <plugin.ttl> .\n";
        manifest.close();

        std::ofstream pluginTtl(bundle / "plugin.ttl");
        pluginTtl << "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
                  << "@prefix doap: <http://usefulinc.com/ns/doap#> .\n"
                  << "\n"
                  << "<" << pluginUri << ">\n"
                  << "    doap:name \"" << relativeDir << "\" .\n";
        pluginTtl.close();

        fs::copy_file(GRC_LV2_TEST_PLUGIN, bundle / "plugin.so",
                      fs::copy_options::overwrite_existing);
    }

    /** What LV2PluginFactory::refresh() does to the world: load the bundle and
     *  the tables a new plugin may add to. Caller holds the write lock. */
    void loadIntoWorld(const std::string& relativeDir) const {
        const std::string path = (root_ / relativeDir).string() + "/";
        LilvNode* bundleUri = lilv_new_file_uri(world_, nullptr, path.c_str());
        ASSERT_NE(bundleUri, nullptr);
        lilv_world_load_bundle(world_, bundleUri);
        lilv_node_free(bundleUri);
    }

    /** What LV2PluginFactory::createPlugin() and LV2Plugin::getInfo() do: look
     *  the plugin up, read its name, instantiate it. */
    const LilvPlugin* findPlugin(const std::string& uri) const {
        LilvNode* node = lilv_new_uri(world_, uri.c_str());
        EXPECT_NE(node, nullptr);
        const LilvPlugin* plugin = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world_), node);
        lilv_node_free(node);
        return plugin;
    }

    fs::path root_;
    LilvWorld* world_ = nullptr;
};

TEST_F(LV2WorldLockStressTest, ImportWhileInstantiatingKeepsTheWorldUsable) {
    constexpr int kImports = 40;
    std::atomic<int> importsDone{0};
    std::atomic<int> readsDone{0};
    std::atomic<bool> readerSawTheStablePlugin{true};
    std::atomic<bool> stablePluginInstantiated{true};

    std::thread importer([&] {
        for (int i = 0; i < kImports; ++i) {
            const std::string name = "New" + std::to_string(i) + ".lv2";
            const std::string uri = "http://example.org/new" + std::to_string(i);
            createBundle(name, uri);

            // Same critical section as PluginRegistry::refreshAll().
            lv2world::Guard worldLock = lv2world::guard();
            loadIntoWorld(name);
            lilv_world_load_specifications(world_);
            lilv_world_load_plugin_classes(world_);
            ++importsDone;
        }
    });

    std::thread reader([&] {
        for (int i = 0; i < kImports; ++i) {
            // Same critical section as the JNI entry points that use the world.
            lv2world::Guard worldLock = lv2world::guard();

            const LilvPlugin* plugin = findPlugin(kStablePlugin);
            if (!plugin) {
                readerSawTheStablePlugin = false;
                continue;
            }
            const LilvNode* name = lilv_plugin_get_name(plugin);
            if (!name || std::string(lilv_node_as_string(name)).empty()) {
                readerSawTheStablePlugin = false;
            }
            LilvInstance* instance = lilv_plugin_instantiate(plugin, 48000.0, nullptr);
            if (!instance) {
                stablePluginInstantiated = false;
            } else {
                lilv_instance_free(instance);
            }
            ++readsDone;
        }
    });

    importer.join();
    reader.join();

    EXPECT_TRUE(readerSawTheStablePlugin) << "a loaded plugin disappeared while bundles were imported";
    EXPECT_TRUE(stablePluginInstantiated) << "instantiating a loaded plugin failed during an import";
    EXPECT_EQ(importsDone.load(), kImports);
    EXPECT_EQ(readsDone.load(), kImports);

    lv2world::Guard worldLock = lv2world::guard();
    for (int i = 0; i < kImports; ++i) {
        const std::string uri = "http://example.org/new" + std::to_string(i);
        EXPECT_NE(findPlugin(uri), nullptr) << "imported plugin missing: " << uri;
    }
}

TEST_F(LV2WorldLockStressTest, ConcurrentCreatorsSerializeOnTheWorldLock) {
    // Several JNI threads may create plugins at the same time. lilv instantiates
    // through the same intern trees and the same world->libs table, so they have
    // to serialize on the world lock: this pins that they do, without deadlock
    // and without corrupting the world.
    std::atomic<int> failures{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            for (int i = 0; i < 25; ++i) {
                lv2world::Guard worldLock = lv2world::guard();
                const LilvPlugin* plugin = findPlugin(kStablePlugin);
                if (!plugin) {
                    ++failures;
                    continue;
                }
                LilvInstance* instance = lilv_plugin_instantiate(plugin, 48000.0, nullptr);
                if (!instance) {
                    ++failures;
                } else {
                    lilv_instance_free(instance);
                }
            }
        });
    }
    for (auto& reader : readers) {
        reader.join();
    }
    EXPECT_EQ(failures.load(), 0);
}
} // namespace
