#include "morph/providers/mf.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "morph/core/extensions.h"
#include "morph/core/stream.h"
#include "morph/providers/com_stream.h"
#include "mf_formats.h"

namespace morph::mf {
namespace {

using Microsoft::WRL::ComPtr;

// Media Foundation is started once per process and never shut down: MFShutdown
// while another thread holds a reader is a use-after-free, and the worker is
// expendable and short-lived anyway (section 3.1). The guard exists to make the
// startup happen exactly once, not to tear it down.
struct Runtime {
    Runtime() { ok = SUCCEEDED(::MFStartup(MF_VERSION, MFSTARTUP_LITE)); }
    bool ok = false;
};

bool StartRuntime() {
    static const Runtime runtime;
    return runtime.ok;
}

Result Failed(HRESULT hr, std::string_view what) {
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));

    if (hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE || hr == MF_E_UNSUPPORTED_FORMAT ||
        hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_TRANSFORM_TYPE_NOT_SET ||
        hr == MF_E_TOPO_CODEC_NOT_FOUND) {
        return Result::Error(Status::kMissingCapability,
                             std::string(what) +
                                 ": this machine has no codec for that (" + code + ").");
    }
    if (hr == MF_E_INVALID_FILE_FORMAT || hr == MF_E_INVALID_STREAM_DATA ||
        hr == MF_E_UNEXPECTED) {
        return Result::Error(Status::kMalformedInput,
                             std::string(what) + ": the media is damaged or not what it claims "
                                                 "to be (" + code + ").");
    }
    if (hr == E_OUTOFMEMORY) {
        return Result::Error(Status::kResourceLimit,
                             std::string(what) + ": not enough memory for this file.");
    }
    return Result::Error(Status::kInternal, std::string(what) + " failed (" + code + ").");
}

// --- Media types -------------------------------------------------------------

const GUID& SubtypeFor(std::string_view codec) {
    if (codec == "h264") return MFVideoFormat_H264;
    if (codec == "hevc") return MFVideoFormat_HEVC;
    if (codec == "wmv") return MFVideoFormat_WMV3;
    if (codec == "aac") return MFAudioFormat_AAC;
    if (codec == "mp3") return MFAudioFormat_MP3;
    if (codec == "wma") return MFAudioFormat_WMAudioV9;
    if (codec == "flac") return MFAudioFormat_FLAC;
    return MFAudioFormat_PCM;
}

// The codec a stream is actually carrying, as one of our codec names. Empty when
// it is something this backend does not have a name for -- which is not an
// error, only a route that cannot remux.
std::string_view CodecOf(IMFMediaType& type) {
    GUID subtype{};
    if (FAILED(type.GetGUID(MF_MT_SUBTYPE, &subtype))) return {};

    if (::IsEqualGUID(subtype, MFVideoFormat_H264)) return "h264";
    if (::IsEqualGUID(subtype, MFVideoFormat_HEVC)) return "hevc";
    if (::IsEqualGUID(subtype, MFVideoFormat_WMV3)) return "wmv";
    if (::IsEqualGUID(subtype, MFAudioFormat_AAC)) return "aac";
    if (::IsEqualGUID(subtype, MFAudioFormat_MP3)) return "mp3";
    if (::IsEqualGUID(subtype, MFAudioFormat_WMAudioV9)) return "wma";
    if (::IsEqualGUID(subtype, MFAudioFormat_FLAC)) return "flac";
    if (::IsEqualGUID(subtype, MFAudioFormat_PCM)) return "pcm";
    return {};
}

// The PCM type an encoder is offered as input: ordinary stereo CD audio.
void FillPcmType(IMFMediaType& into, UINT32 channels, UINT32 rate) {
    (void)into.SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    (void)into.SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    (void)into.SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    (void)into.SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    (void)into.SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    (void)into.SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
    (void)into.SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);
    (void)into.SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
}

