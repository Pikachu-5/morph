// The shell extension as Explorer sees it (architecture sections 9.1 and 9.3).
//
// Everything else about the context menu is tested without COM, on purpose --
// menu.cpp is a function from a table and a selection to a list of entries, and
// that is where the decisions are. What no unit test can reach is the half that
// only exists once a package is registered: whether the CLSID in the manifest
// resolves, whether it resolves to a SURROGATE rather than to the caller, and
// whether the object on the other end is the one src/shellext implements.
//
// That half was unverifiable until M12, because it needs package identity, and
// "the shell extension has never been loaded by Explorer" was a line in the
// backlog for four milestones. This is the test that stops it being a line in
// the backlog.
//
// SKIPPED, not failed, when the package is not registered. Registering it puts
// a verb in the right-click menu of every file on the machine and is a thing a
// person chooses to do; a developer running the suite has not necessarily
// chosen it. See packaging/msix/AppxManifest.xml.in for the command.
//
// WHAT REGISTERING IT TAUGHT, written here because it cost an hour and looks
// like a bug in the menu: a PACKAGED surrogate and an UNPACKAGED broker do not
// share the capability table. The table is a named file mapping, package
// identity puts the surrogate in a different named-object namespace, and a
// broker started from a plain shell prompt publishes somewhere the surrogate
// cannot see. The menu then shows section 9.2's generic entry -- correctly, and
// for a reason that has nothing to do with the selection.
//
// This is not a defect in anything shipped: in a real install every process in
// section 3.1 has the same identity. It is a defect in the obvious way to test
// it by hand. To see a populated submenu, start the broker THROUGH the package:
//
//     Start-Process 'shell:AppsFolder\<PackageFamilyName>!Morph'
//
// which launches the app, which starts a broker with identity.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include "shellext/shellext.h"

using Microsoft::WRL::ComPtr;
using namespace morph::shellext;

namespace {

// COM for the duration of one test case. Apartment-threaded because that is
// what the surrogate declares and what Explorer uses.
struct Apartment {
    Apartment() { hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~Apartment() {
        if (SUCCEEDED(hr)) ::CoUninitialize();
    }
    HRESULT hr = E_FAIL;
};

// For INFO only. The titles are "To PDF" and the like, so UTF-8 is not in
// question; what is in question is that the iterator-range constructor narrows
// silently, and /W4 with warnings-as-errors is right to say so.
std::string Narrow(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t c : text) {
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

std::wstring CoTaskString(LPWSTR owned) {
    if (owned == nullptr) return {};
    std::wstring out(owned);
    ::CoTaskMemFree(owned);
    return out;
}

// A one-item selection over a real file, which is what the menu is handed.
// A path that does not exist gets no shell item, so the file is written first.
ComPtr<IShellItemArray> SelectionOf(const std::filesystem::path& path) {
    ComPtr<IShellItem> item;
    if (FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        return nullptr;
    }
    ComPtr<IShellItemArray> array;
    if (FAILED(::SHCreateShellItemArrayFromShellItem(item.Get(), IID_PPV_ARGS(&array)))) {
        return nullptr;
    }
    return array;
}

}  // namespace

TEST_CASE("The registered package activates the context menu out of process",
          "[shellext][registered][integration]") {
    const Apartment apartment;
    REQUIRE(SUCCEEDED(apartment.hr));

    ComPtr<IExplorerCommand> command;
    // CLSCTX_LOCAL_SERVER and not INPROC: section 9.1 puts the handler in a
    // surrogate so that a crash in the menu takes down a host process nobody
    // notices instead of the user's desktop. Asking for LOCAL_SERVER only means
    // a manifest that had quietly become an in-process class would fail here
    // rather than work and be wrong.
    const HRESULT created = ::CoCreateInstance(kMorphContextMenuClsid, nullptr,
                                               CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&command));
    if (created == REGDB_E_CLASSNOTREG) {
        SKIP("the Morph package is not registered; see packaging/msix/AppxManifest.xml.in");
    }
    INFO("CoCreateInstance returned 0x" << std::hex << created);
    REQUIRE(SUCCEEDED(created));
    REQUIRE(command);

    // It is our object and not merely something with the right class id.
    CHECK(CoTaskString([&] {
              LPWSTR title = nullptr;
              command->GetTitle(nullptr, &title);
              return title;
          }()) == L"Convert with Morph");

