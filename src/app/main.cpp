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
// Section 18 says the WinUI project "needs MSBuild". It does not, and that is
// now demonstrated rather than argued -- the second correction to the document
// this repository carries, after the planner's Dijkstra sweep.
//
// WHAT IS IN THIS FILE is layout and marshalling, deliberately and only. Every
// decision the app makes belongs to AppModel (model.h), which is a library with
// no window and no IO, because that is the half that can be wrong in ways a
// person clicking buttons will not notice. This file owns the four things the
// model cannot: the dispatcher, the background threads, the Win32 details
// section 12.1 is specific about, and the virtualisation section 12.2 asks for.
#include <windows.h>
#undef GetCurrentTime

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl_core.h>

#include <MddBootstrap.h>
#include <microsoft.ui.xaml.window.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
// RangeBase and ButtonBase live here, and ProgressBar::Value and Button::Click
// are inherited from them -- so without this the projection compiles but every
// use of either is "a function that returns auto used before it is defined".
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>

#include "app/client.h"
#include "app/model.h"
#include "morph/core/stream.h"
#include "providers.h"

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace controls = winrt::Microsoft::UI::Xaml::Controls;
namespace windowing = winrt::Microsoft::UI::Windowing;
namespace foundation = winrt::Windows::Foundation;

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

// The badge, in the four words the shell extension's menu uses for the same job.
// The sentence behind it is Explain(); this is what fits on a button.
std::wstring BadgeFor(const TargetChoice& choice) {
    std::wstring badge;
    switch (choice.fidelity) {
        case Fidelity::Lossless:   badge = L"lossless"; break;
        case Fidelity::High:       badge = L"re-encoded once"; break;
        case Fidelity::Structural: badge = L"layout not kept"; break;
        case Fidelity::TextOnly:   badge = L"text only"; break;
        case Fidelity::Rasterized: badge = L"becomes an image"; break;
    }
    // "the hop count if the path is indirect" -- and only then, because "1 hop"
    // on every other button teaches nobody anything.
    if (choice.indirect()) {
        badge += L" · " + std::to_wstring(choice.hopCount) + L" steps";
    }
    return badge;
}

std::wstring FileNameOf(const std::string& path) {
    return std::filesystem::path(path).filename().wstring();
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
    explicit FileProber(std::shared_ptr<worker::Providers> providers)
        : providers_(std::move(providers)) {}

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

private:
    std::shared_ptr<worker::Providers> providers_;
};

class Shell;

// --- The queue's rows --------------------------------------------------------
//
// Section 12.2 asks for a VIRTUALISED ItemsRepeater, because "a batch can be
// thousands of files". Virtualisation normally arrives with a DataTemplate,
// which is markup -- so the template is an IElementFactory instead, which is
// what a DataTemplate compiles down to anyway. ItemsRepeater realises only the
// rows on screen and recycles the rest, exactly as it would with markup.
//
// There are no bindings either. A realised row is found by index with
// TryGetElement and updated in place; a row that is not realised is populated
// correctly the moment it scrolls into view, from the model, which is the only
// copy of the truth.
struct RowFactory : winrt::implements<RowFactory, xaml::IElementFactory> {
    explicit RowFactory(Shell* shell) : shell_(shell) {}

    xaml::UIElement GetElement(xaml::ElementFactoryGetArgs const& args);
    void RecycleElement(xaml::ElementFactoryRecycleArgs const& args);

private:
    Shell* shell_;
};

// --- The window --------------------------------------------------------------

class Shell {
public:
    // Files named on the command line, which arrive the same way dropped ones
    // do. Section 9.2's degraded path is the reason this exists: when the
    // capability map is absent the shell extension shows one static "Convert
    // with Morph…" that launches the app, and launching it onto an empty drop
    // zone would lose the selection the user had already made.
    void Open(std::vector<std::string> paths) { pending_ = std::move(paths); }

