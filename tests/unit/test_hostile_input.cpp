// Hostile input, deterministically (architecture sections 15 and 16).
//
// Section 16 puts libFuzzer over the parsers and says it should "run
// continuously, not in PR CI". That is right, and it leaves a gap: a fuzzer that
// runs elsewhere catches a regression days after it is committed. So this is the
// other half -- the same fuzz targets, driven over a corpus this file generates
// from a fixed seed, on every build.
//
// It is not a substitute for the real thing and does not pretend to be. What it
// buys is that the inputs which HAVE broken something once, and the shapes that
// break parsers generally -- truncation, a length that lies, a size field that
// promises more than the file contains -- are checked in the same minute the
// code changes.
//
// The assertion is only that control comes back. Section 3.1: a malformed input
// "is the expected outcome for hostile input and must never be a crash", and
// section 15's threat model is that every input file is hostile. What these
// parsers RETURN for a corrupt file is a question for the other tests.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "docx_fixture.h"
#include "fuzz/targets.h"
#include "morph/core/caps.h"
#include "morph/core/ipc/codec.h"
#include "morph/core/ipc/frame.h"

using namespace morph;

namespace {

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char c : text) out.push_back(static_cast<std::byte>(c));
    return out;
}

// Inputs that are valid, or nearly so. A fuzzer starting from noise spends its
// first million iterations discovering that a DOCX begins with "PK"; starting
// from something real puts it inside the parser immediately, which is what
// section 16 means by "seed from the golden corpus".
std::vector<std::vector<std::byte>> Seeds() {
    std::vector<std::vector<std::byte>> seeds;

    seeds.push_back(Bytes("# Heading\n\nA paragraph with *emphasis*, a [link](x), and a\n\n"
                          "| table | here |\n| --- | --- |\n| a | b |\n\n- one\n- two\n"));
    seeds.push_back(Bytes("<html><head><title>T</title></head><body><h1>H</h1>"
                          "<p>Text with <b>bold</b> and <a href='x'>a link</a>.</p>"
                          "<ul><li>one</li><li>two</li></ul></body></html>"));

    // A real package, through the real writer, so the OPC and DOCX targets are
    // mutating something that parses rather than something that never did.
    test::DocxParts parts;
    parts.body = R"(<w:p><w:r><w:t>Hello</w:t></w:r></w:p>)"
                 R"(<w:tbl><w:tr><w:tc><w:p><w:r><w:t>cell</w:t></w:r></w:p></w:tc></w:tr></w:tbl>)";
    seeds.push_back(test::BuildDocx(parts));

    // A well-formed frame carrying a well-formed message.
    ipc::SubmitJob submit;
    submit.inputPaths = {"C:\\a\\b.docx", "C:\\a\\c.md"};
    submit.targetProfile = "application/pdf";
    submit.output.namingTemplate = "{name}.{ext}";
    seeds.push_back(ipc::EncodeFrame(ipc::MessageType::kSubmitJob, Encode(submit)));

    // A well-formed capability table.
    CapabilitySet caps;
    CapabilitySet::Source source;
    source.extension = "docx";
    source.targets.push_back({"application/pdf", "pdf", 1, 2, false});
    source.targets.push_back({"text/markdown", "md", 2, 2, false});
    caps.sources.push_back(std::move(source));
    seeds.push_back(SerialiseCapabilities(caps, 3));

    // And the degenerate cases every parser meets eventually.
    seeds.emplace_back();
    seeds.push_back(Bytes("\0\0\0\0"));
    seeds.push_back(Bytes("PK\x03\x04"));
    return seeds;
}

// The mutations that actually find things, rather than uniform noise.
//
// A flipped bit inside a length field, a file cut in half, a size that promises
// more than is there: these are the shapes that turn "parse this" into "read
// past the end of this", and they are cheap to generate deliberately instead of
// waiting for a random walk to find them.
std::vector<std::byte> Mutate(const std::vector<std::byte>& seed, std::mt19937& random) {
    std::vector<std::byte> out = seed;
    if (out.empty()) return out;

    switch (random() % 6) {
        case 0: {  // truncate
            const std::size_t keep = random() % out.size();
            out.resize(keep);
            break;
        }
        case 1: {  // flip a bit
            const std::size_t at = random() % out.size();
            out[at] = static_cast<std::byte>(static_cast<unsigned char>(out[at]) ^
                                             (1u << (random() % 8)));
            break;
        }
        case 2: {  // overwrite four bytes with 0xFF, which is every length field's
                   // worst day
            const std::size_t at = random() % out.size();
            for (std::size_t i = at; i < out.size() && i < at + 4; ++i) {
                out[i] = std::byte{0xFF};
            }
            break;
        }
        case 3: {  // splice a chunk over itself
            const std::size_t at = random() % out.size();
            const std::size_t from = random() % out.size();
            const std::size_t count = std::min<std::size_t>(out.size() - std::max(at, from),
                                                            1 + random() % 64);
            for (std::size_t i = 0; i < count; ++i) out[at + i] = out[from + i];
            break;
        }
        case 4: {  // grow it, so a size field that was right no longer is
            out.insert(out.end(), 1 + random() % 128, std::byte{0x41});
            break;
        }
        default: {  // zero a run
            const std::size_t at = random() % out.size();
            const std::size_t count = std::min<std::size_t>(out.size() - at, 1 + random() % 32);
            for (std::size_t i = 0; i < count; ++i) out[at + i] = std::byte{0};
            break;
        }
    }
    return out;
}

}  // namespace