// The output type an audio encoder will actually accept, chosen from the ones it
// offers rather than invented.
//
// This is the correction that made MP3 and WMA work at all. An AAC encoder takes
// a type built from a specification; the MP3 and WMA encoders take one of a
// short list of exact configurations and refuse everything else with
// MF_E_INVALIDMEDIATYPE, which is what "the encoder enumerates but the
// conversion fails" looked like from the outside. Asking the encoder what it can
// produce, and picking the closest to what the user asked for, is both more
// correct and less code than encoding the lists here -- and it stays correct on
// a machine with an encoder we have never seen.
//
// `channels` and `rate` are preferences, not requirements: an encoder that only
// does 48 kHz is still an encoder, and the sink writer resamples.
bool BestAudioOutputType(const GUID& subtype, UINT32 channels, UINT32 rate,
                         UINT32 wantBytesPerSecond, ComPtr<IMFMediaType>& into,
                         ComPtr<IMFTransform>* keepTransform = nullptr) {
    MFT_REGISTER_TYPE_INFO wanted{};
    wanted.guidMajorType = MFMediaType_Audio;
    wanted.guidSubtype = subtype;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const UINT32 flags = MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_TRANSCODE_ONLY;
    if (FAILED(::MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, flags, nullptr, &wanted, &activates,
                           &count))) {
        return false;
    }

    ComPtr<IMFMediaType> best;
    ComPtr<IMFTransform> bestTransform;
    std::uint64_t bestScore = UINT64_MAX;

    for (UINT32 i = 0; i < count; ++i) {
        if (activates[i] == nullptr) continue;
        ComPtr<IMFTransform> transform;
        if (FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(&transform)))) continue;

        for (DWORD index = 0;; ++index) {
            ComPtr<IMFMediaType> candidate;
            if (FAILED(transform->GetOutputAvailableType(0, index, &candidate))) break;

            UINT32 candidateChannels = 0;
            UINT32 candidateRate = 0;
            UINT32 candidateBytes = 0;
            (void)candidate->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &candidateChannels);
            (void)candidate->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &candidateRate);
            (void)candidate->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &candidateBytes);

            // Channel count and sample rate first, bitrate second: a stereo file
            // that came back mono is a complaint, and one encoded at 160 kbps
            // instead of 192 is not.
            std::uint64_t score = 0;
            if (candidateChannels != channels) score += 1'000'000'000ull;
            if (candidateRate != rate) score += 1'000'000ull;
            score += candidateBytes > wantBytesPerSecond ? candidateBytes - wantBytesPerSecond
                                                         : wantBytesPerSecond - candidateBytes;
            if (score < bestScore) {
                bestScore = score;
                best = candidate;
                bestTransform = transform;
            }
        }
        if (bestTransform.Get() != transform.Get()) activates[i]->ShutdownObject();
    }

    for (UINT32 i = 0; i < count; ++i) {
        if (activates[i] != nullptr) activates[i]->Release();
    }
    ::CoTaskMemFree(activates);

    if (!best) return false;
    into = best;
    if (keepTransform != nullptr) *keepTransform = bestTransform;
    return true;
}

// Copies the attributes an encoder needs to describe the same picture: size,
// frame rate, aspect ratio, and interlacing. Everything else is the encoder's
// business.
void CopyVideoShape(IMFMediaType& from, IMFMediaType& to) {
    UINT64 value = 0;
    if (SUCCEEDED(from.GetUINT64(MF_MT_FRAME_SIZE, &value))) {
        (void)to.SetUINT64(MF_MT_FRAME_SIZE, value);
    }
    if (SUCCEEDED(from.GetUINT64(MF_MT_FRAME_RATE, &value))) {
        (void)to.SetUINT64(MF_MT_FRAME_RATE, value);
    } else {
        (void)MFSetAttributeRatio(&to, MF_MT_FRAME_RATE, 30, 1);
    }
    if (SUCCEEDED(from.GetUINT64(MF_MT_PIXEL_ASPECT_RATIO, &value))) {
        (void)to.SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, value);
    } else {
        (void)MFSetAttributeRatio(&to, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    }
    (void)to.SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
}