    void Start() {
        providers_ = std::make_shared<worker::Providers>();
        model_ = std::make_unique<AppModel>(FileProber{providers_});

        // A XAML exception raised on the dispatcher after startup reaches no
        // try/catch anywhere -- the process simply exits, with no window and
        // nothing written down, which is a genuinely hard thing to debug from
        // the outside. Saying what it was costs one message box.
        xaml::Application::Current().UnhandledException(
            [](foundation::IInspectable const&,
               xaml::UnhandledExceptionEventArgs const& args) {
                ::MessageBoxW(nullptr, args.Message().c_str(), L"Morph stopped",
                              MB_ICONERROR | MB_OK);
            });

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

        // The HWND, for the one thing section 12.2 asks for that WinUI has no
        // API of its own for: taskbar progress.
        if (const auto native = window_.try_as<::IWindowNative>()) {
            native->get_WindowHandle(&hwnd_);
        }
        (void)::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(taskbar_.put()));

        BuildLayout();
        window_.Content(root_);
        window_.Activate();

        // Connecting starts a broker, which starts a worker, which describes the
        // graph. None of that belongs on the thread painting the window.
        std::thread([this] { Connect(); }).detach();
    }

    // Probes whatever the command line named, once there is a broker to ask
    // about it. Called from Connect rather than from Start, because the picker
    // has nothing to show until the broker has answered.
    void OpenPending() {
        if (pending_.empty()) return;
        Probe(std::exchange(pending_, {}));
    }

    // Called back from RowFactory, which is why it is public and why the factory
    // holds a raw pointer: both live exactly as long as the window does.
    [[nodiscard]] const AppModel& model() const noexcept { return *model_; }
    void PopulateRow(xaml::UIElement const& element, int index) const;
    void CancelRow(int index);

