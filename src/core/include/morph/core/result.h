// Morph — what a conversion returns.
//
// Section 12.2: "Failures listed with a real reason, never 'conversion failed'."
// That is a product requirement, and it is why Result carries a status the UI
// can branch on AND a message a person can read. A provider that returns
// Status::kMalformedInput with an empty message has not finished its job.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "morph/core/fidelity.h"

namespace morph {

enum class Status {
    kOk = 0,

    // The user cancelled. Not an error: partial output has already been
    // abandoned, and the UI says "cancelled", not "failed" (section 11.3).
    kCancelled,

    // The input is not what the plan assumed. Probe misidentified it, or the
    // file changed between planning and conversion.
    kUnsupportedInput,

    // The input is the right format and is broken. This is the expected outcome
    // for hostile input (section 15) and must never be a crash.
    kMalformedInput,

    // A codec, a Store-delivered extension, or an OS component is missing. The
    // UI offers a deep link rather than an error (section 6.2).
    kMissingCapability,

    // The job would exceed its memory or size budget. Section 11.2 rejects these
    // at plan time where it can; this is the backstop for what it cannot.
    kResourceLimit,

    // Reading the source or committing the sink failed.
    kIoError,

    // Our bug. Distinct from kMalformedInput on purpose: this one is worth a
    // crash report, that one is a Tuesday.
    kInternal,
};

[[nodiscard]] constexpr std::string_view Name(Status s) noexcept {
    switch (s) {
        case Status::kOk:                 return "ok";
        case Status::kCancelled:          return "cancelled";
        case Status::kUnsupportedInput:   return "unsupported-input";
        case Status::kMalformedInput:     return "malformed-input";
        case Status::kMissingCapability:  return "missing-capability";
        case Status::kResourceLimit:      return "resource-limit";
        case Status::kIoError:            return "io-error";
        case Status::kInternal:           return "internal";
    }
    return "unknown";
}

// Something the conversion actually lost, as opposed to what the plan predicted
// it might. The planner publishes a worst case before the user commits
// (section 8.8); this is the after-the-fact truth reported in TaskDone, and the
// two are deliberately separate. A DOCX with no tracked changes converts to
// Markdown with a STRUCTURAL rating and zero notes.
struct FidelityNote {
    // Stable, greppable, and translatable: "dropped-tracked-changes",
    // "flattened-content-control", "rasterized-chart".
    std::string code;

    // Human-readable, specific, and countable where possible:
    // "3 content controls were replaced by their current text".
    std::string detail;

    // Where in the document, when the reader knows. Empty when it does not.
    std::string location;
};

class Result {
public:
    Result() = default;

    [[nodiscard]] static Result Ok() { return Result{Status::kOk, {}}; }

    [[nodiscard]] static Result Error(Status status, std::string message) {
        return Result{status, std::move(message)};
    }

    [[nodiscard]] static Result Cancelled() {
        return Result{Status::kCancelled, "Cancelled before the output was written."};
    }

    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] const std::vector<FidelityNote>& notes() const noexcept { return notes_; }

    [[nodiscard]] bool ok() const noexcept { return status_ == Status::kOk; }
    [[nodiscard]] bool cancelled() const noexcept { return status_ == Status::kCancelled; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    // The worst rating actually incurred, or Lossless when nothing was lost.
    [[nodiscard]] Fidelity incurred() const noexcept { return incurred_; }

    Result& Note(FidelityNote note) {
        notes_.push_back(std::move(note));
        return *this;
    }

    Result& Note(std::string code, std::string detail, std::string location = {}) {
        return Note(FidelityNote{std::move(code), std::move(detail), std::move(location)});
    }

    // Providers call this when they take a loss, so the reported rating reflects
    // what happened rather than what was predicted.
    Result& Incurred(Fidelity f) {
        incurred_ = Worst(incurred_, f);
        return *this;
    }

    // Folds a downstream hop's outcome into this one. Used by the multi-hop
    // executor: notes accumulate, the worst rating wins, and the first failure
    // short-circuits.
    Result& Absorb(Result other) {
        incurred_ = Worst(incurred_, other.incurred_);
        notes_.insert(notes_.end(), std::make_move_iterator(other.notes_.begin()),
                      std::make_move_iterator(other.notes_.end()));
        if (ok() && !other.ok()) {
            status_ = other.status_;
            message_ = std::move(other.message_);
        }
        return *this;
    }

private:
    Result(Status status, std::string message)
        : status_(status), message_(std::move(message)) {}

    Status status_ = Status::kOk;
    std::string message_;
    std::vector<FidelityNote> notes_;
    Fidelity incurred_ = Fidelity::Lossless;
};

}  // namespace morph
