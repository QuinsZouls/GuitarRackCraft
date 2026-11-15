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

#ifndef GUITARRACKCRAFT_LV2_WORLD_LOCK_H
#define GUITARRACKCRAFT_LV2_WORLD_LOCK_H

#include <mutex>

namespace guitarrackcraft {
namespace lv2world {

/**
 * Process-wide lock for the shared LilvWorld.
 *
 * Importing a bundle (`lilv_world_load_bundle`, `lilv_world_load_plugin_classes`)
 * mutates the world's model and the intern tables behind it while plugin
 * creation, `IPlugin::getInfo()` and `IPlugin::activate()` read them.
 *
 * There is no read side to this lock, because in lilv and sord there is no such
 * thing as a read-only call:
 *
 *   - `lilv_new_uri()` (used by getInfo for every port class, and by
 *     instantiate for rdf:type) interns the URI in the sord world
 *     (lilv/src/node.c:66 → `sord_new_uri`), which inserts into a hash table.
 *   - `lilv_plugin_instantiate()` runs `lilv_plugin_load_if_necessary()` and
 *     `lilv_lib_open()`, which inserts the .so handle into `world->libs` and
 *     bumps its refcount (lilv/src/lib.c:48-52).
 *   - `lilv_world_load_bundle()` inserts statements into `world->model`.
 *
 * So a `std::shared_mutex` with shared readers would still leave two
 * concurrent plugin creations racing on the same intern trees. Every user of
 * the world serializes on this one mutex; the callers are UI/user driven
 * (import, add plugin, engine start/stop, preset save, plugin UI creation), the
 * audio callback never touches the world, and the critical sections last
 * milliseconds.
 *
 * Lock order (strict, outermost first):
 *
 *   1. lv2world::lock() — this one.
 *   2. PluginRegistry cache mutex — always taken *after* the world lock
 *      (PluginRegistry::initializeAll/refreshAll take the world lock and then
 *      their own; the readers take the world lock in JNI and then reach the
 *      cache through PluginRegistry::createPlugin).
 *   3. PluginChain chain mutex — taken after the world lock by the JNI paths
 *      that activate plugins (addPlugin/setSampleRate).
 *
 * Nothing reverses that order, so these locks cannot deadlock against each
 * other (ABBA).
 *
 * Two more rules, because the mutex is not recursive:
 *
 *   - A thread must never take it twice. Nesting a second acquisition in a path
 *     that already holds it deadlocks outright.
 *   - An entry point must not hold it while waiting for another thread that will
 *     take it. `PluginUIManager::createPluginUI()` posts plugin-UI instantiation
 *     to the display's pluginUI thread and waits for it, so the lock lives in
 *     each of the two bodies (the `getInfo()` there and in `LV2PluginUI`),
 *     never around the hand-off.
 *
 * The audio callback is out of scope and must stay out: it only touches IPlugin
 * instances already in the chain (`lilv_instance_run`), which never reads or
 * writes the world. No lock of this file may ever be taken from
 * IPlugin::process() or from anything it waits on.
 */
inline std::mutex& lock() {
    static std::mutex instance;
    return instance;
}

/** RAII guard: `lv2world::Guard guard = lv2world::guard();`
 *  A unique_lock (not a lock_guard) so a caller that has to shorten its critical
 *  section — release before waiting for another thread — can `unlock()` it
 *  explicitly instead of restructuring the blocks. */
using Guard = std::unique_lock<std::mutex>;

inline Guard guard() { return Guard(lock()); }

} // namespace lv2world
} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_LV2_WORLD_LOCK_H