private:
    // --- Layout --------------------------------------------------------------

    void BuildLayout() {
        root_ = controls::Grid{};
        // "The entire window is a drop target." Not a rectangle inside it, and
        // not a button -- a Grid with no background does not receive pointer
        // events at all, so it gets a transparent one.
        root_.Background(xaml::Media::SolidColorBrush{winrt::Windows::UI::Colors::Transparent()});
        root_.AllowDrop(true);
        root_.DragOver({this, &Shell::OnDragOver});
        root_.Drop({this, &Shell::OnDrop});

        BuildChooser();
        BuildQueue();

        root_.Children().Append(chooser_);
        root_.Children().Append(queue_);
        ShowScreen();
    }

    // The drop zone and the target picker are one panel, because they are one
    // screen with more on it: "on drop ... move straight to target selection".
    void BuildChooser() {
        heading_ = controls::TextBlock{};
        heading_.Text(L"Drop files here");
        heading_.FontSize(28);
        heading_.HorizontalAlignment(xaml::HorizontalAlignment::Center);
        heading_.TextAlignment(xaml::TextAlignment::Center);

        subheading_ = controls::TextBlock{};
        subheading_.Text(L"Connecting to the conversion engine…");
        subheading_.Opacity(0.7);
        subheading_.HorizontalAlignment(xaml::HorizontalAlignment::Center);
        subheading_.TextAlignment(xaml::TextAlignment::Center);
        subheading_.TextWrapping(xaml::TextWrapping::Wrap);
        subheading_.MaxWidth(560);

        // One StackPanel per family, filled in by ShowTargets. Grouping is not a
        // second data structure: the model sorts targets so that a change of
        // family IS a group boundary, and this walks the list once.
        groups_ = controls::StackPanel{};
        groups_.Spacing(16);
        groups_.HorizontalAlignment(xaml::HorizontalAlignment::Center);

        chooser_ = controls::StackPanel{};
        chooser_.Spacing(14);
        chooser_.VerticalAlignment(xaml::VerticalAlignment::Center);
        chooser_.Margin(xaml::ThicknessHelper::FromLengths(48, 48, 48, 48));
        chooser_.Children().Append(heading_);
        chooser_.Children().Append(subheading_);
        chooser_.Children().Append(groups_);
    }

    void BuildQueue() {
        queueHeading_ = controls::TextBlock{};
        queueHeading_.FontSize(20);
        queueHeading_.Margin(xaml::ThicknessHelper::FromLengths(0, 0, 0, 4));

        overall_ = controls::ProgressBar{};
        overall_.Minimum(0);
        overall_.Maximum(1);
        overall_.Margin(xaml::ThicknessHelper::FromLengths(0, 0, 0, 12));

        rows_ = winrt::single_threaded_observable_vector<foundation::IInspectable>();
        repeater_ = controls::ItemsRepeater{};
        repeater_.ItemsSource(rows_);
        repeater_.ItemTemplate(winrt::make<RowFactory>(this));
        controls::StackLayout layout;
        layout.Spacing(4);
        repeater_.Layout(layout);

        controls::ScrollViewer scroller;
        scroller.Content(repeater_);
        scroller.VerticalScrollBarVisibility(controls::ScrollBarVisibility::Auto);
        controls::Grid::SetRow(scroller, 2);

        stopAll_ = controls::Button{};
        stopAll_.Content(winrt::box_value(L"Stop"));
        stopAll_.Click({this, &Shell::OnStopAll});

        openFolder_ = controls::Button{};
        openFolder_.Content(winrt::box_value(L"Open folder"));
        openFolder_.Click({this, &Shell::OnOpenFolder});

        convertMore_ = controls::Button{};
        convertMore_.Content(winrt::box_value(L"Convert more"));
        convertMore_.Click({this, &Shell::OnConvertMore});

        controls::StackPanel footer;
        footer.Orientation(controls::Orientation::Horizontal);
        footer.Spacing(8);
        footer.Margin(xaml::ThicknessHelper::FromLengths(0, 12, 0, 0));
        footer.Children().Append(stopAll_);
        footer.Children().Append(openFolder_);
        footer.Children().Append(convertMore_);
        controls::Grid::SetRow(footer, 3);

        controls::Grid::SetRow(queueHeading_, 0);
        controls::Grid::SetRow(overall_, 1);

        queue_ = controls::Grid{};
        queue_.Margin(xaml::ThicknessHelper::FromLengths(32, 48, 32, 24));
        for (const auto height : {xaml::GridUnitType::Auto, xaml::GridUnitType::Auto,
                                  xaml::GridUnitType::Star, xaml::GridUnitType::Auto}) {
            controls::RowDefinition definition;
            definition.Height(xaml::GridLengthHelper::FromValueAndType(1, height));
            queue_.RowDefinitions().Append(definition);
        }
        queue_.Children().Append(queueHeading_);
        queue_.Children().Append(overall_);
        queue_.Children().Append(scroller);
        queue_.Children().Append(footer);
    }

    // --- Screens -------------------------------------------------------------

    void ShowScreen() {
        const Screen screen = model_->screen();
        const bool queueing = screen == Screen::kQueue || screen == Screen::kDone;
        chooser_.Visibility(queueing ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);
        queue_.Visibility(queueing ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);

        // A job that is over cannot be stopped, and one that is running has
        // nothing to open yet.
        const bool done = screen == Screen::kDone;
        stopAll_.Visibility(done ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);
        openFolder_.Visibility(done ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
        convertMore_.Visibility(done ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    }

    void ShowChooser() {
        const std::size_t count = model_->inputs().size();
        heading_.Text(count == 0 ? std::wstring(L"Drop files here")
                                 : Widen(std::to_string(count) + " file" +
                                         (count == 1 ? "" : "s")));

        if (count == 0) {
            subheading_.Text(ready_ ? L"Ready." : Widen(readyMessage_));
        } else if (!model_->targetsError().empty()) {
            subheading_.Text(Widen(model_->targetsError()));
        } else if (model_->targets().empty()) {
            // Distinguishable from "we never found out", which is the branch
            // above: SetTargetsUnavailable exists so these two cannot be
            // confused for one another.
            subheading_.Text(L"There is nothing all of these files can become.");
        } else {
            subheading_.Text(L"Convert to");
        }

        // A file nothing recognised is named with its reason rather than
        // silently dropped from the count.
        for (const InputFile& file : model_->inputs()) {
            if (file.usable()) continue;
            subheading_.Text(subheading_.Text() + L"\n" + Widen(file.error));
        }

        groups_.Children().Clear();
        std::string family;
        controls::StackPanel row{nullptr};
        for (const TargetChoice& choice : model_->targets()) {
            if (!row || choice.family != family) {
                family = choice.family;

                controls::TextBlock header;
                header.Text(Widen(family));
                header.Opacity(0.6);
                header.HorizontalAlignment(xaml::HorizontalAlignment::Center);

                row = controls::StackPanel{};
                row.Orientation(controls::Orientation::Horizontal);
                row.Spacing(8);
                row.HorizontalAlignment(xaml::HorizontalAlignment::Center);

                controls::StackPanel group;
                group.Spacing(6);
                group.Children().Append(header);
                group.Children().Append(row);
                groups_.Children().Append(group);
            }
            row.Children().Append(TargetButton(choice));
        }
    }

    [[nodiscard]] controls::Button TargetButton(const TargetChoice& choice) {
        controls::TextBlock label;
        label.Text(Widen(choice.label));
        label.HorizontalAlignment(xaml::HorizontalAlignment::Center);

        controls::TextBlock badge;
        badge.Text(BadgeFor(choice));
        badge.FontSize(11);
        badge.Opacity(0.65);
        badge.HorizontalAlignment(xaml::HorizontalAlignment::Center);

        controls::StackPanel content;
        content.Children().Append(label);
        content.Children().Append(badge);

        controls::Button button;
        button.Content(content);
        button.MinWidth(120);
        // "Clicking a badge explains, in one sentence, what will be lost."
        controls::ToolTipService::SetToolTip(
            button, winrt::box_value(Widen(std::string(choice.Explanation()))));
        // A button whose Content is a PANEL has no name to read: the label and
        // the badge are two TextBlocks, and a screen reader announces neither.
        // The format alone is the name; the badge is the description, which is
        // the same split the tooltip makes.
        xaml::Automation::AutomationProperties::SetName(button, Widen(choice.label));
        xaml::Automation::AutomationProperties::SetHelpText(
            button, Widen(std::string(choice.Explanation())));

        button.Tag(winrt::box_value(Widen(choice.profile)));
        button.Click({this, &Shell::OnChoose});
        return button;
    }

    void ShowQueue() {
        const std::size_t count = model_->rows().size();
        if (rows_.Size() != count) {
            rows_.Clear();
            for (std::size_t i = 0; i < count; ++i) {
                rows_.Append(winrt::box_value(static_cast<std::int32_t>(i)));
            }
        }
        RefreshRealisedRows();
        UpdateTotals();
    }

    // Only the rows on screen exist as elements. The rest are populated when
    // they scroll into view, from the model, which never stopped being right.
    void RefreshRealisedRows() {
        for (std::size_t i = 0; i < model_->rows().size(); ++i) {
            if (const auto element = repeater_.TryGetElement(static_cast<std::uint32_t>(i))) {
                PopulateRow(element, static_cast<int>(i));
            }
        }
    }

    void RefreshRow(std::size_t index) {
        if (index >= model_->rows().size()) return;
        if (const auto element = repeater_.TryGetElement(static_cast<std::uint32_t>(index))) {
            PopulateRow(element, static_cast<int>(index));
        }
        UpdateTotals();
    }

    void UpdateTotals() {
        const double fraction = model_->aggregateFraction();
        overall_.Value(fraction);

        if (model_->screen() == Screen::kDone) {
            std::string summary = std::to_string(model_->succeeded()) + " converted";
            if (model_->failed() > 0) summary += ", " + std::to_string(model_->failed()) + " failed";
            if (model_->cancelled() > 0) {
                summary += ", " + std::to_string(model_->cancelled()) + " cancelled";
            }
            queueHeading_.Text(Widen(summary));
            window_.Title(L"Morph");
        } else {
            const int percent = static_cast<int>(fraction * 100.0 + 0.5);
            queueHeading_.Text(Widen(std::to_string(model_->succeeded() + model_->failed() +
                                                    model_->cancelled()) +
                                     " of " + std::to_string(model_->rows().size()) + " done"));
            // "aggregate progress in the title bar and on the taskbar"
            window_.Title(Widen(std::to_string(percent) + "% · Morph"));
        }

        if (taskbar_ && hwnd_ != nullptr) {
            if (model_->screen() == Screen::kDone) {
                (void)taskbar_->SetProgressState(hwnd_, TBPF_NOPROGRESS);
            } else {
                (void)taskbar_->SetProgressState(hwnd_, TBPF_NORMAL);
                (void)taskbar_->SetProgressValue(hwnd_, static_cast<ULONGLONG>(fraction * 1000),
                                                 1000);
            }
        }
    }

    // --- The broker ----------------------------------------------------------

    void Connect() {
        const Result connected = client_.Connect();
        Post([this, connected] {
            ready_ = connected.ok();
            // Never "connection failed": section 12.2 wants the reason, and
            // BrokerClient already carries one worth reading.
            readyMessage_ = connected.ok() ? "Ready." : connected.message();
            if (model_->screen() == Screen::kDropZone) ShowChooser();
            OpenPending();
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

    void OnChoose(foundation::IInspectable const& sender, xaml::RoutedEventArgs const&) {
        const auto button = sender.as<controls::Button>();
        const std::string profile = Narrow(winrt::unbox_value<winrt::hstring>(button.Tag()));

        const ipc::SubmitJob job = model_->BuildJob(profile);
        // BuildJob returns an empty job for a target the picker never offered,
        // and submitting one would convert files the user did not ask about.
        if (job.inputPaths.empty()) return;

        model_->Begin(job);
        ShowScreen();
        ShowQueue();

        // RunJob blocks until the broker says the job is over. The Cancel that
        // stops it is written from the UI thread while this one reads, which is
        // what the duplex pipe was built for.
        std::thread([this, job] { Run(job); }).detach();
    }

    void Run(const ipc::SubmitJob& job) {
        JobEvents events;
        events.accepted = [this](const ipc::JobAccepted& accepted) {
            Post([this, accepted] {
                model_->OnAccepted(accepted);
                UpdateTotals();
            });
        };
        events.progress = [this](const ipc::Progress& progress) {
            Post([this, progress] {
                model_->OnProgress(progress);
                RefreshRow(progress.taskIndex);
            });
        };
        events.taskDone = [this](const ipc::TaskDone& done) {
            Post([this, done] {
                model_->OnTaskDone(done);
                RefreshRow(done.taskIndex);
            });
        };
        events.jobDone = [this](const ipc::JobDone& done) {
            Post([this, done] {
                model_->OnJobDone(done);
                ShowScreen();
                ShowQueue();
            });
        };

        const Result finished = client_.RunJob(job, events);
        if (finished.ok()) return;

        // A failure of the CONNECTION rather than of a file. The model turns it
        // into a reason on every row still running, so the queue does not sit at
        // sixty percent forever.
        Post([this, finished] {
            ipc::Error error;
            error.status = static_cast<std::uint8_t>(finished.status());
            error.message = finished.message();
            model_->OnError(error);
            ShowScreen();
            ShowQueue();
        });
    }

    void OnStopAll(foundation::IInspectable const&, xaml::RoutedEventArgs const&) {
        model_->MarkCancellingAll();
        RefreshRealisedRows();
        if (const std::uint64_t job = model_->jobId(); job != 0) {
            (void)client_.Cancel(job);
        }
    }

    void OnOpenFolder(foundation::IInspectable const&, xaml::RoutedEventArgs const&) {
        const std::string directory = model_->outputDirectory();
        if (directory.empty()) return;
        const std::wstring wide = Widen(directory);
        (void)::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void OnConvertMore(foundation::IInspectable const&, xaml::RoutedEventArgs const&) {
        model_->ConvertMore();
        rows_.Clear();
        if (taskbar_ && hwnd_ != nullptr) {
            (void)taskbar_->SetProgressState(hwnd_, TBPF_NOPROGRESS);
        }
        window_.Title(L"Morph");
        ShowScreen();
        ShowChooser();
    }

    // --- Dropping ------------------------------------------------------------

    void OnDragOver(foundation::IInspectable const&, xaml::DragEventArgs const& args) {
        args.AcceptedOperation(
            winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
        args.DragUIOverride().Caption(L"Convert with Morph");
    }

    winrt::fire_and_forget OnDrop(foundation::IInspectable const&,
                                  xaml::DragEventArgs const& args) {
        const auto self = this;
        const auto deferral = args.GetDeferral();
        std::vector<std::string> paths;

        if (args.DataView().Contains(winrt::Windows::ApplicationModel::DataTransfer::
                                         StandardDataFormats::StorageItems())) {
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

    // Probing reads bytes and asking the broker is a round trip, so neither
    // belongs on the thread painting the window -- however cheap section 12.2
    // says probing is, fifty files is fifty opens.
    void Probe(std::vector<std::string> paths) {
        heading_.Text(L"Reading…");
        subheading_.Text(L"");
        groups_.Children().Clear();

        std::thread([this, paths = std::move(paths)]() mutable {
            // The model belongs to the UI thread, so the probing is done against
            // a staging copy here and the answers are applied there.
            AppModel staging{FileProber{providers_}};
            staging.Drop(paths);

            ipc::Targets targets;
            Result asked = Result::Ok();
            const ipc::QueryTargets query = staging.TargetQuery();
            if (!query.sourceProfiles.empty()) {
                asked = client_.QueryTargets(query.sourceProfiles, query.largestInputBytes,
                                             targets);
            }

            Post([this, paths = std::move(paths), targets = std::move(targets),
                  asked]() mutable { Show(paths, targets, asked); });
        }).detach();
    }

    void Show(const std::vector<std::string>& paths, const ipc::Targets& targets,
              const Result& asked) {
        model_->Drop(paths);
        if (asked.ok()) {
            model_->SetTargets(targets);
        } else {
            model_->SetTargetsUnavailable(asked.message());
        }
        ShowScreen();
        ShowChooser();
    }

    xaml::Window window_{nullptr};
    controls::Grid root_{nullptr};

    controls::StackPanel chooser_{nullptr};
    controls::TextBlock heading_{nullptr};
    controls::TextBlock subheading_{nullptr};
    controls::StackPanel groups_{nullptr};

    controls::Grid queue_{nullptr};
    controls::TextBlock queueHeading_{nullptr};
    controls::ProgressBar overall_{nullptr};
    controls::ItemsRepeater repeater_{nullptr};
    foundation::Collections::IObservableVector<foundation::IInspectable> rows_{nullptr};
    controls::Button stopAll_{nullptr};
    controls::Button openFolder_{nullptr};
    controls::Button convertMore_{nullptr};

    winrt::com_ptr<::ITaskbarList3> taskbar_;
    HWND hwnd_ = nullptr;

    std::shared_ptr<worker::Providers> providers_;
    std::unique_ptr<AppModel> model_;
    BrokerClient client_;

    bool ready_ = false;
    std::string readyMessage_ = "Connecting to the conversion engine…";
    std::vector<std::string> pending_;
};

// The one piece of process-wide state, because Application::Start's callback
// takes no arguments and the App it makes cannot be handed any.
std::vector<std::string> g_openWith;

// --- RowFactory, now that Shell is a complete type ---------------------------

xaml::UIElement RowFactory::GetElement(xaml::ElementFactoryGetArgs const& args) {
    controls::TextBlock name;
    name.TextTrimming(xaml::TextTrimming::CharacterEllipsis);
    name.VerticalAlignment(xaml::VerticalAlignment::Center);
    controls::Grid::SetColumn(name, 0);

    controls::ProgressBar bar;
    bar.Minimum(0);
    bar.Maximum(1);
    bar.Width(160);
    bar.VerticalAlignment(xaml::VerticalAlignment::Center);
    controls::Grid::SetColumn(bar, 1);

    controls::TextBlock state;
    state.Opacity(0.7);
    state.MinWidth(90);
    state.VerticalAlignment(xaml::VerticalAlignment::Center);
    controls::Grid::SetColumn(state, 2);

    // Per-row cancel, which section 12.2 asks for by name.
    controls::Button stop;
    stop.Content(winrt::box_value(L"Stop"));
    controls::Grid::SetColumn(stop, 3);

    controls::Grid row;
    row.ColumnSpacing(12);
    for (const auto width : {xaml::GridUnitType::Star, xaml::GridUnitType::Auto,
                             xaml::GridUnitType::Auto, xaml::GridUnitType::Auto}) {
        controls::ColumnDefinition definition;
        definition.Width(xaml::GridLengthHelper::FromValueAndType(1, width));
        row.ColumnDefinitions().Append(definition);
    }
    row.Children().Append(name);
    row.Children().Append(bar);
    row.Children().Append(state);
    row.Children().Append(stop);

    Shell* const shell = shell_;
    stop.Click([shell, row](foundation::IInspectable const&, xaml::RoutedEventArgs const&) {
        // The row carries its own index, so a recycled element cancels the file
        // it is currently showing rather than the one it was made for.
        shell->CancelRow(winrt::unbox_value<std::int32_t>(row.Tag()));
    });

    const auto index = winrt::unbox_value<std::int32_t>(args.Data());
    row.Tag(winrt::box_value(index));
    shell_->PopulateRow(row, index);
    return row;
}

void RowFactory::RecycleElement(xaml::ElementFactoryRecycleArgs const&) {
    // Nothing to release: a row owns no resources and is repopulated by index
    // the next time it is handed out.
}

void Shell::PopulateRow(xaml::UIElement const& element, int index) const {
    const auto row = element.as<controls::Grid>();
    row.Tag(winrt::box_value(index));

    const QueueRow& data = model_->rows()[static_cast<std::size_t>(index)];
    row.Children().GetAt(0).as<controls::TextBlock>().Text(FileNameOf(data.inputPath));
    row.Children().GetAt(1).as<controls::ProgressBar>().Value(data.fraction);

    std::wstring state;
    switch (data.state) {
        case RowState::kPending:   state = data.cancelling ? L"stopping" : L"waiting"; break;
        case RowState::kRunning:   state = data.cancelling ? L"stopping" : L"converting"; break;
        case RowState::kSucceeded: state = L"done"; break;
        // Section 11.3: cancelled and failed are different things, and users are
        // the ones who know it.
        case RowState::kCancelled: state = L"cancelled"; break;
        case RowState::kFailed:    state = L"failed"; break;
    }
    row.Children().GetAt(2).as<controls::TextBlock>().Text(state);

    // "Failures listed with a real reason, never 'conversion failed'." The row
    // is one line, so the reason is the tooltip and the notes go with it.
    std::wstring detail = Widen(data.inputPath);
    if (!data.error.empty()) detail = Widen(data.error);
    for (const FidelityNote& note : data.notes) {
        detail += L"\n" + Widen(note.detail);
    }
    controls::ToolTipService::SetToolTip(row, winrt::box_value(detail));

    const auto stop = row.Children().GetAt(3).as<controls::Button>();
    stop.IsEnabled(!data.finished() && !data.cancelling);
    stop.Visibility(data.finished() ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);
}

void Shell::CancelRow(int index) {
    model_->MarkCancelling(static_cast<std::size_t>(index));
    RefreshRow(static_cast<std::size_t>(index));
    if (const std::uint64_t job = model_->jobId(); job != 0) {
        (void)client_.Cancel(job, index);
    }
}

// THE TWO THINGS A CODE-ONLY WinUI APP HAS TO DO ITSELF, both of which a
// generated project gets from App.xaml and nobody ever thinks about.
//
// FIRST, a real IXamlMetadataProvider. It is how the XAML parser resolves type
// names, and WinUI's own theme dictionaries are parsed through it -- so the
// framework's provider has to be reachable from the app object. Implementing
// the interface but answering nullptr is WORSE than not implementing it: it
// tells the parser that no type exists, including the framework's own, and
// XamlControlsResources fails to construct with "Catastrophic failure".
// Delegating to XamlControlsXamlMetaDataProvider is the whole job.
//
// SECOND, the merge belongs in OnLaunched and NOT in the constructor. The
// application has to be fully constructed, WITH its metadata provider attached,
// before it can load a resource dictionary; doing it a few lines earlier fails
// for a reason that has nothing to do with resources.
//
// Neither half works without the other, which is what made this expensive: the
// dictionary in the constructor fails with a provider, and a nullptr provider
// fails wherever the dictionary goes.
struct App : xaml::ApplicationT<App, winrt::Microsoft::UI::Xaml::Markup::IXamlMetadataProvider> {
    winrt::Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(
        winrt::Windows::UI::Xaml::Interop::TypeName const& type) {
        return provider_.GetXamlType(type);
    }
    winrt::Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(winrt::hstring const& name) {
        return provider_.GetXamlType(name);
    }
    winrt::com_array<winrt::Microsoft::UI::Xaml::Markup::XmlnsDefinition> GetXmlnsDefinitions() {
        return provider_.GetXmlnsDefinitions();
    }

    void OnLaunched(xaml::LaunchActivatedEventArgs const&) try {
        // The theme dictionaries every templated control's default style reaches
        // into. Here, not in the constructor: see above.
        Resources().MergedDictionaries().Append(controls::XamlControlsResources{});

        shell_.Open(std::move(g_openWith));
        shell_.Start();
    } catch (winrt::hresult_error const& e) {
        ::MessageBoxW(nullptr, e.message().c_str(), L"Morph failed to start", MB_ICONERROR);
        throw;
    }

private:
    // WinUI's own metadata, for WinUI's own types. There are no custom XAML
    // types in this app, so there is nothing to add to it.
    winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider provider_;
    Shell shell_;
};

}  // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Every argument is a path. There are no switches and there will not be:
    // section 3's design rule makes the app and the shell extension PEERS, and
    // the place a capability belongs is the broker protocol and the worker's
    // CLI, not a flag only this executable understands.
    int count = 0;
    if (LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) g_openWith.push_back(Narrow(argv[i]));
        ::LocalFree(argv);
    }

    // Whether this process has package identity, which decides everything below.
    //
    // UNPACKAGED, the Windows App Runtime has to be found and added to the
    // package graph by hand before any WinUI type can be activated. PACKAGED,
    // the deployment stack has already done it from the manifest's
    // PackageDependency, and calling the bootstrapper is not merely unnecessary
    // -- it FAILS, and the app exits before it has run a line of its own code:
    // no window, no message, nothing written anywhere. That is an afternoon,
    // and it is why this check is here rather than an unconditional call.
    UINT32 length = 0;
    const bool packaged = ::GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;

    if (!packaged) {
        // The version comes from the build, not from a constant here, because it
        // has to be the same line the headers and the .winmd files came from
        // (winappsdk.cmake says why). Any servicing release of that line will
        // do, which is the versioning the framework package is designed around.
        constexpr UINT32 kMajorMinor = MORPH_WINAPPSDK_MAJORMINOR;
        PACKAGE_VERSION noMinimum{};
        if (FAILED(::MddBootstrapInitialize2(kMajorMinor, L"", noMinimum,
                                             MddBootstrapInitializeOptions_OnNoMatch_ShowUI))) {
            // The bootstrapper has already shown the user where to get the
            // runtime.
            return 1;
        }
    }

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    xaml::Application::Start([](auto&&) { winrt::make<App>(); });

    if (!packaged) ::MddBootstrapShutdown();
    return 0;
}