// A sensible video bitrate for a picture of this size, when the user has not
// said. Roughly 0.1 bits per pixel per frame at 30 fps, which is a defensible
// middle for H.264 and generous for HEVC.
std::uint32_t DefaultVideoBitrate(IMFMediaType& decoded) {
    UINT32 width = 0;
    UINT32 height = 0;
    if (FAILED(MFGetAttributeSize(&decoded, MF_MT_FRAME_SIZE, &width, &height))) {
        return 4'000'000;
    }
    const double pixels = static_cast<double>(width) * height;
    const double bits = pixels * 0.1 * 30.0;
    return static_cast<std::uint32_t>(std::clamp(bits, 400'000.0, 40'000'000.0));
}

Result MakeVideoOutputType(const Format& target, IMFMediaType& decoded,
                           const OptionBag& options, ComPtr<IMFMediaType>& into) {
    if (const HRESULT hr = ::MFCreateMediaType(&into); FAILED(hr)) {
        return Failed(hr, "Describing the output video");
    }
    (void)into->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    (void)into->SetGUID(MF_MT_SUBTYPE, SubtypeFor(target.codec));

    const std::int64_t bitrate = options.GetInt(
        "videoBitrate", static_cast<std::int64_t>(DefaultVideoBitrate(decoded)));
    (void)into->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(
                                                 std::clamp<std::int64_t>(bitrate, 100'000,
                                                                          100'000'000)));
    CopyVideoShape(decoded, *into.Get());
    return Result::Ok();
}

Result MakeAudioOutputType(const Format& target, IMFMediaType& decoded,
                           const OptionBag& options, ComPtr<IMFMediaType>& into) {
    UINT32 channels = 2;
    UINT32 rate = 44100;
    (void)decoded.GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    (void)decoded.GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    channels = std::clamp<UINT32>(channels, 1, 2);

    // PCM is not encoded, it is written. The WAV sink takes the samples as they
    // come out of the decoder, so this type is ours to state.
    if (target.codec == "pcm") {
        if (const HRESULT hr = ::MFCreateMediaType(&into); FAILED(hr)) {
            return Failed(hr, "Describing the output audio");
        }
        FillPcmType(*into.Get(), channels, rate);
        return Result::Ok();
    }

    const std::int64_t kbps = std::clamp<std::int64_t>(options.GetInt("audioBitrate", 192), 32,
                                                       320);
    const auto requested = static_cast<UINT32>(kbps * 1000 / 8);

    if (!BestAudioOutputType(SubtypeFor(target.codec), channels, rate, requested, into)) {
        return Result::Error(Status::kMissingCapability,
                             "This machine has no encoder for " + std::string(target.codec) +
                                 ".");
    }
    return Result::Ok();
}

// --- Encoder probing ---------------------------------------------------------

// The media types a trial negotiation offers: ordinary stereo CD audio, and a
// 720p30 picture. Nothing exotic, so a refusal means the encoder cannot do the
// job at all rather than that it disliked these particular numbers.
Result TrialTypes(const GUID& subtype, Media media, ComPtr<IMFMediaType>& output,
                  ComPtr<IMFMediaType>& input) {
    if (FAILED(::MFCreateMediaType(&output)) || FAILED(::MFCreateMediaType(&input))) {
        return Result::Error(Status::kInternal, "Could not describe a trial media type.");
    }

    if (media == Media::kVideo) {
        (void)output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        (void)output->SetGUID(MF_MT_SUBTYPE, subtype);
        (void)output->SetUINT32(MF_MT_AVG_BITRATE, 4'000'000);
        (void)output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        (void)MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, 1280, 720);
        (void)MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, 30, 1);
        (void)MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        (void)input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        (void)input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        (void)input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        (void)MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, 1280, 720);
        (void)MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 30, 1);
        (void)MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        return Result::Ok();
    }

    // The encoder's own idea of what it can produce, not ours.
    if (!BestAudioOutputType(subtype, 2, 44100, 24000, output)) {
        return Result::Error(Status::kMissingCapability, "No encoder offers that format.");
    }
    FillPcmType(*input.Get(), 2, 44100);
    return Result::Ok();
}

