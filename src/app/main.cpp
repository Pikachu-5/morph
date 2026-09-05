// Morph.App.exe — the window (architecture section 12).
//
// THERE IS NO XAML MARKUP IN THIS PROJECT, and that is a decision rather than an
// omission. WinUI's XAML compiler is an MSBuild task, and adopting it would mean
// a second build system for one executable in a tree where section 18's argument
// for tools/build.ps1 is that CI drives the same script a developer does. The
// compiler is only needed for MARKUP; a UI built in code needs the C++/WinRT
// projection and nothing else, and CMake runs cppwinrt.exe like any other code
// generator (winappsdk.cmake). The cost is no designer and no XAML hot reload,
// which section 12.3 had already written off.
//
// WHAT IS IN THIS FILE is layout and marshalling, deliberately and only. Every
// decision the app makes belongs to AppModel (model.h), which is a library with
// no window and no IO, because that is the half that can be wrong in ways a
// person clicking buttons will not notice. This file owns three things the model
// cannot: the dispatcher, the two background threads, and the Win32 details
// section 12.1 is specific about.
#include <windows.h>
#undef GetCurrentTime

#include <shlobj.h>

#include <MddBootstrap.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

#include "app/client.h"
#include "app/model.h"
#include "morph/core/stream.h"
#include "providers.h"

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace controls = winrt::Microsoft::UI::Xaml::Controls;
namespace windowing = winrt::Microsoft::UI::Windowing;

using namespace morph;
using namespace morph::app;

namespace {

std::wstring Widen(std::string_view text) {
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                          size);
    return wide;
}

std::string Narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(),
                          size, nullptr, nullptr);
    return narrow;
}

// The real prober behind AppModel's injected one (section 12.2: "probe the files
// (cheap, 8 KiB each)").
//
// It runs the SAME provider table the worker converts with, so the app cannot
// offer a target for a file the worker will then refuse to read. Probing is not
// converting -- section 12.3's "the UI process does no conversion work" is about
// where the CPU goes, and this reads a few kilobytes and looks at them -- but
// the table is linked here all the same, which is the one thing about this
// process that is heavier than it looks. Moving it to a round trip to the
// worker, which already has every provider loaded, is a change to this function
// and to nothing else: that is what the Prober seam is for.
class FileProber {
public:
    ProbeAnswer operator()(const std::string& path) const {
        ProbeAnswer answer;
        const std::filesystem::path file(path);

        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        if (!ec) answer.sizeBytes = size;

        FileSource source(file);
        PathHint hint;
        hint.filename = file.filename().string();
        hint.extension = file.extension().string();
        for (char& c : hint.extension) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        if (const std::optional<MediaProfile> profile =
                providers_->executor().Probe(source, hint)) {
            answer.profile = profile->Id();
        } else {
            // Section 12.2 again: a real reason, and the name of the file it is
            // about, because a list of five rows needs to say which one.
            answer.error = "Morph does not recognise what is inside " + hint.filename + ".";
        }
        return answer;
    }

    explicit FileProber(std::shared_ptr<worker::Providers> providers)
        : providers_(std::move(providers)) {}

private:
    std::shared_ptr<worker::Providers> providers_;
};

// --- The window --------------------------------------------------------------

