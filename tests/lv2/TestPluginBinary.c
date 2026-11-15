/*
 * Trivial LV2 plugin binary used as the lv2:binary of the host rescan fixtures.
 *
 * The rescan tests only enumerate plugins, but a bundle without a binary is not
 * a valid bundle, so the fixtures carry this real shared object instead of a
 * placeholder file. It exports the one symbol a host looks up.
 */

#include <lv2/core/lv2.h>

#include <stdint.h>
#include <stdlib.h>

static LV2_Handle instantiate(const LV2_Descriptor*     descriptor,
                              double                    rate,
                              const char*               bundle_path,
                              const LV2_Feature* const* features)
{
    (void)descriptor;
    (void)rate;
    (void)bundle_path;
    (void)features;
    return (LV2_Handle)malloc(1);
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    (void)instance;
    (void)port;
    (void)data;
}

static void activate(LV2_Handle instance) { (void)instance; }

static void run(LV2_Handle instance, uint32_t sample_count)
{
    (void)instance;
    (void)sample_count;
}

static void deactivate(LV2_Handle instance) { (void)instance; }

static void cleanup(LV2_Handle instance) { free(instance); }

static const LV2_Descriptor descriptor = {
    "http://guitarrackcraft.org/test/plugin-bin",
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    nullptr,
    nullptr,
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