// Whether an encoder for `subtype` exists AND will accept a conversion.
//
// Enumerating is not enough, and this is the second time the project has learned
// it: the image pipeline found a registered HEIF encoder that failed at commit
// because the HEVC encoder behind it was not installed, and here the MP3 encoder
// MFT enumerates on a stock Windows 11 and then refuses ordinary 44.1 kHz stereo
// PCM outright. Section 6.2's rule -- mark the edge unavailable rather than
// failing mid-job -- can only be applied by asking the encoder to do the thing.
//
// The negotiation is the whole trial: no samples are pushed through, because an
// encoder that accepts both types and then fails on data is a different and much
// rarer bug than one that was never going to work.
bool CanEncode(const GUID& category, const GUID& subtype, Media media) {
    MFT_REGISTER_TYPE_INFO wanted{};
    wanted.guidMajorType = media == Media::kVideo ? MFMediaType_Video : MFMediaType_Audio;
    wanted.guidSubtype = subtype;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    // TRANSCODE_ONLY alongside the rest: the MP3 and WMA encoders are only
    // enumerated under that flag, and leaving it out makes them invisible rather
    // than absent.
    const UINT32 flags = MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_TRANSCODE_ONLY;
    if (FAILED(::MFTEnumEx(category, flags, nullptr, &wanted, &activates, &count))) return false;

    ComPtr<IMFMediaType> output;
    ComPtr<IMFMediaType> input;
    bool usable = false;
    if (TrialTypes(subtype, media, output, input)) {
        for (UINT32 i = 0; i < count && !usable; ++i) {
            if (activates[i] == nullptr) continue;
            ComPtr<IMFTransform> transform;
            if (FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(&transform)))) continue;

            // Output first: an encoder cannot say what input it takes until it
            // knows what it is producing.
            if (SUCCEEDED(transform->SetOutputType(0, output.Get(), 0)) &&
                SUCCEEDED(transform->SetInputType(0, input.Get(), 0))) {
                usable = true;
            }
            transform.Reset();
            activates[i]->ShutdownObject();
        }
    }

    for (UINT32 i = 0; i < count; ++i) {
        if (activates[i] != nullptr) activates[i]->Release();
    }
    ::CoTaskMemFree(activates);
    return usable;
}

EncoderSupport ProbeEncoders() {
    EncoderSupport support;
    if (!StartRuntime()) return support;

    support.h264 = CanEncode(MFT_CATEGORY_VIDEO_ENCODER, MFVideoFormat_H264, Media::kVideo);
    support.hevc = CanEncode(MFT_CATEGORY_VIDEO_ENCODER, MFVideoFormat_HEVC, Media::kVideo);
    support.aac = CanEncode(MFT_CATEGORY_AUDIO_ENCODER, MFAudioFormat_AAC, Media::kAudio);
    support.mp3 = CanEncode(MFT_CATEGORY_AUDIO_ENCODER, MFAudioFormat_MP3, Media::kAudio);
    support.wma = CanEncode(MFT_CATEGORY_AUDIO_ENCODER, MFAudioFormat_WMAudioV9, Media::kAudio);
    // PCM needs no encoder: the WAV sink writes the samples it is handed.
    support.wav = true;
    return support;
}

bool EncoderPresent(const Format& format, const EncoderSupport& support) {
    if (!format.encode) return false;
    if (format.encoder.empty()) return true;
    if (format.encoder == "h264") return support.h264;
    if (format.encoder == "hevc") return support.hevc;
    if (format.encoder == "aac") return support.aac;
    if (format.encoder == "mp3") return support.mp3;
    if (format.encoder == "wma") return support.wma;
    return false;
}

// --- The conversion ----------------------------------------------------------

// One input stream and where its samples go.
struct StreamPlan {
    DWORD readerStream = 0;
    DWORD writerStream = 0;
    bool remux = false;  // compressed samples copied through, no codec involved
    Media media = Media::kAudio;
};

std::wstring Widen(const std::filesystem::path& path) { return path.wstring(); }

