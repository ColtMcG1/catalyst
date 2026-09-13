/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file test_loader.cpp
 * @brief The `loader<T>` seam: `load<T>` over the vfs and the registry, the reference a caller ends
 * up owning, and a loader that loads.
 * @details Two asset types are defined here purely to be loaded. That is the point of the suite:
 * if the seam only worked for `image` it would not be a seam, so most of what is under test is
 * exercised through types this module has never heard of, with `loader<image>` standing in for the
 * real one.
 *
 * The bytes come from `make_memory_source`, so nothing here touches a disk, and the one image used
 * is a DDS -- read by this module's own code -- so the suite runs in a `CATALYST_RESOURCE_STB=OFF`
 * build like the rest of the container work.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/resource/image.hpp>
#include <catalyst/resource/loader.hpp>
#include <catalyst/resource/source.hpp>
#include <catalyst/resource/vfs.hpp>

#include <charconv>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace catalyst::resource;

namespace
{
#include "container_fixtures.inc"
} // namespace

// -----------------------------------------------------------------------------
// Two asset types this module has never heard of
// -----------------------------------------------------------------------------

namespace demo
{
    /** @brief An asset whose whole content is a decimal number. A loader with no options. */
    struct counter
    {
        int value = 0;
    };

    /** @brief An asset that names a texture, to prove a loader may load. */
    struct material
    {
        image_handle albedo;
        bool tinted = false;
    };

    /** @brief Options, to prove a loader may declare them. */
    struct material_options
    {
        bool tinted = false;
    };

    /** @brief How many times `loader<counter>` has actually run, so a cache hit is observable. */
    inline int counter_decodes = 0;

} // namespace demo

namespace catalyst::resource
{

    template <>
    struct loader<demo::counter>
    {
        // No `options` alias: this is the case `no_load_options` exists for.
        static std::expected<demo::counter, error> decode(std::span<const std::byte> bytes, const load_context &,
                                                          const no_load_options &)
        {
            ++demo::counter_decodes;

            const std::string_view text{reinterpret_cast<const char *>(bytes.data()), bytes.size()};

            demo::counter out;
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out.value);
            if (ec != std::errc{})
                return std::unexpected(make_error(error_code::decode_failed, {}, "not a decimal number"));

            return out;
        }
    };

    template <>
    struct loader<demo::material>
    {
        using options = demo::material_options;

        static std::expected<demo::material, error> decode(std::span<const std::byte> bytes,
                                                           const load_context &context, const options &opt)
        {
            // The asset's content is the name of its texture, written relative to itself.
            const std::string_view reference{reinterpret_cast<const char *>(bytes.data()), bytes.size()};

            auto texture_name = context.resolve(reference);
            if (!texture_name)
                return std::unexpected(std::move(texture_name.error()));

            // Unqualified, and it resolves to the namespace-scope pipeline rather than to this
            // struct's own member -- which is exactly why that member is `decode`.
            auto albedo = load<image>(context.files, context.assets, *texture_name);
            if (!albedo)
                return std::unexpected(std::move(albedo.error()));

            return demo::material{*albedo, opt.tinted};
        }
    };

} // namespace catalyst::resource

namespace
{
    std::span<const std::byte> text_bytes(std::string_view text)
    {
        return std::as_bytes(std::span<const char>{text.data(), text.size()});
    }

    template <std::size_t N>
    std::span<const std::byte> fixture_bytes(const std::uint8_t (&data)[N])
    {
        return std::as_bytes(std::span<const std::uint8_t, N>{data});
    }

    /** @brief A vfs with one memory mount holding everything the suite loads. */
    vfs make_files()
    {
        vfs files;
        auto mounted = files.mount({.scheme = "asset"},
                                   make_memory_source({
                                       {"textures/stone.dds", fixture_bytes(dds_bc7_8x8)},
                                       {"textures/moss.dds", fixture_bytes(dds_dxt1_4x4)},
                                       {"materials/wall.mat", text_bytes("../textures/stone.dds")},
                                       {"materials/floor.mat", text_bytes("../textures/stone.dds")},
                                       {"materials/broken.mat", text_bytes("../textures/absent.dds")},
                                       {"counts/seven", text_bytes("7")},
                                       {"counts/bad", text_bytes("not a number")},
                                   }));
        CT_REQUIRE(mounted.has_value());
        return files;
    }

