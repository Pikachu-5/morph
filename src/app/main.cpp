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
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Graphics.h>
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
#include "morph/core/naming.h"
#include "morph/core/stream.h"

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

// The answers to section 12.2's "probe the files on drop", held between the
// round trip that fetched them and the two models that ask for them.
//
// THIS USED TO BE A PROBER THAT DID THE WORK. It linked the whole provider
// table into the window and read 8 KiB of every dropped file, which is cheap in
// CPU and expensive in everything else: WIC, Media Foundation and PDFium all
// resident in the UI process. Section 12.3's argument for the C# escape hatch
// is that "the UI process does no conversion work", and a process holding three
// codec stacks is doing conversion work in every sense that matters for crash
// surface, for memory, and for what a rewrite would have to reproduce.
//
// AppModel::Drop calls its Prober SYNCHRONOUSLY, and it runs on the UI thread,
// so the round trip cannot happen inside it. The table is filled on the
// background thread first and the Prober becomes a lookup -- which is what the
// injected seam was for, and why Drop never had to change.
class ProbeTable {
public:
    // One round trip's worth of answers, in the order they were asked for.
    void Learn(const std::vector<std::string>& paths, const ipc::Identified& identified) {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < paths.size(); ++i) {
            const std::string profile =
                i < identified.profiles.size() ? identified.profiles[i] : std::string();
            answers_[paths[i]] = profile;
        }
    }

    // Nothing could be asked. Recorded as its own state rather than as "not
    // recognised", because those are different sentences and a user who is told
    // the wrong one goes looking for the wrong problem.
    void Unavailable(const std::vector<std::string>& paths, std::string reason) {
        const std::lock_guard<std::mutex> lock(mutex_);
        unreachable_ = std::move(reason);
        for (const std::string& path : paths) answers_.erase(path);
    }

    [[nodiscard]] ProbeAnswer operator()(const std::string& path) const {
        ProbeAnswer answer;

        // The size is read here rather than fetched. It is a directory entry,
        // not a decode -- no provider is involved, and asking the broker for it
        // would be a round trip to learn something already on this machine.
        std::error_code ec;
        const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
        if (!ec) answer.sizeBytes = size;

        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = answers_.find(path);
        if (found != answers_.end() && !found->second.empty()) {
            answer.profile = found->second;
            return answer;
        }

        const std::string name = std::filesystem::path(path).filename().string();
        answer.error = found == answers_.end() && !unreachable_.empty()
                           ? "Morph could not reach the conversion engine: " + unreachable_
                           : "Morph does not recognise what is inside " + name + ".";
        return answer;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> answers_;
    std::string unreachable_;
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
        probes_ = std::make_shared<ProbeTable>();
        model_ = std::make_unique<AppModel>([probes = probes_](const std::string& path) {
            return (*probes)(path);
        });

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
        SetWindowTitle(L"Morph");

        // Section 12.1: Mica is one property on the Window, and BaseAlt is the
        // tabbed variant -- there is no tab strip here, so this is the plain one.
        window_.SystemBackdrop(xaml::Media::MicaBackdrop{});

        auto titleBar = window_.AppWindow().TitleBar();
        titleBar.ExtendsContentIntoTitleBar(true);
        titleBar.PreferredHeightOption(windowing::TitleBarHeightOption::Tall);
        titleBar.ButtonBackgroundColor(winrt::Windows::UI::Colors::Transparent());
        titleBar.ButtonInactiveBackgroundColor(winrt::Windows::UI::Colors::Transparent());
        // Extending content into the title bar takes the drag region away with
        // it. Section 12.1 is specific about what has to replace it, and
        // RefreshTitleBar is that; without it the strip across the top looks
        // like a title bar and cannot be used to move the window.

        // The HWND, for the one thing section 12.2 asks for that WinUI has no
        // API of its own for: taskbar progress.
        if (const auto native = window_.try_as<::IWindowNative>()) {
            native->get_WindowHandle(&hwnd_);
        }
        (void)::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(taskbar_.put()));

        BuildLayout();
        window_.Content(root_);

        // Loaded, not here: XamlRoot is null until the tree is in a window, and
        // RasterizationScale comes from XamlRoot. This is also the first time
        // the strip has a width to measure.
        root_.Loaded([this](foundation::IInspectable const&, xaml::RoutedEventArgs const&) {
            RefreshTitleBar();
            // A move to a monitor with a different scale changes the title bar
            // height, both insets and the DIP-to-pixel ratio at once. All three
            // are read again rather than scaled from the old ones.
            if (const auto xamlRoot = root_.XamlRoot()) {
                xamlRoot.Changed({this, &Shell::OnXamlRootChanged});
            }
        });

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

        BuildTitleBar();
        BuildChooser();
        BuildQueue();

        root_.Children().Append(chooser_);
        root_.Children().Append(queue_);
        // Last, so it is on top: the strip is the drag region and nothing below
        // it should take a click that belongs to moving the window.
        root_.Children().Append(titleBar_);
        ShowScreen();
    }

    // --- The title bar (section 12.1) ----------------------------------------
    //
    // "Set drag rectangles from the actual layout, not hardcoded -- they must be
    // recomputed on resize and on DPI change or the window becomes undraggable."
    //
    // FROM THE ACTUAL LAYOUT is the part that costs something. It would be
    // shorter to compute the rectangle from AppWindow::Size and the two insets,
    // and it would be wrong the first time anything is put in the title bar,
    // because a control inside a drag rectangle is dead: the system claims that
    // area as caption and XAML never sees the pointer. So the strip is a real
    // element, the drag region is a real element inside it, and the rectangle
    // is whatever that element's bounds turn out to be.
    //
    // TWO COORDINATE SYSTEMS meet here, which is the whole of the DPI problem.
    // AppWindowTitleBar's Height, LeftInset and RightInset are PHYSICAL pixels,
    // and so is SetDragRectangles. XAML lays out in DIPs. The conversion is the
    // XamlRoot's RasterizationScale, and every one of those numbers changes when
    // the window moves to a monitor with a different scale -- which is why this
    // runs again on XamlRoot::Changed and not only on resize.
    void BuildTitleBar() {
        // Not the window title. With the content extended into the title bar
        // the system draws no caption text at all, so section 12.2's "aggregate
        // progress in the title bar" was going to the taskbar and to alt-tab and
        // nowhere the user was looking. This is where it becomes visible.
        titleText_ = controls::TextBlock{};
        titleText_.Text(L"Morph");
        titleText_.FontSize(12);
        titleText_.Opacity(0.8);
        titleText_.VerticalAlignment(xaml::VerticalAlignment::Center);
        titleText_.Margin(xaml::ThicknessHelper::FromLengths(16, 0, 0, 0));
        // A TextBlock is safe inside a drag rectangle because it wants no input.
        // Anything that does -- a settings button, a search box -- has to sit
        // OUTSIDE dragRegion_, in its own column, or it will not respond.
        titleText_.IsHitTestVisible(false);

        dragRegion_ = controls::Grid{};
        dragRegion_.Children().Append(titleText_);
        // SizeChanged rather than the window's: this fires after the strip has
        // been laid out at its new size, so ActualWidth is the number we want
        // rather than the previous one.
        dragRegion_.SizeChanged({this, &Shell::OnDragRegionResized});

        titleBar_ = controls::Grid{};
        titleBar_.VerticalAlignment(xaml::VerticalAlignment::Top);
        titleBar_.Children().Append(dragRegion_);
    }

    // The strip's height and the room left for the caption buttons, in DIPs.
    void ApplyTitleBarMetrics() {
        if (!window_ || !titleBar_) return;
        const auto appWindow = window_.AppWindow();
        if (!appWindow) return;
        const auto bar = appWindow.TitleBar();

        const auto xamlRoot = root_.XamlRoot();
        const double scale = xamlRoot ? xamlRoot.RasterizationScale() : 1.0;
        if (scale <= 0.0) return;

        titleBar_.Height(static_cast<double>(bar.Height()) / scale);
        // The caption buttons are drawn by the system on top of our content, and
        // a drag rectangle underneath them would swallow their clicks. LeftInset
        // is zero in a left-to-right layout and is not assumed to be.
        dragRegion_.Margin(xaml::ThicknessHelper::FromLengths(
            static_cast<double>(bar.LeftInset()) / scale, 0,
            static_cast<double>(bar.RightInset()) / scale, 0));
    }

    void UpdateDragRectangles() {
        if (!window_ || !dragRegion_) return;
        const auto appWindow = window_.AppWindow();
        if (!appWindow) return;
        const auto bar = appWindow.TitleBar();
        if (!bar.ExtendsContentIntoTitleBar()) return;

        const auto xamlRoot = dragRegion_.XamlRoot();
        if (!xamlRoot) return;  // not in a tree yet; Loaded will come back here
        const double scale = xamlRoot.RasterizationScale();
        if (scale <= 0.0) return;

        const double width = dragRegion_.ActualWidth();
        const double height = dragRegion_.ActualHeight();
        // A zero-sized region is not "no drag region", it is a layout that has
        // not happened. Publishing an empty rectangle for it would leave the
        // window undraggable until the next resize.
        if (width <= 0.0 || height <= 0.0) return;

        const auto origin =
            dragRegion_.TransformToVisual(nullptr).TransformPoint(foundation::Point{0, 0});

        const winrt::Windows::Graphics::RectInt32 rectangle{
            static_cast<std::int32_t>(std::lround(origin.X * scale)),
            static_cast<std::int32_t>(std::lround(origin.Y * scale)),
            static_cast<std::int32_t>(std::lround(width * scale)),
            static_cast<std::int32_t>(std::lround(height * scale))};
        bar.SetDragRectangles({&rectangle, &rectangle + 1});
    }

    void RefreshTitleBar() {
        ApplyTitleBarMetrics();
        // Changing the height and the margins above schedules a layout pass;
        // the rectangle is taken from the result of it in OnDragRegionResized.
        // Doing it here as well covers the case where nothing actually moved and
        // no SizeChanged is raised.
        UpdateDragRectangles();
    }

    void OnDragRegionResized(foundation::IInspectable const&,
                             xaml::SizeChangedEventArgs const&) {
        UpdateDragRectangles();
    }

    void OnXamlRootChanged(xaml::XamlRoot const&, xaml::XamlRootChangedEventArgs const&) {
        RefreshTitleBar();
    }

    // --- Output policy handlers ----------------------------------------------

    // The preview is recomputed from the model rather than assembled here, so
    // that the line on screen and the name the broker resolves come from one
    // ApplyTemplate. See morph/core/naming.h for why that is not the broker's.
    void RefreshOutputPanel() {
        if (!outputPanel_) return;

        const ipc::OutputPolicy& policy = model_->output();
        destinationText_.Text(policy.directory.empty() ? L"Beside each original file"
                                                       : Widen(policy.directory));
        besideEachFile_.IsEnabled(!policy.directory.empty());

        // The example is the FIRST target offered, which is the best-fidelity
        // one under the first family heading. Any target would do -- the only
        // thing that changes between them is the extension -- and picking one
        // beats a line with "{ext}" still in it, which is the thing the user is
        // trying to find out the meaning of.
        const auto& targets = model_->targets();
        if (targets.empty()) {
            outputPanel_.Visibility(xaml::Visibility::Collapsed);
            return;
        }
        outputPanel_.Visibility(xaml::Visibility::Visible);

        const auto preview = model_->PreviewOutput(targets.front().profile);
        previewText_.Text(preview.example.empty()
                              ? L""
                              : Widen("Saves as   " + preview.example));
        warningText_.Text(Widen(preview.warning));
        warningText_.Visibility(preview.warning.empty() ? xaml::Visibility::Collapsed
                                                        : xaml::Visibility::Visible);
    }

    void OnTemplateChanged(foundation::IInspectable const&,
                           controls::TextChangedEventArgs const&) {
        // Straight through on every keystroke, including the empty string --
        // which the policy already defines as "use the default" rather than as
        // an error, so there is nothing to reject and nothing to fight the user
        // about mid-word.
        model_->output().namingTemplate = Narrow(templateBox_.Text());
        RefreshOutputPanel();
    }

    void OnOverwriteChanged(foundation::IInspectable const&,
                            controls::SelectionChangedEventArgs const&) {
        const std::int32_t index = overwriteBox_.SelectedIndex();
        if (index < 0) return;
        model_->output().overwrite = static_cast<ipc::OverwriteRule>(index);
        RefreshOutputPanel();
    }

    void OnBesideEachFile(foundation::IInspectable const&, xaml::RoutedEventArgs const&) {
        model_->output().directory.clear();
        RefreshOutputPanel();
    }

    // IFileDialog rather than the WinRT FolderPicker: the WinRT one needs an
    // IInitializeWithWindow cast to work outside a UWP process at all, and this
    // process already has the HWND and the COM apartment for the shell dialog.
    // One dependency fewer for the same dialog.
    void OnChooseFolder(foundation::IInspectable const&, xaml::RoutedEventArgs const&) {
        winrt::com_ptr<::IFileOpenDialog> dialog;
        if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(dialog.put())))) {
            return;
        }

        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            (void)dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                                     FOS_PATHMUSTEXIST);
        }
        (void)dialog->SetTitle(L"Where should the converted files go?");

        if (dialog->Show(hwnd_) != S_OK) return;  // cancelled, which is not a failure

        winrt::com_ptr<::IShellItem> item;
        if (FAILED(dialog->GetResult(item.put()))) return;

        LPWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
            model_->output().directory = Narrow(path);
            ::CoTaskMemFree(path);
        }
        RefreshOutputPanel();
    }

    // Both, always. The window title is what the taskbar and alt-tab show; the
    // TextBlock is what the user is actually looking at. Section 12.2 asks for
    // progress "in the title bar", and with the content extended into it those
    // stopped being the same place.
    void SetWindowTitle(std::wstring text) {
        if (window_) window_.Title(text);
        if (titleText_) titleText_.Text(text);
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

        BuildOutputPanel();

        chooser_ = controls::StackPanel{};
        chooser_.Spacing(14);
        chooser_.VerticalAlignment(xaml::VerticalAlignment::Center);
        chooser_.Margin(xaml::ThicknessHelper::FromLengths(48, 48, 48, 48));
        chooser_.Children().Append(heading_);
        chooser_.Children().Append(subheading_);
        chooser_.Children().Append(groups_);
        chooser_.Children().Append(outputPanel_);
    }

    // --- The output policy (section 10.2, section 12.2) ----------------------
    //
    // §10.2 calls this "the part users get wrong and support questions are
    // about". The reason it is easy to get wrong is that nothing shows you the
    // answer until the files already exist, so the panel is built around the
    // preview line rather than around the three controls: the controls are how
    // you change it, the line is what you are changing.
    //
    // The line is ALWAYS visible and the controls are behind an Expander,
    // because §12.2 says the target picker "is the product" and three settings
    // nobody is currently changing should not out-weigh it. Where the files go
    // is worth a sentence on screen at all times; how to alter that is worth a
    // disclosure triangle.
    void BuildOutputPanel() {
        previewText_ = controls::TextBlock{};
        previewText_.Opacity(0.7);
        previewText_.FontSize(12);
        previewText_.TextWrapping(xaml::TextWrapping::Wrap);
        previewText_.HorizontalAlignment(xaml::HorizontalAlignment::Center);
        previewText_.TextAlignment(xaml::TextAlignment::Center);

        warningText_ = controls::TextBlock{};
        warningText_.FontSize(12);
        warningText_.TextWrapping(xaml::TextWrapping::Wrap);
        warningText_.HorizontalAlignment(xaml::HorizontalAlignment::Center);
        warningText_.TextAlignment(xaml::TextAlignment::Center);
        warningText_.Foreground(
            xaml::Media::SolidColorBrush{winrt::Windows::UI::ColorHelper::FromArgb(255, 255, 185, 0)});
        warningText_.Visibility(xaml::Visibility::Collapsed);

        destinationText_ = controls::TextBlock{};
        destinationText_.VerticalAlignment(xaml::VerticalAlignment::Center);
        destinationText_.TextTrimming(xaml::TextTrimming::CharacterEllipsis);
        destinationText_.MaxWidth(280);

        chooseFolder_ = controls::Button{};
        chooseFolder_.Content(winrt::box_value(L"Choose folder…"));
        chooseFolder_.Click({this, &Shell::OnChooseFolder});

        besideEachFile_ = controls::Button{};
        besideEachFile_.Content(winrt::box_value(L"Beside each file"));
        besideEachFile_.Click({this, &Shell::OnBesideEachFile});

        controls::StackPanel destinationRow;
        destinationRow.Orientation(controls::Orientation::Horizontal);
        destinationRow.Spacing(8);
        destinationRow.Children().Append(chooseFolder_);
        destinationRow.Children().Append(besideEachFile_);
        destinationRow.Children().Append(destinationText_);

        templateBox_ = controls::TextBox{};
        templateBox_.Text(Widen(std::string(kDefaultNamingTemplate)));
        templateBox_.Width(260);
        templateBox_.TextChanged({this, &Shell::OnTemplateChanged});
        xaml::Automation::AutomationProperties::SetName(templateBox_, L"File name template");

        controls::TextBlock templateHelp;
        templateHelp.Text(L"{name} {ext} {index} {target}");
        templateHelp.Opacity(0.55);
        templateHelp.FontSize(11);
        templateHelp.VerticalAlignment(xaml::VerticalAlignment::Center);

        controls::StackPanel templateRow;
        templateRow.Orientation(controls::Orientation::Horizontal);
        templateRow.Spacing(8);
        templateRow.Children().Append(templateBox_);
        templateRow.Children().Append(templateHelp);

        overwriteBox_ = controls::ComboBox{};
        // Order matters: it is the OverwriteRule enum's order, so the selected
        // index IS the rule and there is no mapping table to get wrong.
        for (const auto* text : {L"Rename (keep both)", L"Skip", L"Replace"}) {
            overwriteBox_.Items().Append(winrt::box_value(text));
        }
        overwriteBox_.SelectedIndex(static_cast<std::int32_t>(ipc::OverwriteRule::kRename));
        overwriteBox_.SelectionChanged({this, &Shell::OnOverwriteChanged});
        xaml::Automation::AutomationProperties::SetName(overwriteBox_,
                                                        L"When the file already exists");

        controls::StackPanel fields;
        fields.Spacing(10);
        fields.Children().Append(Labelled(L"Save to", destinationRow));
        fields.Children().Append(Labelled(L"Name", templateRow));
        fields.Children().Append(Labelled(L"If it exists", overwriteBox_));

        controls::Expander expander;
        expander.Header(winrt::box_value(L"Output options"));
        expander.Content(fields);
        expander.HorizontalAlignment(xaml::HorizontalAlignment::Center);

        outputPanel_ = controls::StackPanel{};
        outputPanel_.Spacing(6);
        outputPanel_.Margin(xaml::ThicknessHelper::FromLengths(0, 8, 0, 0));
        outputPanel_.Children().Append(previewText_);
        outputPanel_.Children().Append(warningText_);
        outputPanel_.Children().Append(expander);
    }

    [[nodiscard]] static controls::StackPanel Labelled(std::wstring_view text,
                                                       xaml::UIElement const& content) {
        controls::TextBlock label;
        label.Text(text);
        label.Opacity(0.6);
        label.FontSize(12);

        controls::StackPanel row;
        row.Spacing(4);
        row.Children().Append(label);
        row.Children().Append(content);
        return row;
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
            row.Children().Append(TargetEntry(choice));
        }
        RefreshOutputPanel();
    }

    // One target: the button that chooses it, and under it the badge that
    // explains what choosing it costs.
    //
    // THE BADGE IS ITS OWN CONTROL, and that is section 12.2 read literally:
    // "Clicking a badge explains, in one sentence, what will be lost." It used
    // to be a TextBlock inside the button with the sentence on a tooltip, which
    // is not clicking and, more to the point, is not reachable at all by touch
    // -- there is no hover on a touchscreen, so the explanation simply did not
    // exist for anyone using one.
    //
    // It is a sibling rather than a control nested inside the button, because a
    // button inside a button is a hit-testing argument nobody wins: the inner
    // one swallows the click on some paths and the outer one fires on others,
    // and which happens depends on the theme's visual tree. Two controls that
    // each do one thing cannot get that wrong.
    [[nodiscard]] controls::StackPanel TargetEntry(const TargetChoice& choice) {
        const std::wstring explanation = Widen(std::string(choice.Explanation()));

        controls::TextBlock label;
        label.Text(Widen(choice.label));
        label.HorizontalAlignment(xaml::HorizontalAlignment::Center);

        controls::Button button;
        button.Content(label);
        button.MinWidth(120);
        button.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
        // Hover still works and is the fastest way to read it with a mouse. The
        // badge below is what makes it reachable without one.
        controls::ToolTipService::SetToolTip(button, winrt::box_value(explanation));
        xaml::Automation::AutomationProperties::SetName(button, Widen(choice.label));
        xaml::Automation::AutomationProperties::SetHelpText(button, explanation);
        button.Tag(winrt::box_value(Widen(choice.profile)));
        button.Click({this, &Shell::OnChoose});

        controls::TextBlock badgeText;
        badgeText.Text(BadgeFor(choice));
        badgeText.FontSize(11);
        badgeText.Opacity(0.65);

        controls::TextBlock explanationText;
        explanationText.Text(explanation);
        explanationText.TextWrapping(xaml::TextWrapping::Wrap);
        explanationText.MaxWidth(260);

        controls::Flyout flyout;
        flyout.Content(explanationText);

        // A HyperlinkButton rather than a Button: the badge is a thing to read
        // that happens to be clickable, and a second chrome-heavy button under
        // every target would compete with the target itself. Padding stripped
        // for the same reason.
        controls::HyperlinkButton badge;
        badge.Content(badgeText);
        // HyperlinkButton has no Flyout property of its own -- that is Button's.
        // Showing it from the click is the same gesture and one less style to
        // fight with.
        badge.Click([flyout](foundation::IInspectable const& sender,
                             xaml::RoutedEventArgs const&) {
            if (const auto anchor = sender.try_as<xaml::FrameworkElement>()) {
                flyout.ShowAt(anchor);
            }
        });
        badge.Padding(xaml::ThicknessHelper::FromLengths(0, 0, 0, 0));
        badge.HorizontalAlignment(xaml::HorizontalAlignment::Center);
        // Without this a screen reader reads the badge's four words and stops.
        // The four words are the summary; the sentence is the point of clicking.
        xaml::Automation::AutomationProperties::SetName(badge, BadgeFor(choice));
        xaml::Automation::AutomationProperties::SetHelpText(badge, explanation);

        controls::StackPanel entry;
        entry.Spacing(2);
        entry.Children().Append(button);
        entry.Children().Append(badge);
        return entry;
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
            SetWindowTitle(L"Morph");
        } else {
            const int percent = static_cast<int>(fraction * 100.0 + 0.5);
            queueHeading_.Text(Widen(std::to_string(model_->succeeded() + model_->failed() +
                                                    model_->cancelled()) +
                                     " of " + std::to_string(model_->rows().size()) + " done"));
            // "aggregate progress in the title bar and on the taskbar"
            SetWindowTitle(Widen(std::to_string(percent) + "% · Morph"));
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
        SetWindowTitle(L"Morph");
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
            // One round trip for the whole drop, not one per file: the broker
            // hands the batch to its utility worker in a single message.
            ipc::Identified identified;
            if (const Result told = client_.IdentifyFiles(paths, identified); told) {
                probes_->Learn(paths, identified);
            } else {
                probes_->Unavailable(paths, told.message());
            }

            // The model belongs to the UI thread, so the answers are collected
            // against a staging copy here and applied there.
            AppModel staging{[probes = probes_](const std::string& path) {
                return (*probes)(path);
            }};
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

    controls::Grid titleBar_{nullptr};
    controls::Grid dragRegion_{nullptr};
    controls::TextBlock titleText_{nullptr};

    controls::StackPanel chooser_{nullptr};
    controls::TextBlock heading_{nullptr};
    controls::TextBlock subheading_{nullptr};
    controls::StackPanel groups_{nullptr};

    controls::StackPanel outputPanel_{nullptr};
    controls::TextBlock previewText_{nullptr};
    controls::TextBlock warningText_{nullptr};
    controls::TextBlock destinationText_{nullptr};
    controls::Button chooseFolder_{nullptr};
    controls::Button besideEachFile_{nullptr};
    controls::TextBox templateBox_{nullptr};
    controls::ComboBox overwriteBox_{nullptr};

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

    std::shared_ptr<ProbeTable> probes_;
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