Result ConfigureStream(IMFSourceReader& reader, IMFSinkWriter& writer, DWORD readerStream,
                       const Format& target, const OptionBag& options, bool wantRemux,
                       StreamPlan& plan, Result& notes) {
    ComPtr<IMFMediaType> native;
    if (const HRESULT hr = reader.GetNativeMediaType(readerStream, 0, &native); FAILED(hr)) {
        return Failed(hr, "Reading the input stream's format");
    }

    plan.readerStream = readerStream;
    plan.media = target.media;

    if (wantRemux) {
        // Section 7.2's fast path: the samples already are what the target
        // wants, so nothing decodes and nothing encodes. The reader is left on
        // its native type, which is what makes it hand back compressed samples.
        if (const HRESULT hr = writer.AddStream(native.Get(), &plan.writerStream); FAILED(hr)) {
            return Failed(hr, "Adding the copied stream");
        }
        if (const HRESULT hr = writer.SetInputMediaType(plan.writerStream, native.Get(),
                                                        nullptr);
            FAILED(hr)) {
            return Failed(hr, "Copying the stream through");
        }
        plan.remux = true;
        return Result::Ok();
    }

    // Ask the reader for uncompressed samples. MF inserts the decoder.
    ComPtr<IMFMediaType> decodedRequest;
    if (const HRESULT hr = ::MFCreateMediaType(&decodedRequest); FAILED(hr)) {
        return Failed(hr, "Describing the decoded stream");
    }
    if (target.media == Media::kVideo) {
        (void)decodedRequest->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        (void)decodedRequest->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    } else {
        (void)decodedRequest->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        (void)decodedRequest->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        (void)decodedRequest->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    }
    if (const HRESULT hr = reader.SetCurrentMediaType(readerStream, nullptr,
                                                      decodedRequest.Get());
        FAILED(hr)) {
        return Failed(hr, "Decoding the input stream");
    }

    ComPtr<IMFMediaType> decoded;
    if (const HRESULT hr = reader.GetCurrentMediaType(readerStream, &decoded); FAILED(hr)) {
        return Failed(hr, "Reading the decoded format");
    }

    ComPtr<IMFMediaType> output;
    const Result described = target.media == Media::kVideo
                                 ? MakeVideoOutputType(target, *decoded.Get(), options, output)
                                 : MakeAudioOutputType(target, *decoded.Get(), options, output);
    if (!described) return described;

    if (const HRESULT hr = writer.AddStream(output.Get(), &plan.writerStream); FAILED(hr)) {
        return Failed(hr, "Adding the output stream");
    }
    if (const HRESULT hr = writer.SetInputMediaType(plan.writerStream, decoded.Get(), nullptr);
        FAILED(hr)) {
        return Failed(hr, "Connecting the encoder");
    }

    // Only a LOSSY target costs a generation. Decoding an MP3 into a WAV runs
    // this same path and loses nothing: the PCM holds exactly what came out of
    // the decoder, so calling it a re-encode would be a note that says the
    // opposite of the truth.
    if (target.lossy) {
        notes.Note("av.transcoded",
                   std::string(target.media == Media::kVideo ? "The video" : "The audio") +
                       " was re-encoded as " + std::string(target.codec) + ".",
                   "");
        notes.Incurred(Fidelity::High);
    }
    return Result::Ok();
}

}  // namespace

// --- Public surface ----------------------------------------------------------

std::vector<std::string> EncoderSupport::Ids() const {
    std::vector<std::string> ids;
    if (h264) ids.emplace_back("mf-h264-encode");
    if (hevc) ids.emplace_back("mf-hevc-encode");
    if (aac) ids.emplace_back("mf-aac-encode");
    if (mp3) ids.emplace_back("mf-mp3-encode");
    if (wma) ids.emplace_back("mf-wma-encode");
    if (wav) ids.emplace_back("mf-wav-encode");
    return ids;
}

const EncoderSupport& InstalledEncoders() {
    static const EncoderSupport support = ProbeEncoders();
    return support;
}

bool CanRemux(std::string_view from, std::string_view to) noexcept {
    const Format* source = FindFormat(from);
    const Format* target = FindFormat(to);
    if (source == nullptr || target == nullptr) return false;
    if (source->profile == target->profile) return false;

    // The one question that matters: are the samples already what the target
    // container wants? Same codec, different container, and nothing has to be
    // decoded. A 4 GB MOV to MP4 is an IO-bound copy instead of a twenty-minute
    // encode (section 7.2).
    if (source->codec != target->codec) return false;
    if (source->media != target->media) return false;
    return target->encode;
}