    uri name_of(const vfs &files, std::string_view reference)
    {
        auto resolved = files.resolve(reference);
        CT_REQUIRE(resolved.has_value());
        return std::move(*resolved);
    }

    // -----------------------------------------------------------------------------
    // The four steps load<T> replaces
    // -----------------------------------------------------------------------------

    void test_load_reads_decodes_and_registers()
    {
        vfs files = make_files();
        store assets;

        auto handle = load<image>(files, assets, "asset:/textures/stone.dds");
        CT_REQUIRE(handle.has_value());
        CT_REQUIRE(static_cast<bool>(*handle));

        registry<image> &images = assets.registry_for<image>();
        const image *img = images.get(*handle);
        CT_REQUIRE(img != nullptr);
        CT_REQUIRE(img->pixel_format() == format::bc7_unorm);
        CT_REQUIRE(img->mip_levels() == 4);

        // Registered under the canonical name, which is what makes the second call a cache hit.
        const auto info = images.info(*handle);
        CT_REQUIRE(info.has_value());
        CT_REQUIRE(info->references == 1);
        CT_REQUIRE(info->key != nullptr);
        CT_REQUIRE(info->key->view() == "asset:/textures/stone.dds");
    }

    void test_options_reach_the_loader()
    {
        vfs files = make_files();
        store assets;

        auto plain = load<image>(files, assets, "asset:/textures/stone.dds");
        CT_REQUIRE(plain.has_value());
        CT_REQUIRE(assets.registry_for<image>().get(*plain)->pixel_format() == format::bc7_unorm);

        // A different name, so this is a real load rather than a cache hit, and the sRGB flag has
        // somewhere to show up.
        store other;
        auto srgb = load<image>(files, other, "asset:/textures/stone.dds", {.srgb = true});
        CT_REQUIRE(srgb.has_value());
        CT_REQUIRE(other.registry_for<image>().get(*srgb)->pixel_format() == format::bc7_unorm_srgb);
    }

    void test_second_load_is_a_cache_hit_that_still_retains()
    {
        vfs files = make_files();
        store assets;
        demo::counter_decodes = 0;

        auto first = load<demo::counter>(files, assets, "asset:/counts/seven");
        CT_REQUIRE(first.has_value());
        CT_REQUIRE(demo::counter_decodes == 1);

        auto second = load<demo::counter>(files, assets, "asset:/counts/seven");
        CT_REQUIRE(second.has_value());
        CT_REQUIRE(*second == *first);

        // The loader did not run again: one copy per name is the registry's job and load<T> asks it
        // before it reads a byte.
        CT_REQUIRE(demo::counter_decodes == 1);

        registry<demo::counter> &counters = assets.registry_for<demo::counter>();
        CT_REQUIRE(counters.size() == 1);
        CT_REQUIRE(counters.get(*first)->value == 7);

        // The contract that makes this usable: both calls handed back a reference, so the count is
        // two and a caller that releases once has not destroyed something another owner is holding.
        CT_REQUIRE(counters.info(*first)->references == 2);

        CT_REQUIRE(!counters.release(*second)); // not the last reference
        CT_REQUIRE(counters.is_valid(*first));
        CT_REQUIRE(counters.release(*first)); // now it is
        CT_REQUIRE(counters.empty());
    }

    void test_relative_names_resolve_through_the_vfs()
    {
        vfs files = make_files();
        store assets;

        files.set_base(name_of(files, "asset:/materials/wall.mat"));

        // Resolved against vfs::base, then normalized -- so this is the same cache entry as the
        // absolute spelling and not a second copy of an 8x8 BC7 texture.
        auto relative = load<image>(files, assets, "../textures/stone.dds");
        CT_REQUIRE(relative.has_value());

        auto absolute = load<image>(files, assets, "asset:/textures/stone.dds");
        CT_REQUIRE(absolute.has_value());
        CT_REQUIRE(*relative == *absolute);
        CT_REQUIRE(assets.registry_for<image>().size() == 1);
    }

