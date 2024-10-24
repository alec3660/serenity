/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibGfx/Path.h>

TEST_CASE(path_to_fill_short_wide_line_with_butt_linecap)
{
    // Test drawing a horizontal rect by stroking a vertical short wide line.
    {
        int width = 100;
        int height = 1;
        Gfx::Path path;
        path.move_to({ width / 2, 0 });
        path.line_to({ width / 2, height });
        auto fill = path.stroke_to_fill(width, Gfx::Path::CapStyle::Butt);
        EXPECT_EQ(fill.bounding_box(), Gfx::FloatRect({ 0, 0 }, { width, height }));
    }

    // Test drawing a vertical rect by stroking a horizontal short wide line.
    {
        int width = 1;
        int height = 100;
        Gfx::Path path;
        path.move_to({ 0, height / 2 });
        path.line_to({ width, height / 2 });
        auto fill = path.stroke_to_fill(height, Gfx::Path::CapStyle::Butt);
        EXPECT_EQ(fill.bounding_box(), Gfx::FloatRect({ 0, 0 }, { width, height }));
    }
}
TEST_CASE(path_to_fill_square_linecap)
{
    int line_width = 10;
    int width = 100;
    Gfx::Path path;
    path.move_to({ line_width / 2, line_width / 2 });
    path.line_to({ width - line_width / 2, line_width / 2 });
    auto fill = path.stroke_to_fill(line_width, Gfx::Path::CapStyle::Square);
    EXPECT_EQ(fill.bounding_box(), Gfx::FloatRect({ 0, 0 }, { width, line_width }));
}

TEST_CASE(path_to_fill_single_point)
{
    Gfx::Path path;
    path.move_to({ 10, 10 });
    path.line_to({ 10, 10 });

    {
        auto fill = path.stroke_to_fill(8, Gfx::Path::CapStyle::Butt);
        EXPECT(fill.is_empty());
    }

    {
        auto fill = path.stroke_to_fill(8, Gfx::Path::CapStyle::Round);
        EXPECT(!fill.is_empty());
    }

    {
        auto fill = path.stroke_to_fill(8, Gfx::Path::CapStyle::Square);
        EXPECT_EQ(fill.bounding_box(), Gfx::FloatRect({ 6, 6 }, { 8, 8 }));
    }
}

TEST_CASE(path_to_fill_two_single_points)
{
    Gfx::Path path;
    path.move_to({ 10, 10 });
    path.line_to({ 10, 10 });
    path.move_to({ 20, 20 });
    path.line_to({ 20, 20 });

    {
        auto fill = path.stroke_to_fill(8, Gfx::Path::CapStyle::Butt);
        EXPECT(fill.is_empty());
    }

    {
        auto fill = path.stroke_to_fill(8, Gfx::Path::CapStyle::Round);
        EXPECT(!fill.is_empty());
    }

    {
        auto fill = path.stroke_to_fill(8, Gfx::Path::CapStyle::Square);
        EXPECT(!fill.is_empty());
    }
}

TEST_CASE(path_to_string)
{
    {
        Gfx::Path path;
        path.move_to({ 10, 10 });
        path.line_to({ 20, 20 });
        path.quadratic_bezier_curve_to({ 30, 30 }, { 40, 40 });
        path.cubic_bezier_curve_to({ 50, 50 }, { 60, 60 }, { 10, 10 });
        path.close();
        EXPECT_EQ(path.to_byte_string(), "M 10,10 L 20,20 Q 30,30 40,40 C 50,50 60,60 10,10 Z");
    }

    {
        Gfx::Path path;
        path.move_to({ 10, 10 });
        path.line_to({ 20, 20 });
        path.quadratic_bezier_curve_to({ 30, 30 }, { 40, 40 });
        path.cubic_bezier_curve_to({ 50, 50 }, { 60, 60 }, { 10, 10 });
        EXPECT_EQ(path.to_byte_string(), "M 10,10 L 20,20 Q 30,30 40,40 C 50,50 60,60 10,10");
    }
}
