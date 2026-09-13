/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file test_assets.cpp
 * @brief Tier 1 of the asset system: blobs, sources, the mount table, and the registry's
 * generation-checked handles.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/resource/blob.hpp>
#include <catalyst/resource/image.hpp>
#include <catalyst/resource/registry.hpp>
#include <catalyst/resource/source.hpp>
#include <catalyst/resource/vfs.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

using namespace catalyst::resource;
namespace rendering = catalyst::rendering;

namespace
{
    std::vector<std::byte> bytes_of(std::string_view text)
    {
        std::vector<std::byte> out(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
            out[i] = static_cast<std::byte>(text[i]);
        return out;
    }

    uri parse(std::string_view text)
    {
        auto u = uri::parse(text);
        CT_REQUIRE(u.has_value());
        return std::move(*u);
    }

    // A tiny asset type. The registry needs nothing of it beyond move + destroy.
    struct fake_mesh
    {
        int vertices = 0;
    };

    // -----------------------------------------------------------------------------
    // handle
    // -----------------------------------------------------------------------------

    void test_handle_packing()
    {
        const resource_id id = make_asset_id(7, 3);
        CT_REQUIRE(asset_index(id) == 7);
        CT_REQUIRE(asset_generation(id) == 3);

        // Slot 0 at generation 1 must not collide with the null handle.
        CT_REQUIRE(make_asset_id(0, 1) != 0);
        CT_REQUIRE(!asset_handle<fake_mesh>{});
        CT_REQUIRE(static_cast<bool>(asset_handle<fake_mesh>{make_asset_id(0, 1)}));

        // The handle type is the renderer's, tagged by the asset type, so kinds do not convert.
        static_assert(std::is_same_v<asset_handle<fake_mesh>, rendering::resource_handle<fake_mesh>>);
        static_assert(!std::is_same_v<asset_handle<fake_mesh>, asset_handle<image>>);
    }

    void test_flag_operators_are_reused()
    {
        const source_caps caps = source_caps::enumerable | source_caps::stattable;
        CT_REQUIRE(has_flag(caps, source_caps::enumerable));
        CT_REQUIRE(has_flag(caps, source_caps::stattable));
        CT_REQUIRE(!has_flag(caps, source_caps::mutable_entries));
        CT_REQUIRE(has_any(caps, source_caps::mutable_entries | source_caps::stattable));
    }

    // -----------------------------------------------------------------------------
    // blob
    // -----------------------------------------------------------------------------

    void test_blob_adopt_and_borrow()
    {
        blob owned = blob::adopt(bytes_of("hello"));
        CT_REQUIRE(owned.size() == 5);
        CT_REQUIRE(owned.text() == "hello");

        blob moved = std::move(owned);
        CT_REQUIRE(moved.text() == "hello");
        CT_REQUIRE(owned.empty()); // NOLINT(bugprone-use-after-move) -- moved-from is the empty blob

        auto storage = bytes_of("borrowed");
        blob view = blob::borrow(storage);
        CT_REQUIRE(view.text() == "borrowed");
        view.reset();
        CT_REQUIRE(view.empty());
        CT_REQUIRE(storage.size() == 8); // borrow released nothing

        CT_REQUIRE(blob::adopt({}).empty());
    }

    // -----------------------------------------------------------------------------
    // sources and the mount table
    // -----------------------------------------------------------------------------

    void test_memory_source_and_vfs()
    {
        static const auto hull = bytes_of("hull");
        static const auto stone = bytes_of("stone");

        vfs files;
        auto mounted = files.mount({.scheme = "asset"},
                                   make_memory_source({{"meshes/hull.bin", hull},
                                                       {"textures/stone.png", stone}}));
        CT_REQUIRE(mounted.has_value());
        CT_REQUIRE(files.mounts().size() == 1);

        auto read = files.read("asset:/meshes/hull.bin");
        CT_REQUIRE(read.has_value());
        CT_REQUIRE(read->text() == "hull");

        // Missing entry under a mounted scheme is not_found...
        auto missing = files.read("asset:/meshes/absent.bin");
        CT_REQUIRE(!missing.has_value());
        CT_REQUIRE(missing.error().code == error_code::not_found);

        // ...whereas an unmounted scheme is no_such_mount. These are the two the loaders branch on.
        auto unmounted = files.read("pack:/meshes/hull.bin");
        CT_REQUIRE(!unmounted.has_value());
        CT_REQUIRE(unmounted.error().code == error_code::no_such_mount);

        CT_REQUIRE(files.exists(parse("asset:/textures/stone.png")));
        CT_REQUIRE(!files.exists(parse("asset:/textures/absent.png")));

        auto info = files.stat(parse("asset:/textures/stone.png"));
        CT_REQUIRE(info.has_value());
        CT_REQUIRE(info->size_bytes == 5);

        auto listed = files.list(parse("asset:/"));
        CT_REQUIRE(listed.has_value());
        CT_REQUIRE(listed->size() == 2);
    }

    void test_vfs_resolves_relative_names()
    {
        static const auto shared = bytes_of("shared");

        vfs files;
        CT_REQUIRE(files.mount({.scheme = "asset"}, make_memory_source({{"shared/x.json", shared}}))
                       .has_value());

        // A manifest's relative name only means something against the manifest's own URI.
        files.set_base(parse("asset:/levels/forest/level.json"));

        auto resolved = files.resolve("../../shared/x.json");
        CT_REQUIRE(resolved.has_value());
        CT_REQUIRE(resolved->path() == "/shared/x.json");

        auto read = files.read("../../shared/x.json");
        CT_REQUIRE(read.has_value());
        CT_REQUIRE(read->text() == "shared");

        // Two spellings of one name normalize to one cache key.
        auto dotted = files.resolve("asset:/shared/./x.json");
        CT_REQUIRE(dotted.has_value());
        CT_REQUIRE(*dotted == *resolved);
    }

    void test_more_specific_mount_wins()
    {
        static const auto from_base = bytes_of("base");
        static const auto from_core = bytes_of("core");

        vfs files;
        CT_REQUIRE(files.mount({.scheme = "asset"}, make_memory_source({{"core/a.bin", from_base}}))
                       .has_value());
        CT_REQUIRE(files.mount({.scheme = "asset", .path_prefix = "/core"},
                               make_memory_source({{"core/a.bin", from_core}}))
                       .has_value());

        auto read = files.read("asset:/core/a.bin");
        CT_REQUIRE(read.has_value());
        CT_REQUIRE(read->text() == "core");

        // Whole-segment matching: /core must not claim /core_extra.
        CT_REQUIRE(files.mounts().front().path_prefix == "/core");
        CT_REQUIRE(files.find_source(parse("asset:/core_extra/a.bin")) ==
                   files.find_source(parse("asset:/other/a.bin")));

        CT_REQUIRE(files.unmount({.scheme = "asset", .path_prefix = "/core"}) == 1);
        auto after = files.read("asset:/core/a.bin");
        CT_REQUIRE(after.has_value());
        CT_REQUIRE(after->text() == "base");
    }

    // -----------------------------------------------------------------------------
    // registry
    // -----------------------------------------------------------------------------

    void test_registry_insert_and_resolve()
    {
        registry<fake_mesh> meshes;

        auto handle = meshes.insert(parse("asset:/meshes/hull.bin"), fake_mesh{128});
        CT_REQUIRE(handle.has_value());
        CT_REQUIRE(meshes.size() == 1);

        fake_mesh *mesh = meshes.get(*handle);
        CT_REQUIRE(mesh != nullptr);
        CT_REQUIRE(mesh->vertices == 128);

        // Found by name, and a second insert of that name converges instead of overwriting.
        CT_REQUIRE(meshes.find(parse("asset:/meshes/hull.bin")) == *handle);
        auto again = meshes.insert(parse("asset:/meshes/hull.bin"), fake_mesh{999});
        CT_REQUIRE(again.has_value());
        CT_REQUIRE(*again == *handle);
        CT_REQUIRE(meshes.get(*again)->vertices == 128);
        CT_REQUIRE(meshes.size() == 1);

        // ...and it counted as a reference, so one release does not destroy it.
        CT_REQUIRE(meshes.info(*handle)->references == 2);
        CT_REQUIRE(!meshes.release(*handle));
        CT_REQUIRE(meshes.release(*handle));
        CT_REQUIRE(meshes.size() == 0);
        CT_REQUIRE(meshes.get(*handle) == nullptr);
        CT_REQUIRE(!meshes.find(parse("asset:/meshes/hull.bin")));
    }

    void test_registry_detects_stale_handles()
    {
        registry<fake_mesh> meshes;

        auto first = meshes.insert(parse("asset:/a.bin"), fake_mesh{1});
        CT_REQUIRE(first.has_value());
        CT_REQUIRE(meshes.release(*first));

        // The slot is recycled; the handle to its previous occupant must not resolve to the new one.
        auto second = meshes.insert(parse("asset:/b.bin"), fake_mesh{2});
        CT_REQUIRE(second.has_value());
        CT_REQUIRE(asset_index(*second) == asset_index(*first));
        CT_REQUIRE(asset_generation(*second) != asset_generation(*first));

        CT_REQUIRE(meshes.get(*first) == nullptr);
        CT_REQUIRE(!meshes.is_valid(*first));
        CT_REQUIRE(!meshes.retain(*first));
        CT_REQUIRE(!meshes.release(*first)); // double release is a no-op, not a crash
        CT_REQUIRE(meshes.get(*second)->vertices == 2);

        CT_REQUIRE(!meshes.replace(*first, fake_mesh{3}).has_value());
        CT_REQUIRE(meshes.replace(*first, fake_mesh{3}).error().code == error_code::stale_handle);
    }

    void test_registry_replace_keeps_handles_live()
    {
        registry<fake_mesh> meshes;

        auto handle = meshes.insert(parse("asset:/a.bin"), fake_mesh{1});
        CT_REQUIRE(handle.has_value());
        CT_REQUIRE(meshes.retain(*handle));

        // Hot reload: same slot, same generation, same reference count, new contents.
        CT_REQUIRE(meshes.replace(*handle, fake_mesh{42}).has_value());
        CT_REQUIRE(meshes.get(*handle)->vertices == 42);
        CT_REQUIRE(meshes.info(*handle)->references == 2);
    }

    void test_registry_acquire_and_unnamed()
    {
        registry<fake_mesh> meshes;

        // Nothing loaded under that name yet: the loader's cue to go and read bytes.
        CT_REQUIRE(!meshes.acquire(parse("asset:/a.bin")));

        auto handle = meshes.insert(parse("asset:/a.bin"), fake_mesh{1});
        CT_REQUIRE(handle.has_value());

        auto acquired = meshes.acquire(parse("asset:/a.bin"));
        CT_REQUIRE(acquired == *handle);
        CT_REQUIRE(meshes.info(*handle)->references == 2);

        auto generated = meshes.insert_unnamed(fake_mesh{7});
        CT_REQUIRE(generated.has_value());
        CT_REQUIRE(meshes.get(*generated)->vertices == 7);
        CT_REQUIRE(meshes.info(*generated)->key == nullptr);
        CT_REQUIRE(meshes.size() == 2);

        int seen = 0;
        meshes.for_each([&](auto, fake_mesh &) { ++seen; });
        CT_REQUIRE(seen == 2);

        meshes.clear();
        CT_REQUIRE(meshes.empty());
    }

    void test_store_holds_one_registry_per_type()
    {
        store assets;
        CT_REQUIRE(assets.type_count() == 0);
        CT_REQUIRE(assets.find_registry<fake_mesh>() == nullptr);

        auto handle = assets.registry_for<fake_mesh>().insert(parse("asset:/a.bin"), fake_mesh{5});
        CT_REQUIRE(handle.has_value());
        CT_REQUIRE(assets.type_count() == 1);

        // Same registry back on the second call.
        CT_REQUIRE(assets.registry_for<fake_mesh>().get(*handle)->vertices == 5);

        (void)assets.registry_for<image>();
        CT_REQUIRE(assets.type_count() == 2);
        CT_REQUIRE(assets.find_registry<image>() != nullptr);

        assets.clear();
        CT_REQUIRE(assets.registry_for<fake_mesh>().empty());
    }

    // -----------------------------------------------------------------------------
    // image
    // -----------------------------------------------------------------------------

    void test_image_mip_arithmetic()
    {
        // 4x4 rgba8 with a full mip chain: 64 + 16 + 4 bytes.
        std::vector<std::byte> pixels(64 + 16 + 4);
        const image img{std::move(pixels), rendering::format::rgba8_unorm, rendering::extent3d{4, 4, 1}, 3};

        CT_REQUIRE(img.pixel_format() == rendering::format::rgba8_unorm);
        CT_REQUIRE((img.extent_at(1) == rendering::extent3d{2, 2, 1}));
        CT_REQUIRE((img.extent_at(2) == rendering::extent3d{1, 1, 1}));
        CT_REQUIRE((img.extent_at(9) == rendering::extent3d{1, 1, 1})); // clamps, never zero

        CT_REQUIRE(img.level_size_bytes(0) == 64);
        CT_REQUIRE(img.level_size_bytes(1) == 16);
        CT_REQUIRE(img.level(0).size() == 64);
        CT_REQUIRE(img.level(2).size() == 4);
        CT_REQUIRE(img.level(3).empty()); // out of range
        CT_REQUIRE(img.level(0, 1).empty());
    }

    // -----------------------------------------------------------------------------
    // error
    // -----------------------------------------------------------------------------

    void test_error_message()
    {
        const error e = make_error(error_code::decode_failed, "asset:/a.png", "bad chunk CRC");
        CT_REQUIRE(e.message() == "decode failed [asset:/a.png]: bad chunk CRC");
        CT_REQUIRE(make_error(error_code::not_found).message() == "no such entry");
    }

} // namespace

int main()
{
    test_handle_packing();
    test_flag_operators_are_reused();
    test_blob_adopt_and_borrow();
    test_memory_source_and_vfs();
    test_vfs_resolves_relative_names();
    test_more_specific_mount_wins();
    test_registry_insert_and_resolve();
    test_registry_detects_stale_handles();
    test_registry_replace_keeps_handles_live();
    test_registry_acquire_and_unnamed();
    test_store_holds_one_registry_per_type();
    test_image_mip_arithmetic();
    test_error_message();

    return 0;
}
