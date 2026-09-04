#include "shellext.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <shlwapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include "menu.h"
#include "morph/core/caps.h"
#include "morph/core/ipc/pipe.h"
#include "morph/core/ipc/protocol.h"

namespace morph::shellext {
namespace {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

std::atomic<long> gObjects{0};

// The capability table, opened once and kept for the life of the surrogate.
//
// Opening it per menu would be a section-open and a mapping on the 8 ms path.
// Opening it once and never re-reading would show a stale menu after the broker
// republished. So: opened lazily, kept, and the generation checked on every use
// -- which is a single read of a mapped word.
class TableCache {
public:
    // Returns an invalid table when no broker has ever published. Section 9.2:
    // "If the map is absent -- broker never ran, or version mismatch -- the
    // extension shows a single static 'Convert with Morph...' entry."
    [[nodiscard]] CapabilityTable Table() {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!mapping_.Mapped()) {
            if (const Result opened = mapping_.OpenForRead(); !opened) return {};
        }
        return mapping_.Table();
    }

private:
    std::mutex mutex_;
    CapabilityMapping mapping_;
};

TableCache& Cache() {
    static TableCache cache;
    return cache;
}

std::wstring Widen(std::string_view text) {
    if (text.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          needed);
    return out;
}

std::string Narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0, nullptr,
                                             nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          needed, nullptr, nullptr);
    return out;
}

HRESULT CopyTitle(std::wstring_view text, LPWSTR* out) {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    auto* buffer = static_cast<LPWSTR>(::CoTaskMemAlloc(bytes));
    if (buffer == nullptr) return E_OUTOFMEMORY;
    std::memcpy(buffer, text.data(), text.size() * sizeof(wchar_t));
    buffer[text.size()] = L'\0';
    *out = buffer;
    return S_OK;
}

// The extensions in a selection, read WITHOUT touching a file.
//
// GetDisplayName(SIGDN_PARENTRELATIVEPARSING) is a string operation on a shell
// item that is already in memory. Section 9.3 forbids enumerating ten thousand
// items on the calling thread, so the count is capped: a hundred is far more
// than is needed to know what KINDS of file are selected, and the menu only ever
// asks that question.
std::vector<std::string> ExtensionsOf(IShellItemArray* items, DWORD limit) {
    std::vector<std::string> extensions;
    if (items == nullptr) return extensions;

    DWORD count = 0;
    if (FAILED(items->GetCount(&count)) || count == 0) return extensions;

    const DWORD look = count < limit ? count : limit;
    for (DWORD i = 0; i < look; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(items->GetItemAt(i, &item))) continue;

        // Folders have no extension and cannot be converted; one in the
        // selection makes the whole menu generic rather than wrong.
        SFGAOF attributes = 0;
        if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attributes)) &&
            (attributes & SFGAO_FOLDER) != 0) {
            return {};
        }

        LPWSTR name = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &name)) || name == nullptr) {
            continue;
        }
        const std::wstring_view text(name);
        const std::size_t dot = text.find_last_of(L'.');
        if (dot != std::wstring_view::npos) {
            extensions.push_back(Narrow(text.substr(dot + 1)));
        }
        ::CoTaskMemFree(name);
    }
    return extensions;
}