    GUID canonical{};
    REQUIRE(command->GetCanonicalName(&canonical) == S_OK);
    CHECK(::IsEqualGUID(canonical, kMorphContextMenuClsid));

    EXPCMDFLAGS flags = 0;
    REQUIRE(command->GetFlags(&flags) == S_OK);
    CHECK((flags & ECF_HASSUBCOMMANDS) != 0);
}

TEST_CASE("The registered menu answers for a real selection within its budget",
          "[shellext][registered][integration]") {
    const Apartment apartment;
    REQUIRE(SUCCEEDED(apartment.hr));

    ComPtr<IExplorerCommand> command;
    if (::CoCreateInstance(kMorphContextMenuClsid, nullptr, CLSCTX_LOCAL_SERVER,
                           IID_PPV_ARGS(&command)) == REGDB_E_CLASSNOTREG) {
        SKIP("the Morph package is not registered; see packaging/msix/AppxManifest.xml.in");
    }
    REQUIRE(command);

    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "morph-shellext-selection.md";
    {
        std::ofstream out(file);
        out << "# A file with an extension the menu knows\n";
    }

    const ComPtr<IShellItemArray> selection = SelectionOf(file);
    REQUIRE(selection);

    // Section 9.3: the state and the submenu are what Explorer blocks on, and
    // section 17 budgets 8 ms for them. Measured across the marshalling to the
    // surrogate, because that is what the user waits for -- and reported rather
    // than asserted, since a cold surrogate has to be created first and this
    // test is the thing that creates it.
    const auto start = std::chrono::steady_clock::now();

    EXPCMDSTATE state = 0;
    const HRESULT gotState = command->GetState(selection.Get(), FALSE, &state);
    REQUIRE(gotState == S_OK);
    // Hidden or enabled, never greyed out: section 9.2 would rather show
    // nothing than a disabled entry on every right-click.
    CHECK((state == ECS_ENABLED || state == ECS_HIDDEN));

    std::vector<std::wstring> titles;
    ComPtr<IEnumExplorerCommand> children;
    const HRESULT enumerated = command->EnumSubCommands(&children);
    // E_NOTIMPL is the documented answer for "no submenu", which is what the
    // root returns when no broker has ever published a capability table. That
    // is section 9.2's degraded path and a legitimate outcome here: this test
    // does not start a broker.
    if (enumerated == S_OK) {
        REQUIRE(children);
        ComPtr<IExplorerCommand> child;
        ULONG fetched = 0;
        while (children->Next(1, &child, &fetched) == S_OK && fetched == 1) {
            LPWSTR title = nullptr;
            if (SUCCEEDED(child->GetTitle(selection.Get(), &title))) {
                titles.push_back(CoTaskString(title));
            }
            child.Reset();
        }
    }

    const auto cold = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);

    // Again, with the surrogate already up. Section 17's 8 ms is a budget on
    // what Explorer blocks for, and Explorer creates a fresh command object per
    // menu but not a fresh surrogate -- so the second number is the one a user
    // pays on every right-click after the first.
    const auto warmStart = std::chrono::steady_clock::now();
    EXPCMDSTATE again = 0;
    CHECK(command->GetState(selection.Get(), FALSE, &again) == S_OK);
    ComPtr<IEnumExplorerCommand> warmChildren;
    (void)command->EnumSubCommands(&warmChildren);
    const auto warm = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - warmStart);

    // Reported, not gated. This is a debug build talking across a process
    // boundary, and section 16's ten percent gate belongs to morph.bench where
    // the configuration is the one the budget was written for. What is asserted
    // here is only that nothing has become catastrophically slow.
    WARN("GetState + EnumSubCommands: " << cold.count() << " us cold, " << warm.count()
                                        << " us warm, across the surrogate (" << titles.size()
                                        << " targets)");
    CHECK(warm.count() < 1'000'000);

    for (const std::wstring& title : titles) {
        // Every entry the menu offers is one of ours, and none of them is the
        // internal graph node the broker used to publish (backlog C1).
        INFO(Narrow(title));
        CHECK(title.rfind(L"To ", 0) == 0);
        CHECK(title.find(L"MORPH-IR") == std::wstring::npos);
    }

    std::error_code ec;
    std::filesystem::remove(file, ec);
}
