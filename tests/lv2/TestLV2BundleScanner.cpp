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
 * Incremental LV2 rescan against a real lilv world.
 *
 * The app imports user bundles into <filesDir>/lv2_user while the engine is
 * running. The LilvWorld cannot be rebuilt at that point (live LV2Plugins keep
 * pointers into it), so LV2PluginFactory::refresh() must load only the bundles
 * that appeared since the last scan. These tests pin that behaviour against
 * the vendored lilv, not against a stub: bundle discovery, loading into an
 * existing world, plugin URI identity across rescans and idempotency.
 */

#include "plugin/lv2/LV2BundleScanner.h"

#include <gtest/gtest.h>

#include <lilv/lilv.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef GRC_LV2_TEST_PLUGIN
#error "GRC_LV2_TEST_PLUGIN must point at the compiled fixture plugin binary"
#endif

namespace {

namespace fs = std::filesystem;

using guitarrackcraft::LV2BundleScanner;

constexpr const char* kPluginOne = "http://example.org/plugin1";
constexpr const char* kPluginTwo = "http://example.org/plugin2";

class LV2BundleScannerTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("grc-lv2-rescan-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                 "-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root_);
        world_ = lilv_world_new();
        ASSERT_NE(world_, nullptr);
    }

    void TearDown() override {
        if (world_) {
            lilv_world_free(world_);
        }
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    /** Create <root>/<relativeDir>/ with a manifest for [pluginUri] plus the
     *  fixture binary, the minimum a loadable bundle needs. */
    void createBundle(const std::string& relativeDir, const std::string& pluginUri,
                      const std::string& binaryName = "plugin.so") const {
        const fs::path bundle = root_ / relativeDir;
        fs::create_directories(bundle);

        std::ofstream manifest(bundle / "manifest.ttl");
        manifest << "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
                 << "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                 << "\n"
                 << "<" << pluginUri << ">\n"
                 << "    a lv2:Plugin ;\n"
                 << "    lv2:binary <" << binaryName << "> ;\n"
                 << "    rdfs:seeAlso <plugin.ttl> .\n";
        manifest.close();

        std::ofstream pluginTtl(bundle / "plugin.ttl");
        pluginTtl << "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
                  << "@prefix doap: <http://usefulinc.com/ns/doap#> .\n"
                  << "\n"
                  << "<" << pluginUri << ">\n"
                  << "    doap:name \"" << relativeDir << "\" .\n";
        pluginTtl.close();

        fs::copy_file(GRC_LV2_TEST_PLUGIN, bundle / binaryName,
                      fs::copy_options::overwrite_existing);
    }

    std::string rootString() const { return root_.string(); }

    fs::path root_;
    LilvWorld* world_ = nullptr;
    LV2BundleScanner scanner_;
};

TEST_F(LV2BundleScannerTest, LoadsNothingWhenRootDoesNotExist) {
    EXPECT_EQ(scanner_.scanRoot(world_, (root_ / "absent").string()), 0);
    EXPECT_TRUE(LV2BundleScanner::pluginUris(world_).empty());
    EXPECT_TRUE(scanner_.loadedBundles().empty());
}

TEST_F(LV2BundleScannerTest, IgnoresDirectoriesWithoutManifest) {
    fs::create_directories(root_ / "NotABundle.lv2");
    fs::create_directories(root_ / "group" / "nested");

    EXPECT_EQ(scanner_.scanRoot(world_, rootString()), 0);
    EXPECT_TRUE(LV2BundleScanner::pluginUris(world_).empty());
}

// An import copies into `lv2_user/.staging-<uuid>/` and only renames the tree to
// its final name when validation passes, so the scanner routinely walks past a
// half-copied bundle sitting in a dot directory. Loading it would put a plugin
// whose files are still being written into the world (and keep it there after
// the importer rolls the directory back), so every dot entry is skipped, not
// just "." and "..".
TEST_F(LV2BundleScannerTest, SkipsStagingDirectoriesOfInFlightImports) {
    createBundle(".staging-3f6c8a1e/Thing.lv2", kPluginOne);
    createBundle(".staging-3f6c8a1e-wrapped/Wrapped.lv2", kPluginTwo);

    EXPECT_TRUE(scanner_.findNewBundles(rootString()).empty())
        << "a bundle under a dot directory is an in-flight import, not a plugin";
    EXPECT_EQ(scanner_.scanRoot(world_, rootString()), 0);

    const auto uris = LV2BundleScanner::pluginUris(world_);
    EXPECT_TRUE(uris.empty()) << "scanner loaded " << uris.size() << " plugin(s) from a .staging dir";
    EXPECT_TRUE(scanner_.loadedBundles().empty());

    fs::rename(root_ / ".staging-3f6c8a1e", root_ / "Committed.lv2");
    EXPECT_EQ(scanner_.scanRoot(world_, rootString()), 1)
        << "the import's rename into place must make the bundle visible";
    EXPECT_EQ(LV2BundleScanner::pluginUris(world_),
              std::vector<std::string>{kPluginOne});
}

TEST_F(LV2BundleScannerTest, LoadsFirstBundleAndEnumeratesItsPlugin) {
    createBundle("First.lv2", kPluginOne);

    ASSERT_EQ(scanner_.scanRoot(world_, rootString()), 1);

    const auto uris = LV2BundleScanner::pluginUris(world_);
    ASSERT_EQ(uris.size(), 1u);
    EXPECT_EQ(uris.front(), kPluginOne);
    ASSERT_EQ(scanner_.loadedBundles().size(), 1u);
    EXPECT_NE(scanner_.loadedBundles().count((root_ / "First.lv2").string()), 0u);
}