// Submits one job. Runs on a background thread, never on Explorer's.
void SubmitJob(std::vector<std::string> paths, std::string targetProfile) {
    if (paths.empty() || targetProfile.empty()) return;

    ipc::Connection connection;
    if (!ipc::ConnectToBroker(connection, ipc::PipeName(), 1000)) {
        // Section 10.3: the client that fails to connect is the one that starts
        // the broker. A second client racing us gets kResourceLimit, and the
        // right response to that is to connect to the winner.
        (void)ipc::StartBroker();
        if (!ipc::ConnectToBroker(connection, ipc::PipeName(), 5000)) return;
    }

    ipc::Hello hello;
    hello.kind = ipc::ClientKind::kShellExtension;
    hello.clientVersion = MORPH_VERSION;
    if (!connection.Send(hello)) return;

    ipc::HelloAck ack;
    if (!connection.ReceiveAs(ack)) return;

    ipc::SubmitJob submit;
    submit.inputPaths = std::move(paths);
    submit.targetProfile = std::move(targetProfile);
    // A person is waiting: section 11.2 gives this job the interactive CPU
    // weight rather than the background one.
    submit.interactive = true;
    // Beside each input, renamed on collision. The shell extension has no UI to
    // ask with, and section 10.2 makes rename the default precisely because it
    // never destroys anything.
    submit.output.directory.clear();
    submit.output.namingTemplate = "{name}.{ext}";
    submit.output.overwrite = ipc::OverwriteRule::kRename;
    if (!connection.Send(submit)) return;

    // The connection is closed here, and the job keeps running: the broker's
    // ClientLink is built for exactly this (a shell extension "can submit forty
    // files and exit"). Reading the JobAccepted first means a failure to plan is
    // not lost in a race with the close.
    ipc::JobAccepted accepted;
    (void)connection.ReceiveAs(accepted);
}

// Enumerates the selection off Explorer's thread and submits it.
//
// Section 9.3: "Do not enumerate ten thousand items on the calling thread: take
// the count, marshal the array into the worker ... and return immediately."
void SubmitSelection(IShellItemArray* items, std::string targetProfile) {
    IStream* marshalled = nullptr;
    if (FAILED(::CoMarshalInterThreadInterfaceInStream(IID_IShellItemArray, items,
                                                       &marshalled))) {
        return;
    }

    std::thread([marshalled, target = std::move(targetProfile)]() mutable {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        {
            ComPtr<IShellItemArray> array;
            // Releases the stream whether it succeeds or fails, which is what
            // the name promises and what makes the marshalling leak-free.
            if (SUCCEEDED(::CoGetInterfaceAndReleaseStream(marshalled, IID_PPV_ARGS(&array)))) {
                DWORD count = 0;
                if (SUCCEEDED(array->GetCount(&count))) {
                    std::vector<std::string> paths;
                    paths.reserve(count);
                    for (DWORD i = 0; i < count; ++i) {
                        ComPtr<IShellItem> item;
                        if (FAILED(array->GetItemAt(i, &item))) continue;
                        LPWSTR path = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) &&
                            path != nullptr) {
                            paths.push_back(Narrow(path));
                            ::CoTaskMemFree(path);
                        }
                    }
                    SubmitJob(std::move(paths), std::move(target));
                }
            }
        }
        if (SUCCEEDED(hr)) ::CoUninitialize();
    }).detach();
}

// --- The commands ------------------------------------------------------------

// One target: "To PDF". Invoking it submits the job.
class TargetCommand final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand> {
public:
    TargetCommand(std::wstring title, std::string profile)
        : title_(std::move(title)), profile_(std::move(profile)) {
        AddObject();
    }
    ~TargetCommand() override { ReleaseObject(); }

    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* name) override {
        return CopyTitle(title_, name);
    }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override {
        if (icon != nullptr) *icon = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* tip) override {
        if (tip != nullptr) *tip = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetCanonicalName(GUID* id) override {
        if (id != nullptr) *id = GUID_NULL;
        return S_OK;
    }
    IFACEMETHODIMP GetState(IShellItemArray*, BOOL, EXPCMDSTATE* state) override {
        if (state == nullptr) return E_POINTER;
        *state = ECS_ENABLED;
        return S_OK;
    }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        if (flags == nullptr) return E_POINTER;
        *flags = ECF_DEFAULT;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override {
        if (commands != nullptr) *commands = nullptr;
        return E_NOTIMPL;
    }

    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override {
        if (items == nullptr) return E_INVALIDARG;
        SubmitSelection(items, profile_);
        return S_OK;
    }

private:
    std::wstring title_;
    std::string profile_;
};

class CommandEnumerator final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IEnumExplorerCommand> {
public:
    explicit CommandEnumerator(std::vector<ComPtr<IExplorerCommand>> commands)
        : commands_(std::move(commands)) {
        AddObject();
    }
    ~CommandEnumerator() override { ReleaseObject(); }