TEST_CASE("No parser crashes on a corpus of hostile input", "[fuzz][security]") {
    // Fixed seed: a failure here has to be reproducible from the test name
    // alone, and a suite that fails one run in fifty is a suite people learn to
    // re-run rather than read.
    std::mt19937 random(20260904);

    const std::vector<std::vector<std::byte>> seeds = Seeds();
    const std::span<const morph::fuzz::Target> targets = morph::fuzz::Targets();
    REQUIRE_FALSE(targets.empty());

    // Every seed through every target first: a DOCX handed to the Markdown
    // reader is a legitimate thing for a user to do by renaming a file, and
    // section 5 says the answer must be "no" rather than a crash.
    for (const morph::fuzz::Target& target : targets) {
        INFO(target.name);
        for (const std::vector<std::byte>& seed : seeds) {
            target.run(seed);
        }
    }

    // Then the mutations. Two hundred per target is a second of wall clock and
    // is not a fuzzing campaign; it is a tripwire.
    for (const morph::fuzz::Target& target : targets) {
        INFO(target.name);
        for (int iteration = 0; iteration < 200; ++iteration) {
            const std::vector<std::byte>& seed = seeds[random() % seeds.size()];
            target.run(Mutate(seed, random));
        }
    }
    SUCCEED();
}

TEST_CASE("A capability table cannot be talked into reading out of bounds",
          "[fuzz][security][caps]") {
    // The table is the one input that is not a file: it is a shared section any
    // process running as this user can write to, read by a shell extension
    // inside Explorer. Its validator gets its own pass, with mutations aimed at
    // the header rather than spread over the whole thing.
    std::mt19937 random(20260905);

    CapabilitySet set;
    CapabilitySet::Source source;
    source.extension = "png";
    source.targets.push_back({"image/jpeg", "jpg", 1, 1, false});
    source.targets.push_back({"image/tiff", "tiff", 0, 1, false});
    set.sources.push_back(std::move(source));

    const std::vector<std::byte> valid = SerialiseCapabilities(set, 9);
    REQUIRE(CapabilityTable::Attach(valid.data(), valid.size()).Valid());

    for (int iteration = 0; iteration < 2000; ++iteration) {
        std::vector<std::byte> corrupt = valid;
        // Aim at the header: every count and offset the reader trusts lives in
        // the first forty-four bytes.
        const std::size_t at = random() % std::min<std::size_t>(corrupt.size(), 44);
        corrupt[at] = static_cast<std::byte>(random() % 256);

        const CapabilityTable table = CapabilityTable::Attach(corrupt.data(), corrupt.size());
        if (!table.Valid()) continue;

        // If it says it is valid, then everything it hands out has to be inside
        // the mapping. Reading them all is the assertion: under ASAN a lie here
        // is a report, and without it this at least proves the walk terminates.
        for (const CapsExtension& entry : table.Extensions()) {
            const std::string_view name = table.String(entry.nameOff);
            CHECK(name.size() < corrupt.size());
            for (const CapsTarget& target : table.TargetsFor(name)) {
                CHECK(table.String(target.profileOff).size() < corrupt.size());
                CHECK(table.String(target.extensionOff).size() < corrupt.size());
            }
        }
    }
    SUCCEED();
}

TEST_CASE("A frame that lies about its length is refused", "[fuzz][security][ipc]") {
    // The specific shape the framing layer exists to catch: a length prefix that
    // disagrees with what follows. frame.h calls it "the cheapest possible
    // tripwire for a desynchronised or hostile peer".
    ipc::Hello hello;
    const std::vector<std::byte> payload = Encode(hello);

    std::vector<std::byte> frame = ipc::EncodeFrame(ipc::MessageType::kHello, payload);
    REQUIRE(frame.size() > ipc::kFrameHeaderBytes);

    // Claim the payload is enormous.
    frame[0] = std::byte{0xFF};
    frame[1] = std::byte{0xFF};
    frame[2] = std::byte{0xFF};
    frame[3] = std::byte{0xFF};

    ipc::FrameReader reader;
    // Either it refuses the append outright or it waits forever for bytes that
    // will never come. What it must not do is hand back a frame that claims four
    // gigabytes of payload it does not have.
    if (reader.Append(frame)) {
        CHECK_FALSE(reader.Next().has_value());
    } else {
        CHECK(reader.Failed());
    }
}