TEST_F(LV2BundleScannerTest, RescanWithoutChangesLoadsNothingNew) {
    createBundle("First.lv2", kPluginOne);
    ASSERT_EQ(scanner_.scanRoot(world_, rootString()), 1);

    const std::vector<std::string> before = LV2BundleScanner::pluginUris(world_);

    EXPECT_EQ(scanner_.scanRoot(world_, rootString()), 0);
    EXPECT_EQ(scanner_.scanRoot(world_, rootString()), 0);

    EXPECT_EQ(LV2BundleScanner::pluginUris(world_), before);
}

TEST_F(LV2BundleScannerTest, RescanAddsSecondBundleAndKeepsTheFirstPluginAlive) {
    createBundle("First.lv2", kPluginOne);
    ASSERT_EQ(scanner_.scanRoot(world_, rootString()), 1);

    LilvNode* firstUri = lilv_new_uri(world_, kPluginOne);
    const LilvPlugin* firstBefore =
        lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world_), firstUri);
    ASSERT_NE(firstBefore, nullptr);

    createBundle("Second.lv2", kPluginTwo);
    ASSERT_EQ(scanner_.scanRoot(world_, rootString()), 1);

    const LilvPlugins* plugins = lilv_world_get_all_plugins(world_);
    const LilvPlugin* firstAfter = lilv_plugins_get_by_uri(plugins, firstUri);
    EXPECT_EQ(firstBefore, firstAfter);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(firstBefore),
              reinterpret_cast<uintptr_t>(firstAfter));

    LilvNode* secondUri = lilv_new_uri(world_, kPluginTwo);
    EXPECT_NE(lilv_plugins_get_by_uri(plugins, secondUri), nullptr);

    const auto uris = LV2BundleScanner::pluginUris(world_);
    ASSERT_EQ(uris.size(), 2u);
    EXPECT_EQ(uris[0], kPluginOne);
    EXPECT_EQ(uris[1], kPluginTwo);

    lilv_node_free(secondUri);
    lilv_node_free(firstUri);
}

TEST_F(LV2BundleScannerTest, ScansRecursivelyIntoGroupingDirectories) {
    createBundle("Group.lv2/nested/Deep.lv2", kPluginOne);

    ASSERT_EQ(scanner_.scanRoot(world_, rootString()), 1);
    EXPECT_EQ(LV2BundleScanner::pluginUris(world_),
              std::vector<std::string>{kPluginOne});
}

TEST_F(LV2BundleScannerTest, ReportsEachPluginUriOnlyOnce) {
    createBundle("Original.lv2", kPluginOne);
    ASSERT_EQ(scanner_.scanRoot(world_, rootString()), 1);

    createBundle("Copy.lv2", kPluginOne);
    EXPECT_EQ(scanner_.scanRoot(world_, rootString()), 1);

    const auto uris = LV2BundleScanner::pluginUris(world_);
    ASSERT_EQ(uris.size(), 1u);
    EXPECT_EQ(uris.front(), kPluginOne);

    LilvNode* uri = lilv_new_uri(world_, kPluginOne);
    const LilvPlugin* plugin =
        lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world_), uri);
    ASSERT_NE(plugin, nullptr);
    const std::string winner = lilv_node_as_string(lilv_plugin_get_bundle_uri(plugin));
    EXPECT_NE(winner.find("Original.lv2"), std::string::npos)
        << "first loaded bundle must win, got " << winner;
    lilv_node_free(uri);
}

TEST_F(LV2BundleScannerTest, FindNewBundlesSkipsWhatWasAlreadyLoaded) {
    createBundle("First.lv2", kPluginOne);
    const std::string first = (root_ / "First.lv2").string();

    EXPECT_EQ(scanner_.findNewBundles(rootString()), std::vector<std::string>{first});

    ASSERT_EQ(scanner_.scanRoot(world_, rootString()), 1);
    EXPECT_TRUE(scanner_.findNewBundles(rootString()).empty())
        << "a bundle already in the world must not be offered again";

    createBundle("Second.lv2", kPluginTwo);
    EXPECT_EQ(scanner_.findNewBundles(rootString()),
              std::vector<std::string>{(root_ / "Second.lv2").string()});
}

TEST_F(LV2BundleScannerTest, LoadBundlesSkipsAlreadyLoadedDirectories) {
    createBundle("First.lv2", kPluginOne);
    const std::vector<std::string> bundleDirs = {(root_ / "First.lv2").string()};

    ASSERT_EQ(scanner_.loadBundles(world_, bundleDirs), 1);
    EXPECT_EQ(scanner_.loadBundles(world_, bundleDirs), 0);
    EXPECT_EQ(LV2BundleScanner::pluginUris(world_).size(), 1u);
}

TEST_F(LV2BundleScannerTest, ResolvesPluginBinaryFromBundle) {
    createBundle("First.lv2", kPluginOne, "plugin1.so");
    ASSERT_EQ(scanner_.scanRoot(world_, rootString()), 1);

    LilvNode* uri = lilv_new_uri(world_, kPluginOne);
    const LilvPlugin* plugin =
        lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world_), uri);
    ASSERT_NE(plugin, nullptr);

    const LilvNode* libraryUri = lilv_plugin_get_library_uri(plugin);
    ASSERT_NE(libraryUri, nullptr);
    // serd_uri_to_path hands back borrowed storage; only the node is ours.
    const char* libraryPath = lilv_uri_to_path(lilv_node_as_uri(libraryUri));
    ASSERT_NE(libraryPath, nullptr);
    const fs::path binary = fs::path(libraryPath);
    EXPECT_TRUE(fs::exists(binary)) << binary;
    EXPECT_EQ(binary.filename().string(), "plugin1.so");
    lilv_node_free(uri);
}

} // namespace