    // -----------------------------------------------------------------------------
    // A loader that loads
    // -----------------------------------------------------------------------------

    void test_a_loader_may_load_its_dependencies()
    {
        vfs files = make_files();
        store assets;

        auto wall = load<demo::material>(files, assets, "asset:/materials/wall.mat", {.tinted = true});
        CT_REQUIRE(wall.has_value());

        registry<demo::material> &materials = assets.registry_for<demo::material>();
        registry<image> &images = assets.registry_for<image>();

        const demo::material *mat = materials.get(*wall);
        CT_REQUIRE(mat != nullptr);
        CT_REQUIRE(mat->tinted); // the options reached a loader that declared its own type
        CT_REQUIRE(images.is_valid(mat->albedo));
        CT_REQUIRE(images.get(mat->albedo)->pixel_format() == format::bc7_unorm);

        // Copy the handle out before loading anything else.  hands back a pointer
        // that the next insertion may invalidate, and the handle is the reference that survives --
        // which is the whole argument for the registry handing out handles rather than pointers.
        const image_handle albedo = mat->albedo;

        // A second material naming the same texture shares it rather than decoding it again -- the
        // whole reason a loader calls back into load<T> instead of decoding for itself.
        auto floor = load<demo::material>(files, assets, "asset:/materials/floor.mat");
        CT_REQUIRE(floor.has_value());
        CT_REQUIRE(materials.get(*floor)->albedo == albedo);
        CT_REQUIRE(images.size() == 1);

        // Two materials hold a reference each, so the texture survives either one going away.
        CT_REQUIRE(images.info(albedo)->references == 2);
    }

    void test_a_dependency_is_resolved_against_its_parent()
    {
        // `"../textures/stone.dds"` inside `asset:/materials/wall.mat` must mean
        // `asset:/textures/stone.dds` regardless of what the vfs's base happens to be -- which is
        // the bug that hides until two assets in different directories use one relative path.
        vfs files = make_files();
        store assets;

        files.set_base(name_of(files, "asset:/counts/seven")); // deliberately somewhere else

        auto wall = load<demo::material>(files, assets, "asset:/materials/wall.mat");
        CT_REQUIRE(wall.has_value());

        const auto info = assets.registry_for<image>().info(assets.registry_for<demo::material>().get(*wall)->albedo);
        CT_REQUIRE(info.has_value());
        CT_REQUIRE(info->key->view() == "asset:/textures/stone.dds");
    }

    // -----------------------------------------------------------------------------
    // Failures, and what they say
    // -----------------------------------------------------------------------------

    void test_failures_name_the_asset()
    {
        vfs files = make_files();
        store assets;

        // Nothing mounted on that scheme: the build is configured wrong, not the content.
        auto no_mount = load<image>(files, assets, "other:/textures/stone.dds");
        CT_REQUIRE(!no_mount.has_value());
        CT_REQUIRE(no_mount.error().code == error_code::no_such_mount);
        CT_REQUIRE(no_mount.error().uri == "other:/textures/stone.dds");

        // Mounted, but no such entry: a content author typo'd a path. Distinguishing these two is
        // why error_code has both.
        auto missing = load<image>(files, assets, "asset:/textures/absent.dds");
        CT_REQUIRE(!missing.has_value());
        CT_REQUIRE(missing.error().code == error_code::not_found);
        CT_REQUIRE(missing.error().uri == "asset:/textures/absent.dds");

        // A decode failure inside the loader is named too, so a log line says which asset.
        auto bad = load<demo::counter>(files, assets, "asset:/counts/bad");
        CT_REQUIRE(!bad.has_value());
        CT_REQUIRE(bad.error().code == error_code::decode_failed);
        CT_REQUIRE(bad.error().uri == "asset:/counts/bad");
        CT_REQUIRE(bad.error().detail == "not a decimal number");

        // Nothing was registered by any of them.
        CT_REQUIRE(assets.registry_for<image>().empty());
        CT_REQUIRE(assets.registry_for<demo::counter>().empty());
    }

