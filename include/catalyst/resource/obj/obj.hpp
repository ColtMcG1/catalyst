/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file obj.hpp
 * @brief The parsed contents of a Wavefront OBJ file: the three vertex arrays and the face tape.
 * @details An OBJ file is three independent arrays -- positions (`v`), texture coordinates (`vt`)
 * and normals (`vn`) -- plus faces (`f`) whose every corner indexes into those arrays separately.
 * The three arrays are *not* parallel: a cube has 8 positions, 6 normals and however many texture
 * coordinates its unwrap needed, and the corner `5/12/3` pulls one of each. That is why a face is
 * not a `vec3i` and why the vertex arrays cannot simply be zipped together; turning this into the
 * single interleaved vertex buffer a GPU wants is a de-indexing step that belongs to whoever builds
 * the mesh, not to the parser.
 *
 * Faces are stored the way `catalyst::resource::csv` stores rows: one flat array of corners plus an
 * offsets array saying where each face starts. OBJ permits an n-gon, so faces are variable length,
 * and a vector-of-vectors would put one allocation behind every face of the model. @ref obj::face
 * hands back a `std::span` over a run of @ref obj::face_vertices, and triangulating is the caller's
 * choice -- a fan is right for the convex polygons exporters emit, and nothing here has thrown away
 * the information needed to do better.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/math/math.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace catalyst::resource::obj
{

    /**
     * @struct face_vertex
     * @brief One corner of one face: an index into each of the three vertex arrays.
     * @details Indices are 0-based and ready to subscript, which is not how they appear in the file.
     * OBJ numbers from 1 and also allows a negative index counting back from the most recently
     * declared element, so `f -3 -2 -1` means "the last three positions"; @ref parser resolves both
     * spellings against the counts seen so far, which is why resolution has to happen during the
     * parse and not afterwards.
     *
     * @ref texcoord and @ref normal are @ref absent when the file omitted them -- `f 1 2 3` gives
     * positions only, and `f 1//4 2//5 3//6` gives positions and normals. @ref position is never
     * absent in a face the parser accepted.
     */
    struct face_vertex
    {
        /** @brief The value @ref texcoord and @ref normal carry when the corner did not name one. */
        static constexpr std::int32_t absent = -1;

        /** @brief Index into @ref obj::vertices. Always valid. */
        std::int32_t position = absent;

        /** @brief Index into @ref obj::texcoords, or @ref absent. */
        std::int32_t texcoord = absent;

        /** @brief Index into @ref obj::normals, or @ref absent. */
        std::int32_t normal = absent;

        friend bool operator==(const face_vertex &, const face_vertex &) = default;
    };

    /**
     * @struct obj
     * @brief Everything @ref parser retains from an OBJ file.
     * @details Deliberately only the geometry. Material references (`mtllib`, `usemtl`), object and
     * group names (`o`, `g`), smoothing groups (`s`) and the free-form curve records are parsed past
     * and dropped: a material system to attach them to does not exist yet, and a field that is
     * always empty is worse than no field. They are additive when it does -- an array of names plus
     * a per-face index, alongside the tape that is already here.
     */
    struct obj
    {
        /** @brief Geometric vertices, one per `v` record. A fourth `w` component, and the
         * per-vertex colours some exporters append, are read past and discarded. */
        std::vector<math::vec3f> vertices;

        /** @brief Vertex normals, one per `vn` record. Not guaranteed normalized -- the file says
         * what it says. */
        std::vector<math::vec3f> normals;

        /** @brief Texture coordinates, one per `vt` record. OBJ allows one to three components; a
         * missing `v` is stored as 0. */
        std::vector<math::vec2f> texcoords;

        /** @brief Every face's corners, concatenated. Face `i` owns the half-open run
         * `[face_offsets[i], face_offsets[i + 1])`. */
        std::vector<face_vertex> face_vertices;

        /**
         * @brief Where each face begins in @ref face_vertices, with a trailing end sentinel.
         * @details Either empty (no faces) or `face_count() + 1` entries beginning at 0 and ending
         * at `face_vertices.size()`. The sentinel is what lets @ref face avoid a special case for
         * the last face.
         */
        std::vector<std::uint32_t> face_offsets;

        /** @brief How many faces the file declared. */
        [[nodiscard]] std::size_t face_count() const noexcept
        {
            return face_offsets.empty() ? 0 : face_offsets.size() - 1;
        }

        /**
         * @fn face
         * @brief The corners of face @p index, in the winding order the file gave them.
         * @param index A face index below @ref face_count.
         * @return A span of at least three corners, or an empty span if @p index is out of range.
         * @warning The span points into @ref face_vertices and is invalidated by anything that
         * reallocates it.
         */
        [[nodiscard]] std::span<const face_vertex> face(std::size_t index) const noexcept
        {
            if (index >= face_count())
                return {};
            const std::uint32_t begin = face_offsets[index];
            const std::uint32_t end = face_offsets[index + 1];
            return std::span<const face_vertex>(face_vertices.data() + begin, end - begin);
        }
    };

} // namespace catalyst::resource::obj