    IFACEMETHODIMP Next(ULONG count, IExplorerCommand** out, ULONG* fetched) override {
        ULONG produced = 0;
        for (; produced < count && position_ < commands_.size(); ++produced, ++position_) {
            commands_[position_].CopyTo(&out[produced]);
        }
        if (fetched != nullptr) *fetched = produced;
        return produced == count ? S_OK : S_FALSE;
    }

    IFACEMETHODIMP Skip(ULONG count) override {
        position_ += count;
        if (position_ > commands_.size()) position_ = commands_.size();
        return S_OK;
    }

    IFACEMETHODIMP Reset() override {
        position_ = 0;
        return S_OK;
    }

    IFACEMETHODIMP Clone(IEnumExplorerCommand** out) override {
        if (out != nullptr) *out = nullptr;
        return E_NOTIMPL;
    }

private:
    std::vector<ComPtr<IExplorerCommand>> commands_;
    std::size_t position_ = 0;
};

// "Convert with Morph", with the targets underneath it.
class RootCommand final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand> {
public:
    RootCommand() { AddObject(); }
    ~RootCommand() override { ReleaseObject(); }

    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* name) override {
        return CopyTitle(L"Convert with Morph", name);
    }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override {
        if (icon != nullptr) *icon = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* tip) override {
        if (tip != nullptr) *tip = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetCanonicalName(GUID* id) override {
        if (id != nullptr) *id = kMorphContextMenuClsid;
        return S_OK;
    }

    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override {
        if (state == nullptr) return E_POINTER;
        // Hidden rather than greyed out when there is nothing to offer: a
        // disabled entry on every right-click is clutter, and Explorer's menu is
        // not ours to fill.
        *state = Build(items).generic && !HaveTable() ? ECS_HIDDEN : ECS_ENABLED;
        return S_OK;
    }

    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        if (flags == nullptr) return E_POINTER;
        *flags = ECF_HASSUBCOMMANDS;
        return S_OK;
    }

    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override {
        if (commands == nullptr) return E_POINTER;
        *commands = nullptr;

        std::vector<ComPtr<IExplorerCommand>> children;
        for (const MenuEntry& entry : lastMenu_.entries) {
            children.push_back(Make<TargetCommand>(Widen(entry.Title()), entry.profile));
        }
        if (children.empty()) return E_NOTIMPL;  // no submenu; the root is inert

        auto enumerator = Make<CommandEnumerator>(std::move(children));
        return enumerator.CopyTo(commands);
    }

    IFACEMETHODIMP Invoke(IShellItemArray*, IBindCtx*) override {
        // Only reachable when the submenu is empty -- section 9.2's static
        // entry. The app is not built yet, so this does the honest thing and
        // does nothing rather than pretending.
        return S_OK;
    }

private:
    [[nodiscard]] bool HaveTable() const noexcept { return haveTable_; }

    // GetState runs first and EnumSubCommands second, so the menu is built once
    // and kept. A command object is created per invocation of the menu by
    // Explorer, which is what makes that safe.
    const Menu& Build(IShellItemArray* items) {
        if (built_) return lastMenu_;
        built_ = true;

        const CapabilityTable table = Cache().Table();
        haveTable_ = table.Valid();
        lastMenu_ = BuildMenu(table, ExtensionsOf(items, 100));
        return lastMenu_;
    }

    Menu lastMenu_;
    bool built_ = false;
    bool haveTable_ = false;
};

}  // namespace

long LiveObjects() noexcept { return gObjects.load(); }
void AddObject() noexcept { gObjects.fetch_add(1); }
void ReleaseObject() noexcept { gObjects.fetch_sub(1); }

HRESULT CreateRootCommand(REFIID riid, void** out) {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    auto command = Make<RootCommand>();
    if (!command) return E_OUTOFMEMORY;
    return command.CopyTo(riid, out);
}

}  // namespace morph::shellext