std::string_view IdentifyContainer(std::span<const std::byte> prefix) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(prefix.data());
    const std::size_t size = prefix.size();

    const auto at = [&](std::size_t offset, std::string_view text) {
        if (size < offset + text.size()) return false;
        return std::memcmp(bytes + offset, text.data(), text.size()) == 0;
    };

    // ISO base media. The brand distinguishes an MP4 from a QuickTime file, and
    // they are different nodes because one can be written and the other cannot.
    if (at(4, "ftyp") && size >= 12) {
        const std::string_view brand(reinterpret_cast<const char*>(bytes) + 8, 4);
        if (brand == "qt  ") return "video/quicktime";
        if (brand == "M4A " || brand == "M4B ") return "audio/mp4";
        return "video/mp4";
    }
    if (at(0, "RIFF") && at(8, "WAVE")) return "audio/wav";
    if (at(0, "fLaC")) return "audio/flac";
    // ASF/WMV header object GUID, first eight bytes are enough to be sure.
    if (size >= 8 && bytes[0] == 0x30 && bytes[1] == 0x26 && bytes[2] == 0xB2 &&
        bytes[3] == 0x75 && bytes[4] == 0x8E && bytes[5] == 0x66 && bytes[6] == 0xCF &&
        bytes[7] == 0x11) {
        return "video/x-ms-asf";
    }
    if (at(0, "ID3")) return "audio/mpeg";
    // A bare MPEG audio frame: eleven sync bits, and a layer field that is not
    // the reserved value. Weaker than the others, so it goes last.
    if (size >= 2 && bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0 &&
        (bytes[1] & 0x06) != 0x00) {
        return "audio/mpeg";
    }
    return {};
}

MediaFoundationProvider::MediaFoundationProvider() { (void)InstalledEncoders(); }

std::vector<EdgeDecl> MediaFoundationProvider::EnumerateEdges() const {
    const EncoderSupport& support = InstalledEncoders();
    std::vector<EdgeDecl> edges;

    for (const Format& from : Formats()) {
        if (!from.decode) continue;
        const std::optional<MediaProfile> source = MediaProfile::Parse(from.profile);
        if (!source) continue;

        for (const Format& to : Formats()) {
            if (from.profile == to.profile) continue;
            if (!EncoderPresent(to, support)) continue;
            // Audio cannot become video. The reverse is a legitimate request --
            // "give me the soundtrack" -- and is offered below.
            if (from.media == Media::kAudio && to.media == Media::kVideo) continue;

            const std::optional<MediaProfile> target = MediaProfile::Parse(to.profile);
            if (!target) continue;

            EdgeCost cost;
            if (CanRemux(from.profile, to.profile)) {
                // Nothing is decoded, so nothing is lost and nothing is spent.
                cost.fidelity = Fidelity::Lossless;
                cost.generational = 0;
                cost.cpuPerMiB = 3.0f;
                cost.memPeakFactor = 1.3f;
            } else if (from.media == Media::kVideo && to.media == Media::kAudio) {
                // The picture is discarded entirely. Nothing in the fidelity
                // scale is a perfect fit for "half the file is gone", and
                // Structural is the honest one: what survives is the content of
                // one stream, not the presentation of the whole.
                cost.fidelity = Fidelity::Structural;
                cost.generational = to.lossy ? 1 : 0;
                cost.cpuPerMiB = to.cpuPerMiB;
                cost.memPeakFactor = to.memPeakFactor;
            } else {
                // A lossless target holds exactly what it was given, whatever the
                // source was -- the same rule the image pipeline applies to PNG.
                cost.fidelity = to.lossy ? Fidelity::High : Fidelity::Lossless;
                cost.generational = to.lossy ? 1 : 0;
                cost.cpuPerMiB = from.cpuPerMiB + to.cpuPerMiB;
                cost.memPeakFactor = std::max(from.memPeakFactor, to.memPeakFactor);
            }

            // Always. An MP4 sink seeks backwards to write its index, and the
            // ISink contract is append-only on purpose (section 11.3), so the
            // output is built in the temp store and moved across at the end.
            cost.requiresDisk = true;

            edges.push_back(EdgeDecl{*source, *target, cost});
        }
    }
    return edges;
}

std::optional<MediaProfile> MediaFoundationProvider::Probe(ISource& source,
                                                           const PathHint& /*hint*/) const {
    const std::vector<std::byte> prefix = ReadAtMost(source, 64);
    const std::string_view container = IdentifyContainer(prefix);
    if (container.empty()) return std::nullopt;

    // The container is known from the bytes; the codec is not, and a profile
    // without one is not a node in the graph. Rather than opening the file a
    // second time to find out, the container's usual codec is assumed and the
    // conversion corrects itself: the reader reports what the stream really is,
    // and a remux is only chosen when they agree.
    for (const Format& format : Formats()) {
        if (format.profile.starts_with(container) && format.decode) {
            return MediaProfile::Parse(format.profile);
        }
    }
    return std::nullopt;
}