    void test_a_dependency_failure_keeps_its_own_name()
    {
        // The material exists and the texture it names does not. The failure that surfaces must
        // name the texture, because that is the file somebody has to go and fix -- not the material
        // the load happened to start from.
        vfs files = make_files();
        store assets;

        auto broken = load<demo::material>(files, assets, "asset:/materials/broken.mat");
        CT_REQUIRE(!broken.has_value());
        CT_REQUIRE(broken.error().code == error_code::not_found);
        CT_REQUIRE(broken.error().uri == "asset:/textures/absent.dds");

        // ...and a half-loaded material is not left in the registry.
        CT_REQUIRE(assets.registry_for<demo::material>().empty());
    }

    void test_an_unparseable_name_never_reaches_a_source()
    {
        vfs files = make_files();
        store assets;

        auto bad = load<image>(files, assets, "://not a uri");
        CT_REQUIRE(!bad.has_value());
        CT_REQUIRE(bad.error().code == error_code::invalid_uri);
    }

    // -----------------------------------------------------------------------------
    // The async shape
    // -----------------------------------------------------------------------------

    void test_load_async_matches_load()
    {
        vfs files = make_files();
        store assets;
        demo::counter_decodes = 0;

        auto task = load_async<demo::counter>(files, assets, name_of(files, "asset:/counts/seven"));
        auto handle = task.get();

        CT_REQUIRE(handle.has_value());
        CT_REQUIRE(demo::counter_decodes == 1);
        CT_REQUIRE(assets.registry_for<demo::counter>().get(*handle)->value == 7);

        // Tier 1 completes it on the calling thread, so the only thing that differs from `load` is
        // the shape -- which is the point: this caller does not change when Tier 3 puts a worker
        // pool behind vfs::read_async.
        auto again = load_async<demo::counter>(files, assets, name_of(files, "asset:/counts/seven")).get();
        CT_REQUIRE(again.has_value());
        CT_REQUIRE(*again == *handle);
        CT_REQUIRE(demo::counter_decodes == 1);
        CT_REQUIRE(assets.registry_for<demo::counter>().info(*handle)->references == 2);

        auto missing = load_async<demo::counter>(files, assets, name_of(files, "asset:/counts/absent")).get();
        CT_REQUIRE(!missing.has_value());
        CT_REQUIRE(missing.error().code == error_code::not_found);
        CT_REQUIRE(missing.error().uri == "asset:/counts/absent");
    }

    // -----------------------------------------------------------------------------
    // The seam itself
    // -----------------------------------------------------------------------------

    struct not_an_asset
    {
    };

    void test_the_concept_describes_the_seam()
    {
        // A type with a loader satisfies it; one without does not, and asking for it is a compile
        // error naming the type at the call site rather than a run-time "no loader registered".
        static_assert(loadable<image>);
        static_assert(loadable<demo::counter>);
        static_assert(loadable<demo::material>);
        static_assert(!loadable<not_an_asset>);

        // A loader that declares no options gets the empty struct, so `load<T>(...)` needs no third
        // argument; one that declares them gets its own type.
        static_assert(std::is_same_v<load_options_t<demo::counter>, no_load_options>);
        static_assert(std::is_same_v<load_options_t<demo::material>, demo::material_options>);
        static_assert(std::is_same_v<load_options_t<image>, image_decode_options>);
    }

} // namespace

int main()
{
    test_load_reads_decodes_and_registers();
    test_options_reach_the_loader();
    test_second_load_is_a_cache_hit_that_still_retains();
    test_relative_names_resolve_through_the_vfs();

    test_a_loader_may_load_its_dependencies();
    test_a_dependency_is_resolved_against_its_parent();

    test_failures_name_the_asset();
    test_a_dependency_failure_keeps_its_own_name();
    test_an_unparseable_name_never_reaches_a_source();

    test_load_async_matches_load();
    test_the_concept_describes_the_seam();

    return 0;
}
