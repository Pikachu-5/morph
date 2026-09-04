#include "targets.h"

#include <array>
#include <string_view>
#include <vector>

#include "morph/core/caps.h"
#include "morph/core/doc/document.h"
#include "morph/core/ipc/codec.h"
#include "morph/core/ipc/frame.h"
#include "morph/core/ipc/worker_protocol.h"
#include "morph/core/options.h"
#include "morph/core/stream.h"
#include "morph/core/temp_store.h"
#include "morph/providers/docx.h"
#include "morph/providers/html.h"
#include "morph/providers/markdown.h"
#include "morph/providers/opc.h"

namespace morph::fuzz {
namespace {

// A context a parser can be handed a million times a second.
//
// The temp store is deliberately NOT a ScopedTempStore: creating and reaping a
// directory per input would make the fuzzer measure the filesystem instead of
// the parser. No reader in the document pipeline touches it -- they read from
// the source and build the IR in memory -- and one that starts to will fail
// loudly here rather than quietly writing a million files.
struct RefusingTempStore final : ITempStore {
    std::filesystem::path Acquire(std::string_view) override { return {}; }
};

struct SilentProgress final : IProgressSink {
    void Report(double, std::uint64_t) override {}
};

// Reads through a document reader, discarding whatever comes out.
void ReadDocument(doc::IDocumentReader& reader, std::span<const std::byte> data,
                  std::string_view profile) {
    MemorySource source(std::string_view(reinterpret_cast<const char*>(data.data()),
                                         data.size()));

    const std::optional<MediaProfile> from = MediaProfile::Parse(profile);
    if (!from) return;

    OptionBag options;
    SilentProgress progress;
    RefusingTempStore temp;
    Capabilities capabilities;
    std::stop_source stop;
    ConvertContext context{options, progress, stop.get_token(), temp, capabilities};

    doc::Document document;
    // The Result is discarded on purpose. A failure is the expected outcome for
    // most of these inputs and says nothing; what is being tested is that
    // control came back at all.
    (void)reader.Read(context, source, *from, document);
}

constexpr std::array<Target, 6> kTargets{{
    {"markdown", &FuzzMarkdown},
    {"html", &FuzzHtml},
    {"opc", &FuzzOpc},
    {"docx", &FuzzDocx},
    {"frame", &FuzzFrame},
    {"caps", &FuzzCapabilityTable},
}};

}  // namespace

void FuzzMarkdown(std::span<const std::byte> data) {
    docmd::MarkdownReader reader;
    ReadDocument(reader, data, "text/markdown");
}

void FuzzHtml(std::span<const std::byte> data) {
    dochtml::HtmlReader reader;
    ReadDocument(reader, data, "text/html");
}

void FuzzOpc(std::span<const std::byte> data) {
    // The container layer on its own: a ZIP with a hostile central directory
    // never reaches the DOCX reader, and section 15.2's defences live here.
    opc::Package package;
    (void)opc::Package::Open(data, package);
}

void FuzzDocx(std::span<const std::byte> data) {
    docooxml::DocxReader reader;
    ReadDocument(reader, data, "application/docx");
}

void FuzzFrame(std::span<const std::byte> data) {
    // The framing layer reads a length off the wire and has to survive one that
    // disagrees with what follows.
    ipc::FrameReader reader;
    if (!reader.Append(data)) return;

    while (const std::optional<ipc::FrameReader::Frame> frame = reader.Next()) {
        // Every decoder, over whatever payload arrived. Decoding is total by
        // contract (codec.h): it returns false rather than throwing, and never
        // partially applies.
        ipc::Hello hello;
        (void)Decode(frame->payload, hello);
        ipc::SubmitJob submit;
        (void)Decode(frame->payload, submit);
        ipc::TaskDone done;
        (void)Decode(frame->payload, done);
        ipc::GraphDescription description;
        (void)Decode(frame->payload, description);
        ipc::RunTask task;
        (void)Decode(frame->payload, task);
    }
}

void FuzzCapabilityTable(std::span<const std::byte> data) {
    // The one input that is not a file and not a message: a shared section that
    // any process running as this user can write to, read by a shell extension
    // inside Explorer. Attach validates every count and offset before the reader
    // can dereference anything, and this is what proves it.
    const CapabilityTable table = CapabilityTable::Attach(data.data(), data.size());
    if (!table.Valid()) return;

    for (const CapsExtension& entry : table.Extensions()) {
        const std::string_view name = table.String(entry.nameOff);
        for (const CapsTarget& target : table.TargetsFor(name)) {
            (void)table.String(target.profileOff);
            (void)table.String(target.extensionOff);
        }
    }
    // And a lookup for something that is not there, which walks the index.
    (void)table.TargetsFor("zzz");
}

std::span<const Target> Targets() noexcept {
    return std::span<const Target>(kTargets.data(), kTargets.size());
}

}  // namespace morph::fuzz