Result MediaFoundationProvider::Convert(ConvertContext& context, ISource& source, ISink& sink,
                                        const MediaProfile& from, const MediaProfile& to) {
    const Format* target = FindFormat(to.Id());
    if (target == nullptr || !EncoderPresent(*target, InstalledEncoders())) {
        return Result::Error(Status::kMissingCapability,
                             "Morph cannot write " + to.Id() + " on this machine.");
    }
    if (!StartRuntime()) {
        return Result::Error(Status::kMissingCapability,
                             "Media Foundation is not available on this machine.");
    }

    Result result = Result::Ok();
    context.progress.Report(0.0, 0);

    // --- Open the input ------------------------------------------------------
    comio::SourceStream input(source);
    ComPtr<IMFByteStream> byteStream;
    if (const HRESULT hr = ::MFCreateMFByteStreamOnStream(&input, &byteStream); FAILED(hr)) {
        return Failed(hr, "Opening the input");
    }

    ComPtr<IMFAttributes> attributes;
    if (const HRESULT hr = ::MFCreateAttributes(&attributes, 4); FAILED(hr)) {
        return Failed(hr, "Configuring the reader");
    }
    // Section 7.1: "Ask for hardware MFTs explicitly; without this you silently
    // get software."
    (void)attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    (void)attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> reader;
    if (const HRESULT hr = ::MFCreateSourceReaderFromByteStream(byteStream.Get(),
                                                                attributes.Get(), &reader);
        FAILED(hr)) {
        return Failed(hr, "Reading " + from.Id());
    }

    // --- Decide what goes where ---------------------------------------------
    // The stream-selection constants are negative enumerators that the API takes
    // as a DWORD, so the cast is the API's own shape rather than a conversion.
    if (FAILED(reader->SetStreamSelection(
            static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE))) {
        return Result::Error(Status::kInternal, "Could not choose the input streams.");
    }

    std::vector<StreamPlan> plans;

    // The temp file's EXTENSION is load-bearing: MFCreateSinkWriterFromURL picks
    // the muxer from it, and there is no other way to ask for one. A name ending
    // in anything Media Foundation does not recognise fails with a bare
    // 0xC00D36D5 that says nothing about why.
    const std::string_view extension = ExtensionFor(to.Id());
    if (extension.empty()) {
        return Result::Error(Status::kInternal,
                             "There is no file extension registered for " + to.Id() + ".");
    }
    const std::filesystem::path temporary =
        context.temp.Acquire("mf-out." + std::string(extension));

    ComPtr<IMFSinkWriter> writer;
    if (const HRESULT hr = ::MFCreateSinkWriterFromURL(Widen(temporary).c_str(), nullptr,
                                                       attributes.Get(), &writer);
        FAILED(hr)) {
        return Failed(hr, "Writing " + to.Id());
    }

    bool sawVideo = false;
    for (DWORD index = 0; index < 64; ++index) {
        ComPtr<IMFMediaType> native;
        const HRESULT hr = reader->GetNativeMediaType(index, 0, &native);
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        if (FAILED(hr)) continue;

        GUID major{};
        if (FAILED(native->GetGUID(MF_MT_MAJOR_TYPE, &major))) continue;

        const bool isVideo = ::IsEqualGUID(major, MFMediaType_Video);
        const bool isAudio = ::IsEqualGUID(major, MFMediaType_Audio);
        if (!isVideo && !isAudio) continue;

        // An audio target takes the audio and drops the picture; a video target
        // takes the first of each. A second video stream is not something any of
        // our containers carry, so it is left unselected rather than guessed at.
        if (target->media == Media::kAudio && isVideo) {
            if (!sawVideo) {
                result.Note("av.video-dropped",
                            "The picture was discarded; only the sound was kept.", "");
                result.Incurred(Fidelity::Structural);
                sawVideo = true;
            }
            continue;
        }
        if (isVideo && sawVideo) continue;
        if (isAudio && std::any_of(plans.begin(), plans.end(), [](const StreamPlan& plan) {
                return plan.media == Media::kAudio;
            })) {
            continue;
        }

        // A video target still needs its audio, and the audio target for that is
        // the container's own: AAC in MP4.
        const Format* streamTarget = target;
        if (target->media == Media::kVideo && isAudio) {
            streamTarget = FindFormat("audio/mp4;codec=aac");
            if (streamTarget == nullptr || !EncoderPresent(*streamTarget, InstalledEncoders())) {
                result.Note("av.audio-dropped",
                            "This machine has no AAC encoder, so the sound was left out.", "");
                result.Incurred(Fidelity::Structural);
                continue;
            }
        }

        const std::string_view actual = CodecOf(*native.Get());
        const bool remux = !actual.empty() && actual == streamTarget->codec;

        if (FAILED(reader->SetStreamSelection(index, TRUE))) continue;

        StreamPlan plan;
        if (const Result configured = ConfigureStream(*reader.Get(), *writer.Get(), index,
                                                      *streamTarget, context.options, remux,
                                                      plan, result);
            !configured) {
            return configured;
        }
        plans.push_back(plan);
        if (isVideo) sawVideo = true;
    }

    if (plans.empty()) {
        return Result::Error(Status::kUnsupportedInput,
                             "That file has nothing in it Morph can put in a " + to.Id() + ".");
    }

    if (std::all_of(plans.begin(), plans.end(),
                    [](const StreamPlan& plan) { return plan.remux; })) {
        // Worth saying: the difference between this and a transcode is minutes.
        result.Note("av.remuxed", "The streams were copied without re-encoding.", "");
    }

    // --- Pump ----------------------------------------------------------------
    if (const HRESULT hr = writer->BeginWriting(); FAILED(hr)) {
        return Failed(hr, "Starting to write " + to.Id());
    }

    // Duration drives the progress bar. A file that does not know its own
    // length still converts; the bar just cannot move proportionally.
    std::uint64_t duration = 0;
    {
        PROPVARIANT value{};
        ::PropVariantInit(&value);
        if (SUCCEEDED(reader->GetPresentationAttribute(
                static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value)) &&
            value.vt == VT_UI8) {
            duration = value.uhVal.QuadPart;
        }
        ::PropVariantClear(&value);
    }

    std::uint64_t written = 0;
    int sinceCancelCheck = 0;

    while (true) {
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;

        const HRESULT hr =
            reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &streamIndex,
                               &flags, &timestamp, &sample);
        if (FAILED(hr)) {
            (void)writer->Finalize();
            return Failed(hr, "Reading the media");
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 && !sample) break;

        // Every 100 ms at the outside: section 11.3's cooperative cancellation
        // wants a check at that granularity, and a sample is far shorter.
        if (++sinceCancelCheck >= 8) {
            sinceCancelCheck = 0;
            if (context.cancelled()) {
                (void)writer->Finalize();
                return Result::Cancelled();
            }
        }

        if (!sample) continue;

        const auto plan = std::find_if(plans.begin(), plans.end(),
                                       [streamIndex](const StreamPlan& candidate) {
                                           return candidate.readerStream == streamIndex;
                                       });
        if (plan == plans.end()) continue;  // a stream we did not select

        if (const HRESULT wrote = writer->WriteSample(plan->writerStream, sample.Get());
            FAILED(wrote)) {
            (void)writer->Finalize();
            return Failed(wrote, "Writing the media");
        }

        DWORD length = 0;
        if (SUCCEEDED(sample->GetTotalLength(&length))) written += length;

        if (duration > 0 && timestamp > 0) {
            const double fraction =
                static_cast<double>(timestamp) / static_cast<double>(duration);
            context.progress.Report(std::clamp(fraction, 0.0, 0.99), written);
        }
    }

    if (const HRESULT hr = writer->Finalize(); FAILED(hr)) {
        return Failed(hr, "Finishing " + to.Id());
    }
    writer.Reset();

    if (context.cancelled()) return Result::Cancelled();

    // --- Hand the file over --------------------------------------------------
    {
        FileSource produced(temporary);
        std::vector<std::byte> chunk(kStreamChunkBytes);
        std::uint64_t copied = 0;
        while (true) {
            const std::size_t got = produced.Read(std::span(chunk.data(), chunk.size()));
            if (got == 0) break;
            sink.Write(std::span(chunk.data(), got));
            copied += got;
        }
        if (copied == 0) {
            return Result::Error(Status::kInternal, "The encoder produced nothing.");
        }
        sink.Commit();
        context.progress.Report(1.0, copied);
    }

    return result;
}

}  // namespace morph::mf
