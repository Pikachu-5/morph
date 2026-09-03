#include <catch2/catch_test_macros.hpp>

#include "morph/core/media_profile.h"

using morph::MediaProfile;

TEST_CASE("a profile is a container plus attributes, not an extension", "[profile]") {
    const auto p = MediaProfile::Parse("video/mp4;codec=h264");
    REQUIRE(p.has_value());
    CHECK(p->Container() == "video/mp4");
    CHECK(p->Codec() == "h264");
    CHECK(p->Family() == "video");
    CHECK(p->Id() == "video/mp4;codec=h264");
}

TEST_CASE("the same codec in different containers is a different node", "[profile]") {
    // Section 4.1: collapsing these would make the planner blind to the
    // difference between a remux and a full re-encode.
    const auto mp4 = MediaProfile::Parse("video/mp4;codec=h264");
    const auto mkv = MediaProfile::Parse("video/mkv;codec=h264");
    const auto hevc = MediaProfile::Parse("video/mp4;codec=hevc");

    CHECK(*mp4 != *mkv);
    CHECK(*mp4 != *hevc);
}

TEST_CASE("canonicalisation makes equivalent spellings equal", "[profile]") {
    const auto a = MediaProfile::Parse("Video/MP4;Codec=H264;Profile=High");
    const auto b = MediaProfile::Parse("video/mp4 ; profile=high ; codec=h264");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    CHECK(a->Id() == b->Id());
    CHECK(*a == *b);
    // Attributes are sorted by key regardless of how they were written.
    CHECK(a->Id() == "video/mp4;codec=h264;profile=high");
}

TEST_CASE("a duplicate attribute key resolves last-wins", "[profile]") {
    const auto p = MediaProfile::Parse("image/jpeg;quality=70;quality=90");
    REQUIRE(p.has_value());
    CHECK(p->Attr("quality") == "90");
    CHECK(p->Attributes().size() == 1);
}

TEST_CASE("an absent attribute is distinguishable from an empty one", "[profile]") {
    const auto p = MediaProfile::Parse("image/png;codec=");
    REQUIRE(p.has_value());
    REQUIRE(p->Attr("codec").has_value());
    CHECK(p->Attr("codec")->empty());
    CHECK_FALSE(p->Attr("bitrate").has_value());
}

TEST_CASE("malformed specs are rejected rather than thrown on", "[profile]") {
    // These strings reach us from provider tables and, later, across the IPC
    // boundary (section 10.2), where a throw would be the wrong shape entirely.
    CHECK_FALSE(MediaProfile::Parse("").has_value());
    CHECK_FALSE(MediaProfile::Parse("   ").has_value());
    CHECK_FALSE(MediaProfile::Parse(";codec=h264").has_value());
    CHECK_FALSE(MediaProfile::Parse("video/mp4;codec").has_value());
    CHECK_FALSE(MediaProfile::Parse("video/mp4;=h264").has_value());
}

TEST_CASE("empty and repeated separators are tolerated", "[profile]") {
    const auto p = MediaProfile::Parse("video/mp4;;codec=h264;");
    REQUIRE(p.has_value());
    CHECK(p->Id() == "video/mp4;codec=h264");
}

TEST_CASE("a container with no codec is a complete profile", "[profile]") {
    const auto p = MediaProfile::Parse("image/png");
    REQUIRE(p.has_value());
    CHECK(p->Id() == "image/png");
    CHECK_FALSE(p->Codec().has_value());
    CHECK(p->Family() == "image");
    CHECK_FALSE(p->Empty());
}

TEST_CASE("a default-constructed profile is empty", "[profile]") {
    const MediaProfile p;
    CHECK(p.Empty());
    CHECK(p.Id().empty());
}