class Shell {
public:
    void Start() {
        providers_ = std::make_shared<worker::Providers>();
        model_ = std::make_unique<AppModel>(FileProber{providers_});

        window_ = xaml::Window{};
        window_.Title(L"Morph");

        // Section 12.1: Mica is one property on the Window, and BaseAlt is the
        // tabbed variant -- there is no tab strip here, so this is the plain one.
        window_.SystemBackdrop(xaml::Media::MicaBackdrop{});

        auto titleBar = window_.AppWindow().TitleBar();
        titleBar.ExtendsContentIntoTitleBar(true);
        titleBar.PreferredHeightOption(windowing::TitleBarHeightOption::Tall);
        titleBar.ButtonBackgroundColor(winrt::Windows::UI::Colors::Transparent());
        titleBar.ButtonInactiveBackgroundColor(winrt::Windows::UI::Colors::Transparent());

        BuildLayout();
        window_.Content(root_);
        window_.Activate();

        // Connecting starts a broker, which starts a worker, which describes the
        // graph. None of that belongs on the thread painting the window.
        std::thread([this] { Connect(); }).detach();
    }

private:
    void BuildLayout() {
        root_ = controls::Grid{};
        // "The entire window is a drop target." Not a rectangle inside it, and
        // not a button -- a Grid with no background does not receive pointer
        // events at all, so it gets a transparent one.
        root_.Background(xaml::Media::SolidColorBrush{winrt::Windows::UI::Colors::Transparent()});
        root_.AllowDrop(true);
        root_.DragOver({this, &Shell::OnDragOver});
        root_.Drop({this, &Shell::OnDrop});

        status_ = controls::TextBlock{};
        status_.Text(L"Drop files here");
        status_.FontSize(28);
        status_.HorizontalAlignment(xaml::HorizontalAlignment::Center);
        status_.TextAlignment(xaml::TextAlignment::Center);

        detail_ = controls::TextBlock{};
        detail_.Text(L"Connecting to the conversion engine…");
        detail_.Opacity(0.7);
        detail_.HorizontalAlignment(xaml::HorizontalAlignment::Center);
        detail_.TextAlignment(xaml::TextAlignment::Center);
        detail_.TextWrapping(xaml::TextWrapping::Wrap);

        // The picker's buttons, filled in when the broker answers. One row per
        // family in the model's order, which is already grouped (model.cpp).
        targets_ = controls::StackPanel{};
        targets_.Orientation(controls::Orientation::Horizontal);
        targets_.HorizontalAlignment(xaml::HorizontalAlignment::Center);
        targets_.Spacing(8);

        controls::StackPanel column;
        column.Spacing(12);
        column.VerticalAlignment(xaml::VerticalAlignment::Center);
        column.Margin(xaml::ThicknessHelper::FromLengths(48, 48, 48, 48));
        column.Children().Append(status_);
        column.Children().Append(detail_);
        column.Children().Append(targets_);

        root_.Children().Append(column);
    }

    void Connect() {
        const Result connected = client_.Connect();
        Post([this, connected] {
            if (connected.ok()) {
                detail_.Text(L"Ready.");
            } else {
                // Never "connection failed": section 12.2 wants the reason, and
                // BrokerClient already carries one worth reading.
                detail_.Text(Widen(connected.message()));
            }
        });
    }

    // Runs `work` on the UI thread. Every broker event arrives on a background
    // thread and the model is single-threaded by contract (model.h), so this is
    // the only way anything below touches it.
    template <typename F>
    void Post(F&& work) {
        window_.DispatcherQueue().TryEnqueue(
            winrt::Microsoft::UI::Dispatching::DispatcherQueueHandler(std::forward<F>(work)));
    }

