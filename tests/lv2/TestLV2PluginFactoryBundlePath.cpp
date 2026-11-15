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
 * The plugin -> bundle mapping the LV2 import relies on, end to end.
 *
 * `Lv2BundleAttribution` (Kotlin) decides which imported bundle a plugin belongs
 * to from `PluginInfo.bundlePath`, and the browser hides a plugin when its bundle
 * is removed. The path is therefore load-bearing for the whole "Manage imported"
 * flow, but it travels through the whole native chain (lilv bundle URI ->
 * `pluginBundlePath` -> `PluginInfo` -> JNI -> `PluginInfo.kt`) with no assertion
 * on it anywhere: if the factory stopped filling it in, `bundlePath` would stay
 * empty, every imported plugin would silently lose its attribution and nothing
 * would fail.
 *
 * So this test runs the real factory: it is constructed with an explicit scan
 * root (no assets, no AAssetManager), initializes against the vendored lilv and
 * is asked to enumerate a fixture bundle. What comes back is what the JNI layer
 * marshals. The Android headers the factory includes are stubbed in
 * tests/lv2/stubs; nothing else about the translation unit changes.
 */

#include "plugin/lv2/LV2PluginFactory.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef GRC_LV2_TEST_PLUGIN
#error "GRC_LV2_TEST_PLUGIN must point at the compiled fixture plugin binary"
#endif

namespace {

namespace fs = std::filesystem;

using guitarrackcraft::LV2PluginFactory;
using guitarrackcraft::PluginInfo;

constexpr const char* kPluginOne = "http://example.org/plugin1";
constexpr const char* kPluginTwo = "http://example.org/plugin2";

class LV2PluginFactoryBundlePathTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("grc-lv2-bundlepath-" +
                 std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    void createBundle(const std::string& relativeDir, const std::string& pluginUri,
                      const std::string& binaryName = "plugin.so") const {
        const fs::path bundle = root_ / relativeDir;
        fs::create_directories(bundle);

        std::ofstream manifest(bundle / "manifest.ttl");
        manifest << "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
                 << "@prefix doap: <http://usefulinc.com/ns/doap#> .\n"
                 << "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                 << "\n"
                 << "<" << pluginUri << ">\n"
                 << "    a lv2:Plugin ;\n"
                 << "    lv2:binary <" << binaryName << "> ;\n"
                 << "    doap:name \"" << pluginUri << "\" ;\n"
                 << "    rdfs:seeAlso <plugin.ttl> .\n";
        manifest.close();

        std::ofstream pluginTtl(bundle / "plugin.ttl");
        pluginTtl << "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
                  << "\n"
                  << "<" << pluginUri << ">\n"
                  << "    lv2:port [\n"
                  << "        a lv2:AudioPort , lv2:InputPort ;\n"
                  << "        lv2:index 0 ;\n"
                  << "        lv2:symbol \"in\" ;\n"
                  << "        lv2:name \"in\"\n"
                  << "    ] .\n";
        pluginTtl.close();

        fs::copy_file(GRC_LV2_TEST_PLUGIN, bundle / binaryName,
                      fs::copy_options::overwrite_existing);
    }

    /** What a fresh factory initialized on [root_] reports, i.e. the list the
     *  JNI layer marshals to `PluginInfo.kt`. */
    std::vector<PluginInfo> enumerate() {
        LV2PluginFactory factory(root_.string());
        EXPECT_TRUE(factory.initialize());
        return factory.enumeratePlugins();
    }

    fs::path root_;
};

TEST_F(LV2PluginFactoryBundlePathTest, ReportsTheBundleDirectoryEachPluginCameFrom) {
    createBundle("First.lv2", kPluginOne);

    const std::vector<PluginInfo> plugins = enumerate();
    ASSERT_EQ(plugins.size(), 1u);
    EXPECT_EQ(plugins.front().id, kPluginOne);
    EXPECT_EQ(plugins.front().bundlePath, (root_ / "First.lv2").string())
        << "bundlePath is what Lv2BundleAttribution maps the plugin back to";
}

TEST_F(LV2PluginFactoryBundlePathTest, BundlePathHasNoTrailingSeparator) {
    createBundle("First.lv2", kPluginOne);

    const std::vector<PluginInfo> plugins = enumerate();
    ASSERT_EQ(plugins.size(), 1u);
    const std::string& bundlePath = plugins.front().bundlePath;
    ASSERT_FALSE(bundlePath.empty());
    EXPECT_NE(bundlePath.back(), '/')
        << "attribution composes paths from it; a trailing slash produces '//'";
}

TEST_F(LV2PluginFactoryBundlePathTest, BundlePathMapsEveryPluginOfAGroupToItsOwnBundle) {
    createBundle("Group.lv2/One.lv2", kPluginOne);
    createBundle("Group.lv2/Two.lv2", kPluginTwo);

    const std::vector<PluginInfo> plugins = enumerate();
    ASSERT_EQ(plugins.size(), 2u);

    for (const PluginInfo& plugin : plugins) {
        const std::string expected =
            plugin.id == kPluginOne ? (root_ / "Group.lv2" / "One.lv2").string()
                                    : (root_ / "Group.lv2" / "Two.lv2").string();
        EXPECT_EQ(plugin.bundlePath, expected) << plugin.id;
    }
}

TEST_F(LV2PluginFactoryBundlePathTest, BundlePathIsSetForAnImportedBundlesDirectory) {
    const fs::path userDir = root_ / "lv2_user";
    fs::create_directories(userDir);
    createBundle("lv2_user/9f1c-imported/Bundled.lv2", kPluginOne);

    const std::vector<PluginInfo> plugins = enumerate();
    ASSERT_EQ(plugins.size(), 1u);
    const std::string expected = (userDir / "9f1c-imported" / "Bundled.lv2").string();
    EXPECT_EQ(plugins.front().bundlePath, expected);
    EXPECT_EQ(plugins.front().bundlePath.rfind(userDir.string() + "/", 0), 0u)
        << "an imported plugin must look like it lives under lv2_user";
}

TEST_F(LV2PluginFactoryBundlePathTest, RefreshKeepsBundlePathsForExistingAndNewPlugins) {
    createBundle("First.lv2", kPluginOne);
    LV2PluginFactory factory(root_.string());
    ASSERT_TRUE(factory.initialize());
    ASSERT_EQ(factory.enumeratePlugins().size(), 1u);

    createBundle("Second.lv2", kPluginTwo);
    ASSERT_TRUE(factory.refresh());

    const std::vector<PluginInfo> plugins = factory.enumeratePlugins();
    ASSERT_EQ(plugins.size(), 2u);
    for (const PluginInfo& plugin : plugins) {
        const std::string expected = plugin.id == kPluginOne
                                         ? (root_ / "First.lv2").string()
                                         : (root_ / "Second.lv2").string();
        EXPECT_EQ(plugin.bundlePath, expected) << plugin.id
                                               << " lost its bundle path across a refresh";
    }
}

} // namespace