    void OnDragOver(winrt::Windows::Foundation::IInspectable const&,
                    xaml::DragEventArgs const& args) {
        args.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::
                                   DataPackageOperation::Copy);
        args.DragUIOverride().Caption(L"Convert with Morph");
    }

    winrt::fire_and_forget OnDrop(winrt::Windows::Foundation::IInspectable const&,
                                  xaml::DragEventArgs const& args) {
        const auto self = this;
        const auto deferral = args.GetDeferral();
        std::vector<std::string> paths;

        if (args.DataView().Contains(
                winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::
                    StorageItems())) {
            const auto items = co_await args.DataView().GetStorageItemsAsync();
            for (auto const& item : items) {
                // A folder is not a conversion. Section 9.3 makes the same call
                // for the shell menu, and for the same reason: what a user means
                // by dropping one is a question with more than one answer.
                if (item.IsOfType(winrt::Windows::Storage::StorageItemTypes::File)) {
                    paths.push_back(Narrow(item.Path()));
                }
            }
        }
        deferral.Complete();
        if (paths.empty()) co_return;

        self->Probe(std::move(paths));
    }

    // Probing reads bytes, so it is not something to do on the thread painting
    // the window either, however cheap section 12.2 says it is: fifty files is
    // fifty opens.
    void Probe(std::vector<std::string> paths) {
        status_.Text(L"Reading…");
        std::thread([this, paths = std::move(paths)]() mutable {
            // The model is the UI thread's, so the probe answers are collected
            // here and applied there.
            AppModel staging{FileProber{providers_}};
            staging.Drop(paths);

            std::vector<InputFile> probed(staging.inputs().begin(), staging.inputs().end());
            ipc::Targets targets;
            const ipc::QueryTargets query = staging.TargetQuery();
            Result asked = Result::Ok();
            if (!query.sourceProfiles.empty()) {
                asked = client_.QueryTargets(query.sourceProfiles, query.largestInputBytes,
                                             targets);
            }

            Post([this, probed = std::move(probed), targets = std::move(targets),
                  asked]() mutable { Show(std::move(probed), std::move(targets), asked); });
        }).detach();
    }

    void Show(std::vector<InputFile> probed, ipc::Targets targets, const Result& asked) {
        std::vector<std::string> paths;
        paths.reserve(probed.size());
        for (const InputFile& file : probed) paths.push_back(file.path);

        const std::size_t usable = model_->Drop(paths);
        if (asked.ok()) {
            model_->SetTargets(targets);
        } else {
            model_->SetTargetsUnavailable(asked.message());
        }

        status_.Text(Widen(std::to_string(model_->inputs().size()) + " file" +
                           (model_->inputs().size() == 1 ? "" : "s")));

        if (usable == 0) {
            // The reason, not a count. A user who dropped one unreadable file
            // wants to know what was wrong with it.
            detail_.Text(Widen(model_->inputs().empty() ? std::string("Nothing was dropped.")
                                                        : model_->inputs().front().error));
        } else if (!model_->targetsError().empty()) {
            detail_.Text(Widen(model_->targetsError()));
        } else if (model_->targets().empty()) {
            detail_.Text(L"There is nothing all of these files can become.");
        } else {
            detail_.Text(L"Convert to");
        }

        targets_.Children().Clear();
        for (const TargetChoice& choice : model_->targets()) {
            controls::Button button;
            button.Content(winrt::box_value(Widen(choice.label)));
            // "Clicking a badge explains, in one sentence, what will be lost."
            // A tooltip is the cheapest honest version of that; the badge itself
            // is the next screen's work.
            controls::ToolTipService::SetToolTip(
                button, winrt::box_value(Widen(std::string(choice.Explanation()))));
            targets_.Children().Append(button);
        }
    }

    xaml::Window window_{nullptr};
    controls::Grid root_{nullptr};
    controls::TextBlock status_{nullptr};
    controls::TextBlock detail_{nullptr};
    controls::StackPanel targets_{nullptr};

    std::shared_ptr<worker::Providers> providers_;
    std::unique_ptr<AppModel> model_;
    BrokerClient client_;
};

struct App : xaml::ApplicationT<App> {
    void OnLaunched(xaml::LaunchActivatedEventArgs const&) { shell_.Start(); }

private:
    Shell shell_;
};

}  // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Unpackaged, so the Windows App Runtime has to be found and added to this
    // process's package graph before any WinUI type can be activated. Packaged
    // (the MSIX in section 13) this call is unnecessary and harmless.
    //
    // 2.x with no minimum: the app is built against the 2.0 headers and runs on
    // whatever 2.x servicing release the machine has, which is the versioning
    // the framework package is designed around.
    constexpr UINT32 kMajorMinor = 0x00020000;
    PACKAGE_VERSION noMinimum{};
    if (FAILED(::MddBootstrapInitialize2(kMajorMinor, L"", noMinimum,
                                         MddBootstrapInitializeOptions_OnNoMatch_ShowUI))) {
        // The bootstrapper has already shown the user where to get the runtime.
        return 1;
    }

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    xaml::Application::Start([](auto&&) { winrt::make<App>(); });

    ::MddBootstrapShutdown();
    return 0;
}
