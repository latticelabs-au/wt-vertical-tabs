// ==WindhawkMod==
// @id              windows-terminal-vertical-tabs
// @name            Windows Terminal Vertical Tabs
// @description     Browser-style vertical tabs for Windows Terminal, toggled from any tab's right-click menu
// @version         1.1.0
// @author          Lattice Labs
// @github          https://github.com/addievo
// @homepage        https://github.com/latticelabs-au/wt-vertical-tabs
// @include         WindowsTerminal.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// @license         MIT
// ==/WindhawkMod==

// Source code: https://github.com/latticelabs-au/wt-vertical-tabs
// Copyright (c) 2026 Lattice Labs. MIT License.

// ==WindhawkModReadme==
/*
# Windows Terminal Vertical Tabs

Browser-style vertical tabs for Windows Terminal. The tab strip moves into a
sidebar where every tab gets a full-width row with its icon, title and close
button, and the sidebar collapses to an icon rail, the way Edge does it.

![Vertical tabs](https://raw.githubusercontent.com/latticelabs-au/wt-vertical-tabs/main/docs/images/vertical.png)

## Usage

Enabling the mod changes nothing on its own: Terminal keeps its horizontal tabs
until you ask for vertical ones.

- **Turn it on:** right-click any tab and choose **Turn on vertical tabs**.
- **Turn it off:** right-click any tab and choose **Turn off vertical tabs**.
- **Collapse or expand:** the pane button at the top of the sidebar. Collapsed,
  the sidebar is a narrow rail of tab icons. Each window collapses on its own.
- **Peek:** rest the mouse on a collapsed rail and it opens over the terminal,
  with titles, until the mouse leaves. The terminal doesn't move.
- **New tab:** the **+** button sits right under the last tab.

Both choices are remembered across restarts. New windows, and every window
after a restart, start collapsed or expanded the way you last left one.

## Settings

- **Sidebar width:** 120 to 600 pixels, 220 by default.
- **Sidebar position:** left or right.

## Notes

- Works with "Show tabs in title bar" on or off. When on, the title bar keeps
  the window buttons and becomes a plain drag area, like Edge in vertical tab
  mode.
- Focus mode and full screen hide the sidebar, the same as they hide tabs.
- Tab colours, renaming, the tab menu and the new-tab dropdown keep working.
- Nothing on disk is patched. The mod rearranges Terminal's tab strip in memory
  and disabling it puts everything back without restarting Terminal.
- Tested on Windows Terminal 1.24. If a future Terminal changes its tab strip,
  the mod leaves the tabs alone and says so in the log.
- The mod finds the tab strip through XAML Diagnostics. Once loaded, that costs
  about 35 KB per tab opened, until Terminal restarts.

Source, screenshots and the design notes:
[latticelabs-au/wt-vertical-tabs](https://github.com/latticelabs-au/wt-vertical-tabs).
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- sidebarWidth: 220
  $name: Sidebar width
  $description: Width of the tab sidebar, in pixels (120 to 600)
- position: left
  $name: Sidebar position
  $options:
  - left: Left
  - right: Right
*/
// ==/WindhawkModSettings==

#define WH_WINRT_WINUI2

#include <windhawk_utils.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Unknwn.h>
#include <ocidl.h>
#include <xamlom.h>

#undef GetCurrentTime

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace wf = winrt::Windows::Foundation;
namespace ws = winrt::Windows::System;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxi = winrt::Windows::UI::Xaml::Input;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;

#ifdef WTVT_TEST_HOOKS
// Test builds also append every log line to the file named by WTVT_TEST_LOG,
// so a test can read the mod's log without an OutputDebugString listener,
// which can stop receiving for reasons outside the mod.
void TestLogLine(PCWSTR format, ...) {
    WCHAR path[MAX_PATH];
    if (!GetEnvironmentVariableW(L"WTVT_TEST_LOG", path, ARRAYSIZE(path))) {
        return;
    }
    WCHAR line[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(line, _TRUNCATE, format, args);
    va_end(args);
    SYSTEMTIME now;
    GetLocalTime(&now);
    char text[4096];
    int prefix = sprintf_s(text, "%02u:%02u:%02u %lu ", now.wHour, now.wMinute, now.wSecond,
                           GetCurrentProcessId());
    int body = WideCharToMultiByte(CP_UTF8, 0, line, -1, text + prefix,
                                   static_cast<int>(sizeof(text)) - prefix - 2, nullptr, nullptr);
    if (prefix < 0 || body <= 0) {
        return;
    }
    int length = prefix + body - 1;
    text[length++] = '\r';
    text[length++] = '\n';
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, text, length, &written, nullptr);
        CloseHandle(file);
    }
}

#undef Wh_Log
#define Wh_Log(message, ...)                                                       \
    do {                                                                           \
        if (InternalWh_IsLogEnabled(InternalWhModPtr)) {                           \
            InternalWh_Log_Wrapper(L"[%d:%S]: " message, __LINE__, __FUNCTION__, \
                                   ##__VA_ARGS__);                                 \
        }                                                                          \
        TestLogLine(L"[%d:%S]: " message, __LINE__, __FUNCTION__, ##__VA_ARGS__);  \
    } while (0)
#endif

////////////////////////////////////////////////////////////////////////////////
// Settings

constexpr int kMinSidebarWidth = 120;
constexpr int kMaxSidebarWidth = 600;

std::atomic<int> g_sidebarWidth{220};
std::atomic<bool> g_sidebarOnRight{false};

// The user's layout choice, toggled from a tab's context menu, and kept in the
// mod's storage between sessions. Horizontal (Windows Terminal's own layout)
// until turned on. Collapsing is per window (WindowState::collapsed); this is
// the state a new window starts in: the one last chosen in any window, also
// kept between sessions.
constexpr double kCollapsedWidth = 48;
std::atomic<bool> g_vertical{false};
std::atomic<bool> g_collapsed{false};

void LoadSettings() {
    g_sidebarWidth = std::clamp(Wh_GetIntSetting(L"sidebarWidth"),
                                kMinSidebarWidth, kMaxSidebarWidth);

    PCWSTR position = Wh_GetStringSetting(L"position");
    g_sidebarOnRight = position && wcscmp(position, L"right") == 0;
    Wh_FreeStringSetting(position);
}

// Cleared first thing on uninit, so that nothing new is started while the
// layout is being put back.
std::atomic<bool> g_active{false};

////////////////////////////////////////////////////////////////////////////////
// Vertical templates

// The tab strip itself is not re-templated: its list owns the tab items, and
// moving live items from one list to another fails. Instead the stock template
// is rearranged in place (see ApplyVerticalStrip). Only the list's panel may
// need a template, when the list hasn't created its panel yet.
constexpr std::wstring_view kVerticalItemsPanelXaml = LR"(
<ItemsPanelTemplate
    xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
    <ItemsStackPanel Orientation="Vertical" />
</ItemsPanelTemplate>
)";

// A tab as a full-width row. Part names match the stock template where TabViewItem
// code looks them up (ContentPresenter, CloseButton) or binds them
// (IconControl), and the visual state names match the ones it and ListViewItem
// go to. Colours come from the same theme resources the stock template uses, so
// per-tab colours and Windows Terminal themes carry over.
constexpr std::wstring_view kTabViewItemTemplateXaml = LR"(
<ControlTemplate
    xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
    xmlns:mux="using:Microsoft.UI.Xaml.Controls"
    TargetType="mux:TabViewItem">
    <Grid x:Name="LayoutRoot"
          Padding="0,1,0,1">
        <Grid.RenderTransform>
            <ScaleTransform x:Name="LayoutRootScale" />
        </Grid.RenderTransform>

        <VisualStateManager.VisualStateGroups>
            <VisualStateGroup x:Name="CommonStates">
                <VisualState x:Name="Normal" />
                <VisualState x:Name="PointerOver">
                    <VisualState.Setters>
                        <Setter Target="TabContainer.Background" Value="{ThemeResource TabViewItemHeaderBackgroundPointerOver}" />
                        <Setter Target="ContentPresenter.Foreground" Value="{ThemeResource TabViewItemHeaderForegroundPointerOver}" />
                        <Setter Target="IconControl.Foreground" Value="{ThemeResource TabViewItemIconForegroundPointerOver}" />
                        <Setter Target="CloseButton.Background" Value="{ThemeResource TabViewItemHeaderPointerOverCloseButtonBackground}" />
                        <Setter Target="CloseButton.Foreground" Value="{ThemeResource TabViewItemHeaderPointerOverCloseButtonForeground}" />
                    </VisualState.Setters>
                </VisualState>
                <VisualState x:Name="Pressed">
                    <VisualState.Setters>
                        <Setter Target="TabContainer.Background" Value="{ThemeResource TabViewItemHeaderBackgroundPressed}" />
                        <Setter Target="ContentPresenter.Foreground" Value="{ThemeResource TabViewItemHeaderForegroundPressed}" />
                        <Setter Target="IconControl.Foreground" Value="{ThemeResource TabViewItemIconForegroundPressed}" />
                        <Setter Target="CloseButton.Background" Value="{ThemeResource TabViewItemHeaderPressedCloseButtonBackground}" />
                        <Setter Target="CloseButton.Foreground" Value="{ThemeResource TabViewItemHeaderPressedCloseButtonForeground}" />
                    </VisualState.Setters>
                </VisualState>
                <VisualState x:Name="Selected">
                    <VisualState.Setters>
                        <Setter Target="TabContainer.Background" Value="{ThemeResource TabViewItemHeaderBackgroundSelected}" />
                        <Setter Target="ContentPresenter.Foreground" Value="{ThemeResource TabViewItemHeaderForegroundSelected}" />
                        <Setter Target="ContentPresenter.FontWeight" Value="SemiBold" />
                        <Setter Target="IconControl.Foreground" Value="{ThemeResource TabViewItemIconForegroundSelected}" />
                        <Setter Target="CloseButton.Background" Value="{ThemeResource TabViewItemHeaderSelectedCloseButtonBackground}" />
                        <Setter Target="CloseButton.Foreground" Value="{ThemeResource TabViewItemHeaderSelectedCloseButtonForeground}" />
                        <Setter Target="SelectionPill.Opacity" Value="1" />
                    </VisualState.Setters>
                </VisualState>
                <VisualState x:Name="PointerOverSelected">
                    <VisualState.Setters>
                        <Setter Target="TabContainer.Background" Value="{ThemeResource TabViewItemHeaderBackgroundSelected}" />
                        <Setter Target="ContentPresenter.Foreground" Value="{ThemeResource TabViewItemHeaderForegroundSelected}" />
                        <Setter Target="ContentPresenter.FontWeight" Value="SemiBold" />
                        <Setter Target="IconControl.Foreground" Value="{ThemeResource TabViewItemIconForegroundSelected}" />
                        <Setter Target="CloseButton.Background" Value="{ThemeResource TabViewItemHeaderPointerOverCloseButtonBackground}" />
                        <Setter Target="CloseButton.Foreground" Value="{ThemeResource TabViewItemHeaderPointerOverCloseButtonForeground}" />
                        <Setter Target="SelectionPill.Opacity" Value="1" />
                    </VisualState.Setters>
                </VisualState>
                <VisualState x:Name="PressedSelected">
                    <VisualState.Setters>
                        <Setter Target="TabContainer.Background" Value="{ThemeResource TabViewItemHeaderBackgroundSelected}" />
                        <Setter Target="ContentPresenter.Foreground" Value="{ThemeResource TabViewItemHeaderForegroundSelected}" />
                        <Setter Target="ContentPresenter.FontWeight" Value="SemiBold" />
                        <Setter Target="IconControl.Foreground" Value="{ThemeResource TabViewItemIconForegroundSelected}" />
                        <Setter Target="CloseButton.Background" Value="{ThemeResource TabViewItemHeaderPressedCloseButtonBackground}" />
                        <Setter Target="CloseButton.Foreground" Value="{ThemeResource TabViewItemHeaderPressedCloseButtonForeground}" />
                        <Setter Target="SelectionPill.Opacity" Value="1" />
                    </VisualState.Setters>
                </VisualState>
            </VisualStateGroup>

            <VisualStateGroup x:Name="DisabledStates">
                <VisualState x:Name="Enabled" />
                <VisualState x:Name="Disabled">
                    <VisualState.Setters>
                        <Setter Target="TabContainer.Background" Value="{ThemeResource TabViewItemHeaderBackgroundDisabled}" />
                        <Setter Target="ContentPresenter.Foreground" Value="{ThemeResource TabViewItemHeaderForegroundDisabled}" />
                        <Setter Target="IconControl.Foreground" Value="{ThemeResource TabViewItemIconForegroundDisabled}" />
                        <Setter Target="CloseButton.Foreground" Value="{ThemeResource TabViewItemHeaderDisabledCloseButtonForeground}" />
                    </VisualState.Setters>
                </VisualState>
            </VisualStateGroup>

            <VisualStateGroup x:Name="ReorderHintStates">
                <VisualState x:Name="NoReorderHint" />
                <VisualState x:Name="BottomReorderHint">
                    <Storyboard>
                        <DragOverThemeAnimation TargetName="LayoutRoot" ToOffset="8" Direction="Bottom" />
                    </Storyboard>
                </VisualState>
                <VisualState x:Name="TopReorderHint">
                    <Storyboard>
                        <DragOverThemeAnimation TargetName="LayoutRoot" ToOffset="8" Direction="Top" />
                    </Storyboard>
                </VisualState>
                <VisualState x:Name="RightReorderHint" />
                <VisualState x:Name="LeftReorderHint" />
                <VisualStateGroup.Transitions>
                    <VisualTransition To="NoReorderHint" GeneratedDuration="0:0:0.2" />
                </VisualStateGroup.Transitions>
            </VisualStateGroup>

            <VisualStateGroup x:Name="DragStates">
                <VisualState x:Name="NotDragging" />
                <VisualState x:Name="Dragging">
                    <Storyboard>
                        <DoubleAnimation Storyboard.TargetName="LayoutRoot" Storyboard.TargetProperty="Opacity" To="0.8" Duration="0" />
                    </Storyboard>
                </VisualState>
                <VisualState x:Name="DraggingTarget" />
                <VisualState x:Name="MultipleDraggingPrimary" />
                <VisualState x:Name="MultipleDraggingSecondary" />
                <VisualState x:Name="DraggedPlaceholder">
                    <Storyboard>
                        <FadeOutThemeAnimation TargetName="LayoutRoot" />
                    </Storyboard>
                </VisualState>
                <VisualState x:Name="Reordering">
                    <Storyboard>
                        <DoubleAnimation Storyboard.TargetName="LayoutRoot" Storyboard.TargetProperty="Opacity" To="0.8" Duration="0:0:0.240" />
                    </Storyboard>
                </VisualState>
                <VisualState x:Name="ReorderingTarget" />
                <VisualState x:Name="MultipleReorderingPrimary" />
                <VisualState x:Name="ReorderedPlaceholder">
                    <Storyboard>
                        <FadeOutThemeAnimation TargetName="LayoutRoot" />
                    </Storyboard>
                </VisualState>
                <VisualState x:Name="DragOver">
                    <Storyboard>
                        <DropTargetItemThemeAnimation TargetName="LayoutRoot" />
                    </Storyboard>
                </VisualState>
                <VisualStateGroup.Transitions>
                    <VisualTransition To="NotDragging" GeneratedDuration="0:0:0.2" />
                </VisualStateGroup.Transitions>
            </VisualStateGroup>

            <VisualStateGroup x:Name="IconStates">
                <VisualState x:Name="Icon" />
                <VisualState x:Name="NoIcon">
                    <VisualState.Setters>
                        <Setter Target="IconBox.Visibility" Value="Collapsed" />
                    </VisualState.Setters>
                </VisualState>
            </VisualStateGroup>

            <VisualStateGroup x:Name="CloseIconStates">
                <VisualState x:Name="CloseButtonVisible" />
                <VisualState x:Name="CloseButtonCollapsed">
                    <VisualState.Setters>
                        <Setter Target="CloseButton.Visibility" Value="Collapsed" />
                    </VisualState.Setters>
                </VisualState>
            </VisualStateGroup>

            <VisualStateGroup x:Name="ForegroundStates">
                <VisualState x:Name="ForegroundNotSet" />
                <VisualState x:Name="ForegroundSet">
                    <VisualState.Setters>
                        <Setter Target="IconControl.Foreground" Value="{Binding RelativeSource={RelativeSource TemplatedParent}, Path=Foreground}" />
                        <Setter Target="ContentPresenter.Foreground" Value="{Binding RelativeSource={RelativeSource TemplatedParent}, Path=Foreground}" />
                    </VisualState.Setters>
                </VisualState>
            </VisualStateGroup>
        </VisualStateManager.VisualStateGroups>

        <Grid x:Name="TabContainer"
              MinHeight="36"
              Padding="10,0,4,0"
              CornerRadius="4"
              Background="{TemplateBinding Background}"
              Control.IsTemplateFocusTarget="True"
              FocusVisualMargin="{TemplateBinding FocusVisualMargin}">
            <Grid.ColumnDefinitions>
                <ColumnDefinition x:Name="IconColumn" Width="Auto" />
                <ColumnDefinition Width="*" />
                <ColumnDefinition Width="Auto" />
            </Grid.ColumnDefinitions>

            <Viewbox x:Name="IconBox"
                     MaxWidth="16"
                     MaxHeight="16"
                     Margin="0,0,10,0"
                     VerticalAlignment="Center">
                <ContentControl x:Name="IconControl"
                                Content="{Binding RelativeSource={RelativeSource TemplatedParent}, Path=TabViewTemplateSettings.IconElement}"
                                IsTabStop="False"
                                Foreground="{ThemeResource TabViewItemIconForeground}"
                                HighContrastAdjustment="None" />
            </Viewbox>

            <ContentPresenter x:Name="ContentPresenter"
                              Grid.Column="1"
                              HorizontalAlignment="Stretch"
                              VerticalAlignment="Center"
                              Content=""
                              ContentTemplate="{TemplateBinding HeaderTemplate}"
                              FontWeight="{TemplateBinding FontWeight}"
                              FontSize="{ThemeResource TabViewItemHeaderFontSize}"
                              Foreground="{ThemeResource TabViewItemHeaderForeground}"
                              OpticalMarginAlignment="TrimSideBearings"
                              HighContrastAdjustment="None" />

            <!-- TabViewCloseButtonStyle lives in WinUI's own control dictionary,
                 which templates loaded at runtime can't see, so its look is
                 inlined here from the theme resources it uses. -->
            <Button x:Name="CloseButton"
                    Grid.Column="2"
                    Width="28"
                    Height="24"
                    Margin="4,0,0,0"
                    VerticalAlignment="Center"
                    HorizontalContentAlignment="Center"
                    VerticalContentAlignment="Center"
                    Content="&#xE711;"
                    FontFamily="Segoe Fluent Icons, Segoe MDL2 Assets"
                    FontSize="{ThemeResource TabViewItemHeaderCloseFontSize}"
                    CornerRadius="4"
                    Background="{ThemeResource TabViewItemHeaderCloseButtonBackground}"
                    Foreground="{ThemeResource TabViewItemHeaderCloseButtonForeground}"
                    BorderBrush="{ThemeResource TabViewItemHeaderCloseButtonBorderBrush}"
                    BorderThickness="0"
                    FocusVisualMargin="-3"
                    IsTextScaleFactorEnabled="False"
                    IsTabStop="False"
                    HighContrastAdjustment="None">
                <Button.Template>
                    <ControlTemplate TargetType="Button">
                        <ContentPresenter x:Name="ContentPresenter"
                                          AutomationProperties.AccessibilityView="Raw"
                                          Background="{TemplateBinding Background}"
                                          BorderBrush="{TemplateBinding BorderBrush}"
                                          BorderThickness="{TemplateBinding BorderThickness}"
                                          CornerRadius="{TemplateBinding CornerRadius}"
                                          Content="{TemplateBinding Content}"
                                          ContentTemplate="{TemplateBinding ContentTemplate}"
                                          HorizontalContentAlignment="{TemplateBinding HorizontalContentAlignment}"
                                          VerticalContentAlignment="{TemplateBinding VerticalContentAlignment}">
                            <VisualStateManager.VisualStateGroups>
                                <VisualStateGroup x:Name="CommonStates">
                                    <VisualState x:Name="Normal" />
                                    <VisualState x:Name="PointerOver">
                                        <VisualState.Setters>
                                            <Setter Target="ContentPresenter.Background" Value="{ThemeResource TabViewItemHeaderCloseButtonBackgroundPointerOver}" />
                                            <Setter Target="ContentPresenter.Foreground" Value="{ThemeResource TabViewItemHeaderCloseButtonForegroundPointerOver}" />
                                            <Setter Target="ContentPresenter.BorderBrush" Value="{ThemeResource TabViewItemHeaderCloseButtonBorderBrushPointerOver}" />
                                        </VisualState.Setters>
                                    </VisualState>
                                    <VisualState x:Name="Pressed">
                                        <VisualState.Setters>
                                            <Setter Target="ContentPresenter.Background" Value="{ThemeResource TabViewItemHeaderCloseButtonBackgroundPressed}" />
                                            <Setter Target="ContentPresenter.Foreground" Value="{ThemeResource TabViewItemHeaderCloseButtonForegroundPressed}" />
                                            <Setter Target="ContentPresenter.BorderBrush" Value="{ThemeResource TabViewItemHeaderCloseButtonBorderBrushPressed}" />
                                        </VisualState.Setters>
                                    </VisualState>
                                </VisualStateGroup>
                            </VisualStateManager.VisualStateGroups>
                        </ContentPresenter>
                    </ControlTemplate>
                </Button.Template>
            </Button>
        </Grid>

        <Border x:Name="SelectionPill"
                Width="3"
                Height="16"
                Margin="1,0,0,0"
                HorizontalAlignment="Left"
                VerticalAlignment="Center"
                CornerRadius="1.5"
                Opacity="0"
                IsHitTestVisible="False"
                Background="{ThemeResource AccentFillColorDefaultBrush}" />
    </Grid>
</ControlTemplate>
)";

////////////////////////////////////////////////////////////////////////////////
// Visual tree helpers

bool IsClass(wf::IInspectable const& object, std::wstring_view className) {
    return object && winrt::get_class_name(object) == className;
}

wux::DependencyObject GetParent(wux::DependencyObject const& element) {
    return element ? wuxm::VisualTreeHelper::GetParent(element) : nullptr;
}

wux::DependencyObject GetTopAncestor(wux::DependencyObject element) {
    while (auto parent = GetParent(element)) {
        element = parent;
    }
    return element;
}

template <typename T>
T FindAncestor(wux::DependencyObject element) {
    for (element = GetParent(element); element; element = GetParent(element)) {
        if (auto match = element.try_as<T>()) {
            return match;
        }
    }
    return nullptr;
}

// Depth-first search. `descend` decides whether to look inside an element that
// didn't match, which keeps searches out of subtrees that can't contain the
// target (for example the tab list when looking for template parts).
wux::DependencyObject FindDescendant(
    wux::DependencyObject const& root,
    std::function<bool(wux::DependencyObject const&)> const& match,
    std::function<bool(wux::DependencyObject const&)> const& descend = nullptr,
    int maxDepth = 32) {
    if (!root || maxDepth <= 0) {
        return nullptr;
    }

    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; i++) {
        auto child = wuxm::VisualTreeHelper::GetChild(root, i);
        if (match(child)) {
            return child;
        }
        if (!descend || descend(child)) {
            if (auto found = FindDescendant(child, match, descend, maxDepth - 1)) {
                return found;
            }
        }
    }
    return nullptr;
}

bool HasName(wux::DependencyObject const& element, std::wstring_view name) {
    auto fe = element.try_as<wux::FrameworkElement>();
    return fe && fe.Name() == name;
}

// Before a template is replaced, detach the content its presenters host. The
// control hands the same header, icon or strip content to the presenters of the
// new template, and a UIElement that still has a parent can't be adopted.
void ReleaseTemplateContent(wux::DependencyObject const& control,
                            std::initializer_list<std::wstring_view> names,
                            std::wstring_view stopAtName = {}) {
    std::function<void(wux::DependencyObject const&, int)> walk =
        [&](wux::DependencyObject const& element, int depth) {
            if (depth > 16) {
                return;
            }
            int count = wuxm::VisualTreeHelper::GetChildrenCount(element);
            for (int i = 0; i < count; i++) {
                auto child = wuxm::VisualTreeHelper::GetChild(element, i);
                auto fe = child.try_as<wux::FrameworkElement>();
                if (fe) {
                    auto name = fe.Name();
                    if (!stopAtName.empty() && name == stopAtName) {
                        continue;
                    }
                    if (std::find(names.begin(), names.end(),
                                  std::wstring_view{name}) != names.end()) {
                        if (auto presenter = fe.try_as<wuxc::ContentPresenter>()) {
                            presenter.Content(nullptr);
                            continue;
                        }
                        if (auto contentControl = fe.try_as<wuxc::ContentControl>()) {
                            contentControl.Content(nullptr);
                            continue;
                        }
                    }
                }
                walk(child, depth + 1);
            }
        };
    walk(control, 0);
}

////////////////////////////////////////////////////////////////////////////////
// Per-window and per-thread state

// A local property value the mod changed, with what was there before, so that
// disabling the mod leaves the terminal as it found it.
//
// Elements of the tree are held weakly, so a closed window isn't kept alive.
// Anything else (row and column definitions, flyouts) is held strongly: XAML
// drops the object behind such a reference when nothing else holds it, a weak
// reference to it then comes back empty, and the value would never be put
// back.
struct SavedValue {
    winrt::weak_ref<wux::DependencyObject> weakObject;
    wux::DependencyObject strongObject{nullptr};
    wux::DependencyProperty property{nullptr};
    wf::IInspectable value;

    wux::DependencyObject Object() const {
        return strongObject ? strongObject : weakObject.get();
    }
};

struct WindowState {
    winrt::weak_ref<wuxc::ContentPresenter> tabRow;
    winrt::weak_ref<wuxc::Grid> pageRoot;
    winrt::weak_ref<muxc::TabView> tabView;

    // Tracking, kept in both layouts: new tabs, and the toggle in each tab's
    // context menu.
    winrt::event_token tabItemsChangedToken{};
    // Held strongly (see SavedValue): an entry that couldn't be found again
    // would stay in Terminal's menu after the mod unloads, with a click handler
    // pointing into the unloaded module.
    struct MenuEntry {
        wuxc::MenuFlyout flyout{nullptr};
        wuxc::MenuFlyoutItem item{nullptr};
        winrt::event_token clickToken{};
    };
    std::vector<MenuEntry> menuEntries;

    // Layout, while vertical tabs are on.
    bool layoutApplied = false;
    // Set when the strip couldn't be made vertical (an unfamiliar template, or
    // a failure part way through). The window is then left alone until the
    // next toggle or settings change: retrying on every tab row sighting would
    // move the row back and forth, and each move is another sighting.
    bool stripFailed = false;
    // Collapsed to the icon rail, by this window's own collapse button.
    bool collapsed = false;
    // A collapsed rail opens to full width while the mouse rests on it, drawn
    // over the terminal rather than pushing it aside (see SetPeek), and closes
    // when the mouse leaves. After the rail is collapsed with the mouse on its
    // button, it stays shut until the mouse has left once.
    bool peeking = false;
    bool suppressPeek = false;
    bool dragging = false;
    ULONGLONG leftAt = 0;
    wf::IInspectable pointerEnteredHandler{nullptr};
    wf::IInspectable pointerExitedHandler{nullptr};
    winrt::event_token dragStartingToken{};
    winrt::event_token dragCompletedToken{};
    ws::DispatcherQueueTimer peekTimer{nullptr};
    winrt::event_token peekTimerToken{};
    // The opaque backdrop behind the strip while it peeks over the terminal
    // (see kPeekBackdropXaml); plain when its theme resources failed to load.
    winrt::weak_ref<wuxc::Border> peekBackdrop;
    bool peekBackdropPlain = false;
    // The XAML island window this sidebar lives in, recorded each time the
    // mouse enters it: other Terminal windows share the thread, and may share
    // the size too.
    HWND island = nullptr;

    // Set when "Show tabs in title bar" is on: the title bar presenter the tab
    // row was taken from, and the title bar itself, whose background the
    // sidebar borrows.
    winrt::weak_ref<wuxc::ContentPresenter> titlebarHost;
    winrt::weak_ref<wuxc::Panel> titlebar;
    int64_t titlebarBackgroundToken = 0;

    // TabContent and the info bar panel, and their margins before the mod
    // pushed them aside to make room for the sidebar.
    std::vector<std::pair<winrt::weak_ref<wux::FrameworkElement>, wux::Thickness>>
        shiftedContent;

    int64_t tabViewVisibilityToken = 0;

    // The tab strip template, rearranged in place: its column definitions are
    // taken out and rows put in.
    bool stripApplied = false;
    winrt::weak_ref<wuxc::Grid> containerGrid;
    std::vector<wuxc::ColumnDefinition> removedColumns;
    std::vector<wuxc::RowDefinition> addedRows;
    winrt::weak_ref<wuxc::ListView> listView;
    winrt::weak_ref<wux::FrameworkElement> itemsPresenter;
    winrt::event_token itemsPresenterSizeChangedToken{};
    // The strip footer (Windows Terminal's new-tab button). It sits right under
    // the last tab; the size handler keeps the tab list short enough for that.
    winrt::weak_ref<wux::FrameworkElement> footer;
    winrt::event_token containerSizeChangedToken{};

    // The collapse/expand button the mod adds at the top of the sidebar.
    winrt::weak_ref<wuxc::Button> collapseButton;
    winrt::event_token collapseButtonClickToken{};

    // Windows Terminal's new-tab split button in the strip footer, and the
    // callback that re-places its dropdown whenever Terminal rebuilds it. The
    // flyout currently re-placed, and its own placement, are kept in a slot of
    // their own rather than in savedValues, which would otherwise gain an entry
    // (and keep a whole flyout alive) on every Terminal settings reload.
    winrt::weak_ref<muxc::SplitButton> newTabButton;
    int64_t newTabFlyoutToken = 0;
    wuxc::Primitives::FlyoutBase newTabFlyout{nullptr};
    wf::IInspectable newTabFlyoutPlacement{nullptr};
    winrt::event_token newTabFlyoutOpeningToken{};

    // See ReassertVerticalStrip.
    int reassertCount = 0;
    ULONGLONG reassertPeriodStart = 0;

    std::vector<SavedValue> savedValues;
};

struct ThreadContext {
    DWORD threadId = 0;

    // Tab templates for the expanded sidebar and the collapsed rail. Each is
    // tried on one tab before use (see ValidateTabViewItemTemplate).
    wuxc::ControlTemplate tabTemplate{nullptr};
    bool tabTemplateValidated = false;
    wuxc::ControlTemplate collapsedTabTemplate{nullptr};
    bool collapsedTabTemplateValidated = false;
    wuxc::ItemsPanelTemplate verticalItemsPanel{nullptr};

    std::vector<std::unique_ptr<WindowState>> windows;

    // Work that can't be done from inside a visual tree callback or an event
    // handler runs from these one-shot timers. A timer, unlike a dispatcher
    // work item, can be stopped from the thread's uninit, so nothing is left
    // queued to call into the mod after it's unloaded.
    ws::DispatcherQueueTimer workTimer{nullptr};
    winrt::event_token workTimerToken{};
    std::vector<winrt::weak_ref<wuxc::ContentPresenter>> pendingTabRows;
    bool pendingToggleVertical = false;
    // The TabViews whose collapse button was clicked.
    std::vector<winrt::weak_ref<muxc::TabView>> pendingCollapseToggles;
    bool pendingMenuSweep = false;
    bool pendingTemplateRetry = false;

#ifdef WTVT_TEST_HOOKS
    // Development builds only (tools/build.py --define WTVT_TEST_HOOKS): lets
    // an automated test open a tab's context menu without real mouse input.
    HHOOK testHook = nullptr;
    int pendingTestMenuTab = -1;
    int pendingTestPointer = -1;
    bool pendingTestReport = false;
    bool pendingTestClosePopups = false;
    // The menu the hook last opened, and its one-shot Opened handler, which
    // stays registered if the menu never opens; revoked on the next open and
    // in the thread's uninit. Held strongly: a weak reference to a flyout can
    // come back empty (see SavedValue), and the handler would stay behind.
    wuxc::Primitives::FlyoutBase testMenu{nullptr};
    winrt::event_token testMenuOpenedToken{};
#endif

    ws::DispatcherQueueTimer releaseTimer{nullptr};
    winrt::event_token releaseTimerToken{};
    std::vector<InstanceHandle> pendingReleases;
    ULONGLONG lastReleaseQueueTick = 0;

    ws::DispatcherQueueTimer detachTimer{nullptr};
    winrt::event_token detachTimerToken{};

    // Zero-delay thread timers set on window creation (see CreateWindowExW_Hook).
    std::vector<UINT_PTR> windowTimers;
};

std::mutex g_contextsMutex;
std::vector<ThreadContext*> g_contexts;
// A plain pointer: a thread_local with a destructor would register code that
// outlives the module on threads the mod never gets to clean up.
thread_local ThreadContext* t_context = nullptr;

#ifdef WTVT_TEST_HOOKS
UINT TestMessage() {
    static UINT message = RegisterWindowMessageW(L"WTVT_TEST_OPEN_TAB_MENU");
    return message;
}

// wParam 1: the mouse entered the sidebar, 0: it left. The cursor is then
// taken to be where the message says, not where it really is, until a
// message with wParam 2 puts the real cursor back.
UINT TestPointerMessage() {
    static UINT message = RegisterWindowMessageW(L"WTVT_TEST_SIDEBAR_POINTER");
    return message;
}

// Logs whether the diagnostics are attached, from the UI thread.
UINT TestReportMessage() {
    static UINT message = RegisterWindowMessageW(L"WTVT_TEST_REPORT");
    return message;
}

// Closes every open popup (menus, flyouts) of the thread's windows, as Escape
// would; UI Automation can't reach the rest of a window while one is open.
UINT TestClosePopupsMessage() {
    static UINT message = RegisterWindowMessageW(L"WTVT_TEST_CLOSE_POPUPS");
    return message;
}

void QueueWork();
LRESULT CALLBACK TestGetMessageHook(int code, WPARAM wParam, LPARAM lParam);
int DiagnosticsAdvisedForTest();

void RevokeTestMenuHandler(ThreadContext& context) {
    auto menu = std::exchange(context.testMenu, nullptr);
    auto token = std::exchange(context.testMenuOpenedToken, {});
    if (menu && token) {
        menu.Opened(token);
    }
}
#endif

ThreadContext* GetThreadContext(bool create) {
    if (!t_context && create) {
        auto context = new ThreadContext();
        context->threadId = GetCurrentThreadId();
#ifdef WTVT_TEST_HOOKS
        context->testHook = SetWindowsHookExW(WH_GETMESSAGE, TestGetMessageHook, nullptr,
                                              context->threadId);
#endif
        std::lock_guard lock(g_contextsMutex);
        g_contexts.push_back(context);
        t_context = context;
    }
    return t_context;
}

void SetValueSaved(WindowState& window,
                   wux::DependencyObject const& object,
                   wux::DependencyProperty const& property,
                   wf::IInspectable const& value) {
    bool saved = std::any_of(
        window.savedValues.begin(), window.savedValues.end(),
        [&](SavedValue const& entry) {
            return entry.property == property && entry.Object() == object;
        });
    if (!saved) {
        SavedValue entry;
        if (object.try_as<wux::UIElement>()) {
            entry.weakObject = winrt::make_weak(object);
        } else {
            entry.strongObject = object;
        }
        entry.property = property;
        entry.value = object.ReadLocalValue(property);
        window.savedValues.push_back(std::move(entry));
    }
    object.SetValue(property, value);
}

void RestoreSavedValues(WindowState& window) {
    for (auto it = window.savedValues.rbegin(); it != window.savedValues.rend();
         ++it) {
        auto object = it->Object();
        if (!object) {
            continue;
        }
        try {
            if (it->value == wux::DependencyProperty::UnsetValue()) {
                object.ClearValue(it->property);
            } else {
                object.SetValue(it->property, it->value);
            }
        } catch (winrt::hresult_error const& ex) {
            Wh_Log(L"Restoring a value failed: %08X", ex.code().value);
        }
    }
    window.savedValues.clear();
}

// Whether the sidebar shows the icon rail right now (collapsed, and not peeking).
bool LooksCollapsed(WindowState const& window) {
    return window.collapsed && !window.peeking;
}

// The sidebar's width right now.
double SidebarWidth(WindowState const& window) {
    return LooksCollapsed(window) ? kCollapsedWidth : g_sidebarWidth.load();
}

// The room the terminal makes for the sidebar. A peeking rail draws over the
// terminal, so the terminal keeps the rail's width and doesn't reflow.
double DockedWidth(WindowState const& window) {
    return window.collapsed ? kCollapsedWidth : g_sidebarWidth.load();
}

// Puts one recorded property back to its value from before the mod, keeping the
// record, for parts of the layout that switch back and forth.
void RestoreSavedValue(WindowState& window,
                       wux::DependencyObject const& object,
                       wux::DependencyProperty const& property) {
    for (auto const& entry : window.savedValues) {
        if (entry.property == property && entry.Object() == object) {
            if (entry.value == wux::DependencyProperty::UnsetValue()) {
                object.ClearValue(property);
            } else {
                object.SetValue(property, entry.value);
            }
            return;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Templates

template <typename T>
T LoadXaml(std::wstring_view xaml) {
    try {
        return wux::Markup::XamlReader::Load(winrt::hstring{xaml}).as<T>();
    } catch (winrt::hresult_error const& ex) {
        Wh_Log(L"Loading XAML failed: %08X %s", ex.code().value, ex.message().c_str());
        return nullptr;
    }
}

std::wstring ReplaceOnce(std::wstring text, std::wstring_view from, std::wstring_view to) {
    auto at = text.find(from);
    if (at != std::wstring::npos) {
        text.replace(at, from.size(), to);
    }
    return text;
}

// The collapsed rail shows each tab's icon alone; its title stays available as
// the tooltip Windows Terminal already gives every tab.
std::wstring CollapsedTabTemplateXaml() {
    std::wstring xaml{kTabViewItemTemplateXaml};
    xaml = ReplaceOnce(xaml, LR"(Padding="10,0,4,0")", LR"(Padding="12,0,0,0")");
    xaml = ReplaceOnce(xaml, LR"(Margin="0,0,10,0")", LR"(Margin="0")");
    xaml = ReplaceOnce(xaml, LR"(<ContentPresenter x:Name="ContentPresenter")",
                       LR"(<ContentPresenter x:Name="ContentPresenter" Visibility="Collapsed")");
    xaml = ReplaceOnce(xaml, LR"(<Button x:Name="CloseButton")",
                       LR"(<Button x:Name="CloseButton" Visibility="Collapsed")");
    return xaml;
}

bool EnsureTemplates(ThreadContext& context) {
    if (!context.tabTemplate && !context.tabTemplateValidated) {
        context.tabTemplate = LoadXaml<wuxc::ControlTemplate>(kTabViewItemTemplateXaml);
    }
    if (!context.collapsedTabTemplate && !context.collapsedTabTemplateValidated) {
        context.collapsedTabTemplate =
            LoadXaml<wuxc::ControlTemplate>(CollapsedTabTemplateXaml());
    }
    if (!context.verticalItemsPanel) {
        context.verticalItemsPanel =
            LoadXaml<wuxc::ItemsPanelTemplate>(kVerticalItemsPanelXaml);
    }
    return context.verticalItemsPanel != nullptr;
}

// `itemTemplate` null means back to the default style's template.
void SetTabViewItemTemplate(muxc::TabViewItem const& item,
                            wuxc::ControlTemplate const& itemTemplate) {
    if (itemTemplate ? item.Template() == itemTemplate
                     : item.ReadLocalValue(wuxc::Control::TemplateProperty()) ==
                           wux::DependencyProperty::UnsetValue()) {
        return;
    }

    if (wuxm::VisualTreeHelper::GetChildrenCount(item) > 0) {
        ReleaseTemplateContent(item, {L"ContentPresenter", L"IconControl"});
    }

    if (itemTemplate) {
        item.Template(itemTemplate);
    } else {
        item.ClearValue(wuxc::Control::TemplateProperty());
    }
}

void SetAllTabViewItemTemplates(muxc::TabView const& tabView,
                                wuxc::ControlTemplate const& itemTemplate) {
    for (auto const& item : tabView.TabItems()) {
        if (auto tabViewItem = item.try_as<muxc::TabViewItem>()) {
            SetTabViewItemTemplate(tabViewItem, itemTemplate);
        }
    }
}

// A template that fails to instantiate (a theme resource a future WinUI
// renames, say) would otherwise throw from inside a layout pass and take the
// terminal down. So each tab template is first tried on one tab, synchronously,
// and dropped for the thread if it fails; the tabs then keep the stock look.
bool ValidateTabViewItemTemplate(muxc::TabView const& tabView,
                                 wuxc::ControlTemplate& itemTemplate,
                                 bool& validated) {
    if (validated || !itemTemplate) {
        return itemTemplate != nullptr;
    }

    muxc::TabViewItem probe{nullptr};
    for (auto const& item : tabView.TabItems()) {
        if ((probe = item.try_as<muxc::TabViewItem>())) {
            break;
        }
    }
    if (!probe) {
        return false;  // tried again with the next window
    }

    auto previous = probe.ReadLocalValue(wuxc::Control::TemplateProperty());
    try {
        SetTabViewItemTemplate(probe, itemTemplate);
        probe.ApplyTemplate();
        validated = true;
        return true;
    } catch (winrt::hresult_error const& ex) {
        Wh_Log(L"Tab template failed, keeping the stock tabs: %08X %s", ex.code().value,
               ex.message().c_str());
    }

    try {
        SetTabViewItemTemplate(probe, previous.try_as<wuxc::ControlTemplate>());
        probe.ApplyTemplate();
    } catch (winrt::hresult_error const& ex) {
        Wh_Log(L"Restoring the tab template failed: %08X", ex.code().value);
    }
    itemTemplate = nullptr;
    validated = true;
    return false;
}

// The template a window's tabs should have right now, or null for the stock one.
wuxc::ControlTemplate CurrentTabTemplate(ThreadContext& context, WindowState const& window) {
    if (LooksCollapsed(window)) {
        return context.collapsedTabTemplateValidated ? context.collapsedTabTemplate : nullptr;
    }
    return context.tabTemplateValidated ? context.tabTemplate : nullptr;
}

// Whether the window's template has yet to be tried (it needs a tab to try it on).
bool CurrentTabTemplateUntried(ThreadContext& context, WindowState const& window) {
    return LooksCollapsed(window)
               ? context.collapsedTabTemplate && !context.collapsedTabTemplateValidated
               : context.tabTemplate && !context.tabTemplateValidated;
}

// Tries the window's template on one of its tabs if that hasn't happened yet on
// this thread, then gives every tab of the window the template to use.
void ApplyCurrentTabTemplate(ThreadContext& context,
                             WindowState const& window,
                             muxc::TabView const& tabView) {
    bool usable = LooksCollapsed(window)
                      ? ValidateTabViewItemTemplate(tabView, context.collapsedTabTemplate,
                                                    context.collapsedTabTemplateValidated)
                      : ValidateTabViewItemTemplate(tabView, context.tabTemplate,
                                                    context.tabTemplateValidated);
    if (usable) {
        SetAllTabViewItemTemplates(tabView, CurrentTabTemplate(context, window));
    }
}

////////////////////////////////////////////////////////////////////////////////
// The toggle in each tab's context menu

constexpr wchar_t kMenuTag[] = L"windows-terminal-vertical-tabs";

void RequestToggleVertical();

PCWSTR MenuEntryText() {
    return g_vertical ? L"Turn off vertical tabs" : L"Turn on vertical tabs";
}

PCWSTR SidebarGlyph() {
    return g_sidebarOnRight ? L"\xE90D" : L"\xE90C";  // DockRight / DockLeft
}

void LabelMenuEntry(wuxc::MenuFlyoutItem const& item) {
    item.Text(MenuEntryText());
    wuxc::FontIcon icon;
    icon.Glyph(SidebarGlyph());
    item.Icon(icon);
}

bool IsOwnMenuEntry(wux::DependencyObject const& object) {
    auto element = object.try_as<wux::FrameworkElement>();
    return element && winrt::unbox_value_or<winrt::hstring>(element.Tag(), L"") == kMenuTag;
}

// Adds "Turn on/off vertical tabs" to a tab's context menu, in a group of its
// own just above Terminal's Close group, as Edge places it.
void AddMenuEntry(WindowState& window, muxc::TabViewItem const& tab) {
    auto flyout = tab.ContextFlyout().try_as<wuxc::MenuFlyout>();
    if (!flyout) {
        return;
    }
    auto items = flyout.Items();
    for (auto const& item : items) {
        if (IsOwnMenuEntry(item)) {
            if (auto menuItem = item.try_as<wuxc::MenuFlyoutItem>()) {
                LabelMenuEntry(menuItem);
            }
            return;
        }
    }

    uint32_t insertAt = items.Size();
    for (uint32_t i = items.Size(); i > 0; i--) {
        if (items.GetAt(i - 1).try_as<wuxc::MenuFlyoutSeparator>()) {
            insertAt = i - 1;
            break;
        }
    }

    wuxc::MenuFlyoutSeparator separator;
    separator.Tag(winrt::box_value(kMenuTag));

    wuxc::MenuFlyoutItem menuItem;
    menuItem.Tag(winrt::box_value(kMenuTag));
    LabelMenuEntry(menuItem);
    auto clickToken = menuItem.Click([](wf::IInspectable const&, wux::RoutedEventArgs const&) {
        RequestToggleVertical();
    });

    items.InsertAt(insertAt, menuItem);
    items.InsertAt(insertAt, separator);
    window.menuEntries.push_back({flyout, menuItem, clickToken});
}

void UpdateMenuEntries(WindowState& window) {
    for (auto const& entry : window.menuEntries) {
        LabelMenuEntry(entry.item);
    }
}

// Takes the mod's entry back out of one flyout and drops the handler. Each
// entry separately, so one failure doesn't leave the others' handlers behind.
void RemoveMenuEntry(WindowState::MenuEntry const& entry) {
    try {
        entry.item.Click(entry.clickToken);
    } catch (winrt::hresult_error const& ex) {
        Wh_Log(L"Revoking a menu handler failed: %08X", ex.code().value);
    }
    try {
        auto items = entry.flyout.Items();
        for (uint32_t i = items.Size(); i > 0; i--) {
            if (IsOwnMenuEntry(items.GetAt(i - 1))) {
                items.RemoveAt(i - 1);
            }
        }
    } catch (winrt::hresult_error const& ex) {
        Wh_Log(L"Removing a menu entry failed: %08X", ex.code().value);
    }
}

// Closed tabs never come back (Terminal re-creates a moved tab), so an entry
// whose flyout no longer belongs to any tab of the window is released, rather
// than keeping the closed tab's whole menu alive for the rest of the session.
void SweepMenuEntries(WindowState& window) {
    auto tabView = window.tabView.get();
    if (!tabView) {
        return;
    }
    std::vector<wux::Controls::Primitives::FlyoutBase> live;
    for (auto const& item : tabView.TabItems()) {
        if (auto tab = item.try_as<muxc::TabViewItem>()) {
            if (auto flyout = tab.ContextFlyout()) {
                live.push_back(flyout);
            }
        }
    }
    std::erase_if(window.menuEntries, [&](WindowState::MenuEntry const& entry) {
        bool alive = std::any_of(live.begin(), live.end(),
                                 [&](auto const& flyout) { return flyout == entry.flyout; });
        if (!alive) {
            RemoveMenuEntry(entry);
        }
        return !alive;
    });
}

void RemoveMenuEntries(WindowState& window) {
    for (auto const& entry : window.menuEntries) {
        RemoveMenuEntry(entry);
    }
    window.menuEntries.clear();
}

////////////////////////////////////////////////////////////////////////////////
// The vertical strip

void QueueWork();

// Horizontal scrolling off, tabs back to automatic width, the list unclamped.
// Rate limited, so that if some future TabView keeps insisting, the mod gives up
// on that window instead of fighting it forever.
void ReassertVerticalStrip(WindowState& window) {
    auto listView = window.listView.get();
    auto tabView = window.tabView.get();
    if (!listView || !tabView || !window.stripApplied ||
        wuxc::ScrollViewer::GetHorizontalScrollBarVisibility(listView) ==
            wuxc::ScrollBarVisibility::Disabled) {
        return;
    }

    ULONGLONG now = GetTickCount64();
    if (now - window.reassertPeriodStart > 2000) {
        window.reassertPeriodStart = now;
        window.reassertCount = 0;
    }
    if (++window.reassertCount > 20) {
        if (window.reassertCount == 21) {
            Wh_Log(L"TabView keeps re-enabling horizontal scrolling, giving up");
        }
        return;
    }

    wuxc::ScrollViewer::SetHorizontalScrollBarVisibility(listView,
                                                         wuxc::ScrollBarVisibility::Disabled);
    listView.MaxWidth(std::numeric_limits<double>::infinity());
    for (auto const& item : tabView.TabItems()) {
        if (auto tabViewItem = item.try_as<muxc::TabViewItem>()) {
            if (!std::isnan(tabViewItem.Width())) {
                tabViewItem.Width(std::numeric_limits<double>::quiet_NaN());
            }
        }
    }
}

WindowState* FindWindowByTabView(ThreadContext& context, muxc::TabView const& tabView);

template <typename Step>
void Guarded(PCWSTR what, Step&& step);

// The new-tab dropdown is placed for a button at the top of the window
// (BottomEdgeAlignedLeft). In the sidebar the button follows the last tab, so
// it opens towards the free space: down from a button in the top half of the
// window, up from one in the bottom half, decided each time it opens. Terminal
// rebuilds the flyout on every settings reload, so the handler moves with it.
wuxc::Primitives::FlyoutPlacementMode NewTabFlyoutPlacement(WindowState const& window) {
    bool up = true;
    try {
        auto button = window.newTabButton.get();
        auto root = button ? button.XamlRoot() : nullptr;
        if (root && root.Size().Height > 0) {
            auto top = button.TransformToVisual(nullptr).TransformPoint(wf::Point{0, 0}).Y;
            up = top + button.ActualHeight() / 2 > root.Size().Height / 2;
        }
    } catch (...) {
    }
    using Mode = wuxc::Primitives::FlyoutPlacementMode;
    if (up) {
        return g_sidebarOnRight ? Mode::TopEdgeAlignedRight : Mode::TopEdgeAlignedLeft;
    }
    return g_sidebarOnRight ? Mode::BottomEdgeAlignedRight : Mode::BottomEdgeAlignedLeft;
}

void RestoreNewTabFlyoutPlacement(WindowState& window) {
    if (auto flyout = std::exchange(window.newTabFlyout, nullptr)) {
        if (auto token = std::exchange(window.newTabFlyoutOpeningToken, {})) {
            flyout.Opening(token);
        }
        auto original = std::exchange(window.newTabFlyoutPlacement, nullptr);
        if (original == wux::DependencyProperty::UnsetValue()) {
            flyout.ClearValue(wuxc::Primitives::FlyoutBase::PlacementProperty());
        } else {
            flyout.SetValue(wuxc::Primitives::FlyoutBase::PlacementProperty(), original);
        }
    }
}

void PlaceNewTabFlyout(WindowState& window) {
    auto button = window.newTabButton.get();
    auto flyout = button ? button.Flyout().try_as<wuxc::Primitives::FlyoutBase>() : nullptr;
    if (flyout != window.newTabFlyout) {
        RestoreNewTabFlyoutPlacement(window);
    }
    if (!flyout) {
        return;
    }
    if (!window.newTabFlyout) {
        window.newTabFlyout = flyout;
        window.newTabFlyoutPlacement =
            flyout.ReadLocalValue(wuxc::Primitives::FlyoutBase::PlacementProperty());
        window.newTabFlyoutOpeningToken = flyout.Opening(
            [weakTabView = window.tabView](wf::IInspectable const& sender, wf::IInspectable const&) {
                try {
                    auto context = GetThreadContext(false);
                    auto tabView = weakTabView.get();
                    auto window = context && tabView ? FindWindowByTabView(*context, tabView) : nullptr;
                    auto flyout = sender.try_as<wuxc::Primitives::FlyoutBase>();
                    if (g_active && window && flyout) {
                        flyout.Placement(NewTabFlyoutPlacement(*window));
                    }
                } catch (...) {
                }
            });
    }
    flyout.Placement(NewTabFlyoutPlacement(window));
}

// In the collapsed rail the split button keeps its "+" and drops the dropdown
// arrow, which the rail has no room for; expanding (or peeking) brings it back.
void FitNewTabButton(WindowState& window, muxc::SplitButton const& button) {
    if (wuxm::VisualTreeHelper::GetChildrenCount(button) == 0) {
        return;
    }
    auto root = wuxm::VisualTreeHelper::GetChild(button, 0).try_as<wuxc::Grid>();
    if (!root || root.ColumnDefinitions().Size() != 3) {
        return;
    }
    bool rail = LooksCollapsed(window);
    for (uint32_t column : {1u, 2u}) {
        auto definition = root.ColumnDefinitions().GetAt(column);
        if (rail) {
            SetValueSaved(window, definition, wuxc::ColumnDefinition::WidthProperty(),
                          winrt::box_value(wux::GridLengthHelper::FromPixels(0)));
        } else {
            RestoreSavedValue(window, definition, wuxc::ColumnDefinition::WidthProperty());
        }
    }
    if (auto secondary = FindDescendant(
            root, [](wux::DependencyObject const& e) { return HasName(e, L"SecondaryButton"); },
            nullptr, 4)) {
        if (rail) {
            SetValueSaved(window, secondary, wux::UIElement::VisibilityProperty(),
                          winrt::box_value(wux::Visibility::Collapsed));
        } else {
            RestoreSavedValue(window, secondary, wux::UIElement::VisibilityProperty());
        }
    }
}

void AttachNewTabButton(WindowState& window, wux::DependencyObject const& footer) {
    if (window.newTabFlyoutToken) {
        return;
    }
    auto button = FindDescendant(
                      footer,
                      [](wux::DependencyObject const& element) {
                          return HasName(element, L"NewTabButton");
                      },
                      nullptr, 8)
                      .try_as<muxc::SplitButton>();
    if (!button) {
        return;
    }
    window.newTabButton = winrt::make_weak(button);
    PlaceNewTabFlyout(window);
    FitNewTabButton(window, button);
    window.newTabFlyoutToken = button.RegisterPropertyChangedCallback(
        muxc::SplitButton::FlyoutProperty(),
        [](wux::DependencyObject const& sender, wux::DependencyProperty const&) {
            auto context = GetThreadContext(false);
            if (!context || !g_active) {
                return;
            }
            for (auto& window : context->windows) {
                if (window->newTabButton.get() == sender) {
                    PlaceNewTabFlyout(*window);
                }
            }
        });
}

void DetachNewTabButton(WindowState& window) {
    // The flyout's Opening handler first: it points into this module.
    Guarded(L"Restoring the new-tab dropdown", [&] { RestoreNewTabFlyoutPlacement(window); });
    Guarded(L"Revoking the new-tab callback", [&] {
        auto button = window.newTabButton.get();
        if (button && window.newTabFlyoutToken) {
            button.UnregisterPropertyChangedCallback(muxc::SplitButton::FlyoutProperty(),
                                                     window.newTabFlyoutToken);
        }
    });
    window.newTabFlyoutToken = 0;
    window.newTabFlyout = nullptr;
    window.newTabFlyoutOpeningToken = {};
}

// The collapse/expand button, in the sidebar's top row, styled like the tabs.
constexpr std::wstring_view kCollapseButtonXaml = LR"(
<Button
    xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
    Width="32"
    Height="32"
    Padding="0"
    VerticalAlignment="Center"
    CornerRadius="4"
    BorderThickness="0"
    IsTabStop="False"
    FontFamily="Segoe Fluent Icons, Segoe MDL2 Assets"
    FontSize="16"
    Background="Transparent"
    Foreground="{ThemeResource TabViewItemHeaderForeground}">
    <Button.Template>
        <ControlTemplate TargetType="Button">
            <ContentPresenter x:Name="ContentPresenter"
                              Background="{TemplateBinding Background}"
                              CornerRadius="{TemplateBinding CornerRadius}"
                              Content="{TemplateBinding Content}"
                              HorizontalContentAlignment="Center"
                              VerticalContentAlignment="Center">
                <VisualStateManager.VisualStateGroups>
                    <VisualStateGroup x:Name="CommonStates">
                        <VisualState x:Name="Normal" />
                        <VisualState x:Name="PointerOver">
                            <VisualState.Setters>
                                <Setter Target="ContentPresenter.Background" Value="{ThemeResource TabViewItemHeaderBackgroundPointerOver}" />
                            </VisualState.Setters>
                        </VisualState>
                        <VisualState x:Name="Pressed">
                            <VisualState.Setters>
                                <Setter Target="ContentPresenter.Background" Value="{ThemeResource TabViewItemHeaderBackgroundPressed}" />
                            </VisualState.Setters>
                        </VisualState>
                    </VisualStateGroup>
                </VisualStateManager.VisualStateGroups>
            </ContentPresenter>
        </ControlTemplate>
    </Button.Template>
</Button>
)";

void RequestToggleCollapsed(winrt::weak_ref<muxc::TabView> const& tabView);

// Centred in the rail, at the sidebar's edge otherwise. While a collapsed rail
// peeks open the button still offers to expand it, which keeps it open.
void StyleCollapseButton(WindowState const& window, wuxc::Button const& button) {
    button.HorizontalAlignment(LooksCollapsed(window)
                                   ? wux::HorizontalAlignment::Center
                                   : (g_sidebarOnRight ? wux::HorizontalAlignment::Right
                                                       : wux::HorizontalAlignment::Left));
    button.Margin(LooksCollapsed(window) ? wux::Thickness{0, 4, 0, 0}
                                         : wux::Thickness{8, 4, 8, 0});
    PCWSTR label = window.collapsed ? L"Expand tabs" : L"Collapse tabs";
    wuxc::ToolTipService::SetToolTip(button, winrt::box_value(label));
    wux::Automation::AutomationProperties::SetName(button, label);
}

void AddCollapseButton(WindowState& window, wuxc::Grid const& containerGrid) {
    if (window.collapseButton.get()) {
        return;
    }
    auto button = LoadXaml<wuxc::Button>(kCollapseButtonXaml);
    if (!button) {
        return;
    }
    button.Content(winrt::box_value(g_sidebarOnRight ? L"\xE90D" : L"\xE90C"));
    StyleCollapseButton(window, button);
    wuxc::Grid::SetRow(button, 0);

    // Collapses this window only.
    window.collapseButtonClickToken =
        button.Click([tabView = window.tabView](wf::IInspectable const&,
                                                wux::RoutedEventArgs const&) {
            RequestToggleCollapsed(tabView);
        });
    containerGrid.Children().Append(button);
    window.collapseButton = winrt::make_weak(button);
}

// Behind everything else in the strip, spanning all its rows. Two layers: the
// outer keeps its theme resources, and the mod never writes its brushes, so
// they follow the window's theme; the inner carries the sidebar's own colour
// when that is opaque, and is empty otherwise.
constexpr wchar_t kPeekBackdropXaml[] = LR"(
<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        Visibility="Collapsed"
        BackgroundSizing="OuterBorderEdge"
        Background="{ThemeResource SolidBackgroundFillColorBaseBrush}"
        BorderBrush="{ThemeResource CardStrokeColorDefaultBrush}">
    <Border />
</Border>
)";

void AddPeekBackdrop(WindowState& window, wuxc::Grid const& containerGrid) {
    if (window.peekBackdrop.get()) {
        return;
    }
    auto backdrop = LoadXaml<wuxc::Border>(kPeekBackdropXaml);
    if (!backdrop) {
        // Without the theme resources: a plain fill, picked for the theme each
        // time the rail peeks (see SetPeek).
        backdrop = wuxc::Border{};
        backdrop.Visibility(wux::Visibility::Collapsed);
        backdrop.BackgroundSizing(wuxc::BackgroundSizing::OuterBorderEdge);
        backdrop.Child(wuxc::Border{});
        window.peekBackdropPlain = true;
    }
    wuxc::Grid::SetRowSpan(backdrop, 5);
    containerGrid.Children().InsertAt(0, backdrop);
    window.peekBackdrop = winrt::make_weak(backdrop);
}

void RemovePeekBackdrop(WindowState& window) {
    if (auto backdrop = window.peekBackdrop.get()) {
        if (auto grid = window.containerGrid.get()) {
            uint32_t index = 0;
            if (grid.Children().IndexOf(backdrop, index)) {
                grid.Children().RemoveAt(index);
            }
        }
    }
    window.peekBackdrop = nullptr;
    window.peekBackdropPlain = false;
}

void RemoveCollapseButton(WindowState& window) {
    auto button = window.collapseButton.get();
    if (button) {
        button.Click(window.collapseButtonClickToken);
        if (auto grid = window.containerGrid.get()) {
            uint32_t index = 0;
            if (grid.Children().IndexOf(button, index)) {
                grid.Children().RemoveAt(index);
            }
        }
    }
    window.collapseButton = nullptr;
    window.collapseButtonClickToken = {};
}

// Finds a part of the TabView's own template, without looking inside the
// tabs, which have parts of their own.
wux::FrameworkElement FindStripPart(wux::DependencyObject const& root,
                                    std::wstring_view name) {
    auto found = FindDescendant(
        root, [&](wux::DependencyObject const& element) { return HasName(element, name); },
        [](wux::DependencyObject const& element) {
            return !element.try_as<muxc::TabViewItem>();
        },
        16);
    return found ? found.try_as<wux::FrameworkElement>() : nullptr;
}

// Rearranges the stock TabView template (WinUI 2.8) into a column: the
// collapse button and the tab strip header on top, the tab list filling the
// middle, the strip footer (Windows Terminal's new-tab split button) at the
// bottom.
//
// TabView::UpdateTabWidths sizes tabs for a horizontal strip: it gives every tab
// a fixed width and turns horizontal scrolling on when they don't fit the width
// left over after the header, add button and footer. Giving the footer a
// minimum width of the whole sidebar leaves no width over, which is the case
// UpdateTabWidths skips, and then it only resets each tab to automatic width.
// The parts of the TabView template the vertical strip is built from. All
// null unless the template is the WinUI 2.8 one the mod knows.
struct StripParts {
    wuxc::Grid templateRoot{nullptr};
    wuxc::Grid containerGrid{nullptr};
    wuxc::ListView listView{nullptr};

    explicit operator bool() const { return templateRoot && containerGrid && listView; }
};

StripParts FindStripParts(muxc::TabView const& tabView) {
    if (wuxm::VisualTreeHelper::GetChildrenCount(tabView) == 0) {
        tabView.ApplyTemplate();
    }

    StripParts parts;
    parts.templateRoot =
        wuxm::VisualTreeHelper::GetChildrenCount(tabView) > 0
            ? wuxm::VisualTreeHelper::GetChild(tabView, 0).try_as<wuxc::Grid>()
            : nullptr;
    parts.containerGrid = FindStripPart(tabView, L"TabContainerGrid").try_as<wuxc::Grid>();
    parts.listView = FindStripPart(tabView, L"TabListView").try_as<wuxc::ListView>();
#ifdef WTVT_TEST_HOOKS
    // Lets a test exercise the unfamiliar-template fallback.
    if (GetEnvironmentVariableW(L"WTVT_TEST_UNKNOWN_TEMPLATE", nullptr, 0)) {
        parts.listView = nullptr;
    }
#endif
    if (!parts) {
        return {};
    }
    return parts;
}

WindowState* FindWindowByTabView(ThreadContext& context, muxc::TabView const& tabView);

// The strip footer holding Windows Terminal's new-tab button.
void StyleFooter(WindowState& window) {
    auto footer = window.footer.get();
    if (!footer) {
        return;
    }
    double width = SidebarWidth(window);
    bool rail = LooksCollapsed(window);
    SetValueSaved(window, footer, wux::FrameworkElement::MinWidthProperty(),
                  winrt::box_value(width));
    SetValueSaved(window, footer, wux::FrameworkElement::MarginProperty(),
                  winrt::box_value(rail ? wux::Thickness{0, 0, 0, 4} : wux::Thickness{4, 0, 4, 4}));
    SetValueSaved(window, footer, wuxc::ContentPresenter::HorizontalContentAlignmentProperty(),
                  winrt::box_value(rail ? wux::HorizontalAlignment::Center
                                        : wux::HorizontalAlignment::Stretch));
    // UpdateTabWidths reads the footer's desired size from inside
    // TabView::MeasureOverride, before the footer is measured again, so it has
    // to be current before the next layout pass.
    footer.Measure(wf::Size{static_cast<float>(width), std::numeric_limits<float>::infinity()});
}

// The tab list's row sizes to its content, so the new-tab button follows the
// last tab; the list may grow only as tall as the rows around it leave room for,
// after which it scrolls. Measured, not read from the rows, because right after
// the strip is rearranged the new rows have no sizes yet.
void FitTabListHeight(WindowState& window) {
    auto containerGrid = window.containerGrid.get();
    auto listView = window.listView.get();
    if (!containerGrid || !listView) {
        return;
    }
    double height = containerGrid.ActualHeight();
    if (height <= 0) {
        return;  // not laid out yet; the size handler comes back to it
    }
    double rows[4] = {};
    wf::Size available{static_cast<float>(SidebarWidth(window)),
                       std::numeric_limits<float>::infinity()};
    for (auto const& child : containerGrid.Children()) {
        auto element = child.try_as<wux::FrameworkElement>();
        if (!element || element == listView ||
            element.Visibility() != wux::Visibility::Visible) {
            continue;
        }
        int row = wuxc::Grid::GetRow(element);
        if (row < 0 || row > 3 || row == 2 || wuxc::Grid::GetRowSpan(element) > 1) {
            continue;
        }
        element.Measure(available);
        rows[row] = std::max<double>(rows[row], element.DesiredSize().Height);
    }
    double maxHeight = std::max(0.0, height - rows[0] - rows[1] - rows[3]);
    if (std::abs(listView.MaxHeight() - maxHeight) >= 0.5) {
        SetValueSaved(window, listView, wux::FrameworkElement::MaxHeightProperty(),
                      winrt::box_value(maxHeight));
    }
}

bool ApplyVerticalStrip(ThreadContext& context,
                        WindowState& window,
                        muxc::TabView const& tabView) {
    auto parts = FindStripParts(tabView);
    if (!parts) {
        return false;
    }
    auto templateRoot = parts.templateRoot;
    auto containerGrid = parts.containerGrid;
    auto listView = parts.listView;

    // A TabView that has never been laid out (hidden because only one tab is
    // open, say) has an untemplated list: template it now, so its scroll
    // viewer and items presenter exist for the lookups below.
    listView.ApplyTemplate();

    // The template's two rows are the strip and the (unused) tab content;
    // the strip takes the height.
    auto rootRows = templateRoot.RowDefinitions();
    if (rootRows.Size() >= 2) {
        SetValueSaved(window, rootRows.GetAt(0), wuxc::RowDefinition::HeightProperty(),
                      winrt::box_value(wux::GridLengthHelper::FromValueAndType(
                          1, wux::GridUnitType::Star)));
        SetValueSaved(window, rootRows.GetAt(1), wuxc::RowDefinition::HeightProperty(),
                      winrt::box_value(wux::GridLengthHelper::FromValueAndType(
                          1, wux::GridUnitType::Auto)));
    }

    window.containerGrid = winrt::make_weak(containerGrid);
    for (auto const& column : containerGrid.ColumnDefinitions()) {
        window.removedColumns.push_back(column);
    }
    containerGrid.ColumnDefinitions().Clear();

    // Collapse button, header, tab list, footer, and the rest of the height
    // below them: the new-tab button sits right under the last tab, as in a
    // browser, and the list scrolls once it fills the sidebar (FitTabListHeight).
    for (auto unit : {wux::GridUnitType::Auto, wux::GridUnitType::Auto,
                      wux::GridUnitType::Auto, wux::GridUnitType::Auto,
                      wux::GridUnitType::Star}) {
        wuxc::RowDefinition row;
        row.Height(wux::GridLengthHelper::FromValueAndType(1, unit));
        containerGrid.RowDefinitions().Append(row);
        window.addedRows.push_back(row);
    }

    for (auto const& child : containerGrid.Children()) {
        auto element = child.try_as<wux::FrameworkElement>();
        if (!element) {
            continue;
        }
        auto name = element.Name();
        if (name == L"LeftBottomBorderLine" || name == L"RightBottomBorderLine") {
            SetValueSaved(window, element, wux::UIElement::OpacityProperty(),
                          winrt::box_value(0.0));
            continue;
        }

        int row = 3;  // the footer, and the add button Windows Terminal hides
        if (name == L"LeftContentPresenter") {
            row = 1;
            SetValueSaved(window, element, wux::FrameworkElement::MarginProperty(),
                          winrt::box_value(wux::Thickness{8, 4, 8, 0}));
        } else if (name == L"TabListView") {
            row = 2;
        } else if (name == L"RightContentPresenter") {
            window.footer = winrt::make_weak(element);
            StyleFooter(window);
            AttachNewTabButton(window, element);
        }
        SetValueSaved(window, element, wuxc::Grid::RowProperty(), winrt::box_value(row));
    }

    AddCollapseButton(window, containerGrid);
    AddPeekBackdrop(window, containerGrid);

    // The list's scroll viewer takes these through template bindings.
    SetValueSaved(window, listView, wuxc::ScrollViewer::HorizontalScrollBarVisibilityProperty(),
                  winrt::box_value(wuxc::ScrollBarVisibility::Disabled));
    SetValueSaved(window, listView, wuxc::ScrollViewer::HorizontalScrollModeProperty(),
                  winrt::box_value(wuxc::ScrollMode::Disabled));
    SetValueSaved(window, listView, wuxc::ScrollViewer::IsHorizontalRailEnabledProperty(),
                  winrt::box_value(false));
    SetValueSaved(window, listView, wuxc::ScrollViewer::VerticalScrollBarVisibilityProperty(),
                  winrt::box_value(wuxc::ScrollBarVisibility::Auto));
    SetValueSaved(window, listView, wuxc::ScrollViewer::VerticalScrollModeProperty(),
                  winrt::box_value(wuxc::ScrollMode::Enabled));
    SetValueSaved(window, listView, wuxc::ScrollViewer::IsVerticalRailEnabledProperty(),
                  winrt::box_value(true));
    // The list's padding is template-bound to the TabView's; set it there, so
    // that putting it back restores the binding rather than clearing it.
    SetValueSaved(window, tabView, wuxc::Control::PaddingProperty(),
                  winrt::box_value(wux::Thickness{4, 4, 4, 4}));

    // The strip's bottom border lines, drawn under a horizontal strip by the
    // list's scroll viewer and by the header and footer of its items presenter.
    std::function<void(wux::DependencyObject const&, int)> hideLines =
        [&](wux::DependencyObject const& element, int depth) {
            int count = wuxm::VisualTreeHelper::GetChildrenCount(element);
            for (int i = 0; i < count && depth < 12; i++) {
                auto child = wuxm::VisualTreeHelper::GetChild(element, i);
                if (child.try_as<muxc::TabViewItem>()) {
                    continue;
                }
                if (HasName(child, L"LeftBottomBorderLine") ||
                    HasName(child, L"RightBottomBorderLine")) {
                    SetValueSaved(window, child, wux::UIElement::OpacityProperty(),
                                  winrt::box_value(0.0));
                }
                hideLines(child, depth + 1);
            }
        };
    hideLines(listView, 0);

    if (auto panel = listView.ItemsPanelRoot().try_as<wuxc::ItemsStackPanel>()) {
        SetValueSaved(window, panel, wuxc::ItemsStackPanel::OrientationProperty(),
                      winrt::box_value(wuxc::Orientation::Vertical));
    } else {
        SetValueSaved(window, listView, wuxc::ItemsControl::ItemsPanelProperty(),
                      context.verticalItemsPanel);
    }

    SetValueSaved(window, listView, wux::FrameworkElement::VerticalAlignmentProperty(),
                  winrt::box_value(wux::VerticalAlignment::Stretch));
    SetValueSaved(window, listView, wux::FrameworkElement::MaxWidthProperty(),
                  winrt::box_value(std::numeric_limits<double>::infinity()));

    // Should TabView turn horizontal scrolling back on anyway, the tabs get
    // measured at their full title width and the items presenter grows past
    // the sidebar; undo it when that happens.
    window.listView = winrt::make_weak(listView);
    FitTabListHeight(window);
    window.containerSizeChangedToken = containerGrid.SizeChanged(
        [weakTabView = winrt::make_weak(tabView)](wf::IInspectable const&,
                                                  wux::SizeChangedEventArgs const&) {
            auto context = GetThreadContext(false);
            auto tabView = weakTabView.get();
            if (!context || !tabView || !g_active) {
                return;
            }
            if (auto window = FindWindowByTabView(*context, tabView)) {
                Guarded(L"Fitting the tab list", [&] { FitTabListHeight(*window); });
            }
        });

    if (auto itemsPresenter = FindStripPart(listView, L"TabsItemsPresenter")) {
        window.itemsPresenter = winrt::make_weak(itemsPresenter);
        window.itemsPresenterSizeChangedToken = itemsPresenter.SizeChanged(
            [weakTabView = winrt::make_weak(tabView)](wf::IInspectable const&,
                                                      wux::SizeChangedEventArgs const&) {
                auto context = GetThreadContext(false);
                auto tabView = weakTabView.get();
                if (!context || !tabView || !g_active) {
                    return;
                }
                for (auto& window : context->windows) {
                    if (window->tabView.get() == tabView) {
                        ReassertVerticalStrip(*window);
                    }
                }
            });
    }

    window.stripApplied = true;

    ApplyCurrentTabTemplate(context, window, tabView);
    return true;
}

// Runs `step`, logging instead of propagating a failure, so that one failed
// step of a teardown doesn't skip the ones after it.
template <typename Step>
void Guarded(PCWSTR what, Step&& step) {
    try {
        step();
    } catch (winrt::hresult_error const& ex) {
        Wh_Log(L"%s failed: %08X %s", what, ex.code().value, ex.message().c_str());
    }
}

void RemoveVerticalStrip(WindowState& window) {
    // Handlers first: they point into this module, and must be gone even if
    // restoring the layout fails.
    Guarded(L"Revoking the size handler", [&] {
        auto itemsPresenter = window.itemsPresenter.get();
        if (itemsPresenter && window.itemsPresenterSizeChangedToken) {
            itemsPresenter.SizeChanged(window.itemsPresenterSizeChangedToken);
        }
    });
    window.itemsPresenterSizeChangedToken = {};
    Guarded(L"Revoking the strip size handler", [&] {
        auto containerGrid = window.containerGrid.get();
        if (containerGrid && window.containerSizeChangedToken) {
            containerGrid.SizeChanged(window.containerSizeChangedToken);
        }
    });
    window.containerSizeChangedToken = {};
    window.footer = nullptr;
    Guarded(L"Detaching the new-tab button", [&] { DetachNewTabButton(window); });
    Guarded(L"Removing the collapse button", [&] { RemoveCollapseButton(window); });
    Guarded(L"Removing the peek backdrop", [&] { RemovePeekBackdrop(window); });

    Guarded(L"Restoring tab templates", [&] {
        if (auto tabView = window.tabView.get()) {
            SetAllTabViewItemTemplates(tabView, nullptr);
        }
    });

    Guarded(L"Restoring the strip's grid", [&] {
        if (auto containerGrid = window.containerGrid.get()) {
            auto rows = containerGrid.RowDefinitions();
            for (auto const& row : window.addedRows) {
                uint32_t index = 0;
                if (rows.IndexOf(row, index)) {
                    rows.RemoveAt(index);
                }
            }
            auto columns = containerGrid.ColumnDefinitions();
            if (columns.Size() == 0) {
                for (auto const& column : window.removedColumns) {
                    columns.Append(column);
                }
            }
        }
    });
    window.addedRows.clear();
    window.removedColumns.clear();
    window.stripApplied = false;
}

////////////////////////////////////////////////////////////////////////////////
// Layout

void UpdateContentMargins(WindowState& window) {
    auto tabView = window.tabView.get();
    bool sidebarVisible =
        tabView && tabView.Visibility() == wux::Visibility::Visible;
    double width = sidebarVisible ? DockedWidth(window) : 0.0;
    bool onRight = g_sidebarOnRight;

    for (auto const& [weakElement, original] : window.shiftedContent) {
        if (auto element = weakElement.get()) {
            auto margin = original;
            if (onRight) {
                margin.Right += width;
            } else {
                margin.Left += width;
            }
            element.Margin(margin);
        }
    }
}

void ApplySidebarLayout(WindowState& window) {
    auto tabRow = window.tabRow.get();
    auto pageRoot = window.pageRoot.get();
    auto tabView = window.tabView.get();
    if (!tabRow || !pageRoot || !tabView) {
        return;
    }

    bool onRight = g_sidebarOnRight;
    int rowCount = std::max<int>(1, pageRoot.RowDefinitions().Size());

    SetValueSaved(window, tabRow, wuxc::Grid::RowProperty(), winrt::box_value(0));
    SetValueSaved(window, tabRow, wuxc::Grid::RowSpanProperty(),
                  winrt::box_value(rowCount));
    SetValueSaved(window, tabRow, wux::FrameworkElement::WidthProperty(),
                  winrt::box_value(SidebarWidth(window)));
    SetValueSaved(window, tabRow, wux::FrameworkElement::HorizontalAlignmentProperty(),
                  winrt::box_value(onRight ? wux::HorizontalAlignment::Right
                                           : wux::HorizontalAlignment::Left));
    SetValueSaved(window, tabRow, wux::FrameworkElement::VerticalAlignmentProperty(),
                  winrt::box_value(wux::VerticalAlignment::Stretch));
    SetValueSaved(window, tabView, wux::FrameworkElement::VerticalAlignmentProperty(),
                  winrt::box_value(wux::VerticalAlignment::Stretch));

    if (auto titlebar = window.titlebar.get()) {
        SetValueSaved(window, tabRow, wuxc::ContentPresenter::BackgroundProperty(),
                      titlebar.Background());
    }

    // The terminal area and the info bars make room for the sidebar. Other
    // children (dialogs, the command palette, toasts) keep the full width.
    if (window.shiftedContent.empty()) {
        for (auto const& child : pageRoot.Children()) {
            auto element = child.try_as<wux::FrameworkElement>();
            if (!element || element == tabRow) {
                continue;
            }
            bool isContent = element.Name() == L"TabContent";
            bool isInfoBars = wuxc::Grid::GetRow(element) == 1 &&
                              element.try_as<wuxc::StackPanel>();
            if (isContent || isInfoBars) {
                SetValueSaved(window, element, wux::FrameworkElement::MarginProperty(),
                              winrt::box_value(element.Margin()));
                window.shiftedContent.emplace_back(winrt::make_weak(element),
                                                   element.Margin());
            }
        }
    }

    UpdateContentMargins(window);
}

////////////////////////////////////////////////////////////////////////////////
// Peeking: a collapsed rail opens while the mouse rests on it

constexpr int kPeekOpenDelayMs = 200;
constexpr int kPeekCloseDelayMs = 300;
constexpr int kPeekPollMs = 150;

#ifdef WTVT_TEST_HOOKS
// Set by the test hook: -1 uses the real cursor, 0 or 1 says where it is.
std::atomic<int> g_testPointerOver{-1};
#endif

// The XAML island window (Terminal's DesktopWindowContentBridge) under a screen
// point, if it belongs to this process, and the point in its client coordinates.
HWND IslandAt(POINT screen, POINT* client) {
    HWND hit = WindowFromPoint(screen);
    HWND top = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
    DWORD processId = 0;
    if (!top || !GetWindowThreadProcessId(top, &processId) ||
        processId != GetCurrentProcessId()) {
        return nullptr;
    }
    HWND island = FindWindowExW(top, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge",
                                nullptr);
    *client = screen;
    return island && ScreenToClient(island, client) ? island : nullptr;
}

// Records which island the sidebar lives in, from a mouse event it just
// received, if the cursor is where the event says the pointer is.
void RecordIsland(WindowState& window, wuxi::PointerRoutedEventArgs const& args) {
    try {
        auto tabRow = window.tabRow.get();
        auto root = tabRow ? tabRow.XamlRoot() : nullptr;
        POINT cursor, local;
        if (!root || !GetCursorPos(&cursor)) {
            return;
        }
        HWND island = IslandAt(cursor, &local);
        double scale = root.RasterizationScale();
        auto position = args.GetCurrentPoint(nullptr).Position();
        if (island && scale > 0 && std::abs(local.x / scale - position.X) <= 2 &&
            std::abs(local.y / scale - position.Y) <= 2) {
            window.island = island;
        }
    } catch (...) {
    }
}

// Whether the mouse cursor is over the window's sidebar right now. Worked out
// from the cursor itself, not from pointer events: those bubble up from every
// child the pointer crosses, and a fast exit off the window's edge can arrive
// with the last position still inside, or not at all.
bool PointerOverSidebar(WindowState const& window) {
#ifdef WTVT_TEST_HOOKS
    if (int over = g_testPointerOver; over >= 0) {
        return over == 1;
    }
#endif
    try {
        auto tabRow = window.tabRow.get();
        auto root = tabRow ? tabRow.XamlRoot() : nullptr;
        POINT cursor, local;
        if (!root || !GetCursorPos(&cursor)) {
            return false;
        }
        HWND island = IslandAt(cursor, &local);
        double scale = root.RasterizationScale();
        if (!island || scale <= 0) {
            return false;
        }
        if (window.island) {
            if (island != window.island) {
                return false;
            }
        } else {
            // Not recorded yet: at least the island must be the size of this
            // XAML root.
            RECT client;
            auto size = root.Size();
            if (!GetClientRect(island, &client) ||
                std::abs(client.right / scale - size.Width) > 2 ||
                std::abs(client.bottom / scale - size.Height) > 2) {
                return false;
            }
        }
        auto bounds = tabRow.TransformToVisual(nullptr).TransformBounds(
            wf::Rect{0, 0, static_cast<float>(tabRow.ActualWidth()),
                     static_cast<float>(tabRow.ActualHeight())});
        double x = local.x / scale, y = local.y / scale;
        return x >= bounds.X && x < bounds.X + bounds.Width && y >= bounds.Y &&
               y < bounds.Y + bounds.Height;
    } catch (...) {
        return false;
    }
}

// The sidebar's own colour, if it is opaque, for the backdrop that hides the
// terminal under a peeking rail. Otherwise the backdrop keeps its theme fill.
wuxm::Brush OpaqueSidebarBrush(wuxc::ContentPresenter const& tabRow, muxc::TabView const& tabView) {
    for (auto const& brush : {tabRow.Background(), tabView.Background()}) {
        auto solid = brush.try_as<wuxm::SolidColorBrush>();
        if (solid && solid.Color().A == 255 && solid.Opacity() >= 1.0) {
            return solid;
        }
    }
    return nullptr;
}

// Shows a collapsed window's sidebar at full width, above the terminal, or puts
// the rail back. Only the look changes: the terminal keeps the rail's margin,
// so it doesn't reflow, and nothing is taken out of the tree. Each step runs on
// its own, so one failing doesn't leave the rest in the other state.
void SetPeek(ThreadContext& context, WindowState& window, bool peek) {
    if (window.peeking == peek) {
        return;
    }
    auto tabRow = window.tabRow.get();
    auto tabView = window.tabView.get();
    if (!tabRow || !tabView || !window.stripApplied) {
        window.peeking = false;
        return;
    }

    LARGE_INTEGER started, finished, frequency;
    QueryPerformanceCounter(&started);
    window.peeking = peek;
    Guarded(L"Peek: width", [&] {
        SetValueSaved(window, tabRow, wux::FrameworkElement::WidthProperty(),
                      winrt::box_value(SidebarWidth(window)));
    });
    // The terminal and the info bars go behind the sidebar; overlays such as
    // the command palette stay above it.
    Guarded(L"Peek: order", [&] {
        for (auto const& [weakElement, original] : window.shiftedContent) {
            if (auto element = weakElement.get()) {
                if (peek) {
                    SetValueSaved(window, element, wuxc::Canvas::ZIndexProperty(),
                                  winrt::box_value(-1));
                } else {
                    RestoreSavedValue(window, element, wuxc::Canvas::ZIndexProperty());
                }
            }
        }
    });
    Guarded(L"Peek: backdrop", [&] {
        if (auto backdrop = window.peekBackdrop.get()) {
            if (peek) {
                if (auto inner = backdrop.Child().try_as<wuxc::Border>()) {
                    inner.Background(OpaqueSidebarBrush(tabRow, tabView));
                }
                if (window.peekBackdropPlain) {
                    bool light = tabRow.ActualTheme() == wux::ElementTheme::Light;
                    backdrop.Background(wuxm::SolidColorBrush(
                        light ? winrt::Windows::UI::ColorHelper::FromArgb(255, 243, 243, 243)
                              : winrt::Windows::UI::ColorHelper::FromArgb(255, 32, 32, 32)));
                }
                backdrop.BorderThickness(g_sidebarOnRight ? wux::Thickness{1, 0, 0, 0}
                                                          : wux::Thickness{0, 0, 1, 0});
            }
            backdrop.Visibility(peek ? wux::Visibility::Visible : wux::Visibility::Collapsed);
        }
    });
    Guarded(L"Peek: footer", [&] { StyleFooter(window); });
    Guarded(L"Peek: collapse button", [&] {
        if (auto button = window.collapseButton.get()) {
            StyleCollapseButton(window, button);
        }
    });
    Guarded(L"Peek: new-tab button", [&] {
        if (auto button = window.newTabButton.get()) {
            FitNewTabButton(window, button);
        }
    });
    Guarded(L"Peek: tabs", [&] { ApplyCurrentTabTemplate(context, window, tabView); });
    Guarded(L"Peek: list height", [&] { FitTabListHeight(window); });
    QueryPerformanceCounter(&finished);
    QueryPerformanceFrequency(&frequency);
    Wh_Log(L"Peek %s: %u tabs in %.1f ms", peek ? L"open" : L"closed", tabView.TabItems().Size(),
           (finished.QuadPart - started.QuadPart) * 1000.0 / frequency.QuadPart);
}

// A menu of this window is open. Tooltips don't count: every tab has one, and
// a tooltip left behind by a fast exit would otherwise hold the peek open.
bool AnyPopupOpen(WindowState const& window) {
    try {
        auto tabRow = window.tabRow.get();
        auto root = tabRow ? tabRow.XamlRoot() : nullptr;
        if (!root) {
            return false;
        }
        for (auto const& popup : wuxm::VisualTreeHelper::GetOpenPopupsForXamlRoot(root)) {
            if (!popup.Child().try_as<wuxc::ToolTip>()) {
                return true;
            }
        }
    } catch (...) {
    }
    return false;
}

void OnPeekTimer(winrt::weak_ref<muxc::TabView> const& weakTabView);

void SchedulePeekCheck(WindowState& window, int delayMs) {
    if (!window.peekTimer) {
        auto queue = ws::DispatcherQueue::GetForCurrentThread();
        if (!queue) {
            return;
        }
        window.peekTimer = queue.CreateTimer();
        window.peekTimer.IsRepeating(false);
        window.peekTimerToken = window.peekTimer.Tick(
            [weakTabView = window.tabView](ws::DispatcherQueueTimer const&, wf::IInspectable const&) {
                OnPeekTimer(weakTabView);
            });
    }
    window.peekTimer.Interval(std::chrono::milliseconds{delayMs});
    window.peekTimer.Start();
}

void StopPeekTimer(WindowState& window) {
    if (auto timer = std::exchange(window.peekTimer, nullptr)) {
        timer.Stop();
        timer.Tick(std::exchange(window.peekTimerToken, {}));
    }
}

// The mouse entered or left the sidebar, or crossed between its children. The
// event only prompts a look at where the cursor is (see OnPeekTimer).
void OnSidebarPointer(WindowState& window, bool entered) {
    if (!window.collapsed) {
        return;
    }
    if (!entered && !PointerOverSidebar(window)) {
        window.suppressPeek = false;  // it has left since the collapse
    }
    SchedulePeekCheck(window, entered && !window.peeking ? kPeekOpenDelayMs : kPeekCloseDelayMs);
}

WindowState* FindWindowByTabView(ThreadContext& context, muxc::TabView const& tabView);

void OnPeekTimer(winrt::weak_ref<muxc::TabView> const& weakTabView) {
    auto context = GetThreadContext(false);
    auto tabView = weakTabView.get();
    auto window = context && tabView ? FindWindowByTabView(*context, tabView) : nullptr;
    if (!g_active || !window || !window->layoutApplied) {
        return;
    }
    // Nothing changes while a tab is dragged: re-templating the tabs would pull
    // the dragged one out from under the pointer. A drag whose completion never
    // arrived ends when no mouse button is down (physical buttons, so either).
    if (window->dragging && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000) &&
        !(GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {
        window->dragging = false;
    }
    if (window->dragging) {
        SchedulePeekCheck(*window, kPeekPollMs);
        return;
    }
    bool over = PointerOverSidebar(*window);
    if (!over) {
        window->suppressPeek = false;
    }
    bool want = window->collapsed && over && !window->suppressPeek &&
                tabView.Visibility() == wux::Visibility::Visible;
    if (window->peeking && !want) {
        // Stay open while a tab's menu or the new-tab menu is open, or a tab is
        // being dragged, and for a moment after the mouse leaves, so a brief
        // stray outside doesn't snap it shut.
        ULONGLONG now = GetTickCount64();
        if (!window->leftAt) {
            window->leftAt = now;
        }
        if (AnyPopupOpen(*window) ||
            now - window->leftAt < static_cast<ULONGLONG>(kPeekCloseDelayMs)) {
            SchedulePeekCheck(*window, kPeekPollMs);
            return;
        }
    }
    window->leftAt = 0;
    if (want != window->peeking) {
        Guarded(L"Peeking", [&] { SetPeek(*context, *window, want); });
    }
    if (window->peeking) {
        // Keep looking while open: leaving the window fast may raise no event.
        SchedulePeekCheck(*window, kPeekPollMs);
    }
}

bool IsMouse(wuxi::PointerRoutedEventArgs const& args) {
    return args.Pointer().PointerDeviceType() ==
           winrt::Windows::Devices::Input::PointerDeviceType::Mouse;
}

void AddPointerHandlers(WindowState& window, wuxc::ContentPresenter const& tabRow) {
    if (window.pointerEnteredHandler) {
        return;
    }
    auto weakTabView = window.tabView;
    // Handled events too: the tabs and buttons inside mark theirs handled.
    // Mouse only: a touch or pen tap would open a rail nothing would close; the
    // collapse button expands it for those.
    auto handler = [weakTabView](bool entered) {
        return winrt::box_value(wuxi::PointerEventHandler(
            [weakTabView, entered](wf::IInspectable const&, wuxi::PointerRoutedEventArgs const& args) {
                if (!g_active || !IsMouse(args)) {
                    return;
                }
                auto context = GetThreadContext(false);
                auto tabView = weakTabView.get();
                auto window = context && tabView ? FindWindowByTabView(*context, tabView) : nullptr;
                if (window && window->layoutApplied) {
                    if (entered) {
                        RecordIsland(*window, args);
                    }
                    OnSidebarPointer(*window, entered);
                }
            }));
    };
    window.pointerEnteredHandler = handler(true);
    window.pointerExitedHandler = handler(false);
    tabRow.AddHandler(wux::UIElement::PointerEnteredEvent(), window.pointerEnteredHandler, true);
    tabRow.AddHandler(wux::UIElement::PointerExitedEvent(), window.pointerExitedHandler, true);

    // A peek never closes under a tab being dragged: re-templating the tabs
    // mid-drag would pull the dragged tab out from under the pointer.
    if (auto tabView = window.tabView.get()) {
        window.dragStartingToken = tabView.TabDragStarting(
            [](muxc::TabView const& sender, muxc::TabViewTabDragStartingEventArgs const&) {
                auto context = GetThreadContext(false);
                if (auto window = context ? FindWindowByTabView(*context, sender) : nullptr) {
                    window->dragging = true;
                }
            });
        window.dragCompletedToken = tabView.TabDragCompleted(
            [](muxc::TabView const& sender, muxc::TabViewTabDragCompletedEventArgs const&) {
                auto context = GetThreadContext(false);
                if (auto window = context ? FindWindowByTabView(*context, sender) : nullptr) {
                    window->dragging = false;
                    if (window->peeking) {
                        SchedulePeekCheck(*window, kPeekCloseDelayMs);
                    }
                }
            });
    }
}

void RemovePointerHandlers(WindowState& window) {
    Guarded(L"Stopping the peek timer", [&] { StopPeekTimer(window); });
    auto tabRow = window.tabRow.get();
    Guarded(L"Removing the pointer handlers", [&] {
        if (tabRow && window.pointerEnteredHandler) {
            tabRow.RemoveHandler(wux::UIElement::PointerEnteredEvent(), window.pointerEnteredHandler);
            tabRow.RemoveHandler(wux::UIElement::PointerExitedEvent(), window.pointerExitedHandler);
        }
    });
    Guarded(L"Removing the drag handlers", [&] {
        auto tabView = window.tabView.get();
        if (tabView && window.dragStartingToken) {
            tabView.TabDragStarting(window.dragStartingToken);
        }
        if (tabView && window.dragCompletedToken) {
            tabView.TabDragCompleted(window.dragCompletedToken);
        }
    });
    window.pointerEnteredHandler = nullptr;
    window.pointerExitedHandler = nullptr;
    window.dragStartingToken = {};
    window.dragCompletedToken = {};
    window.peeking = false;
    window.dragging = false;
    window.leftAt = 0;
}

WindowState* FindWindowState(ThreadContext& context,
                             wuxc::ContentPresenter const& tabRow) {
    for (auto& window : context.windows) {
        if (window->tabRow.get() == tabRow) {
            return window.get();
        }
    }
    return nullptr;
}

bool IsVerticalTabView(ThreadContext& context, muxc::TabView const& tabView) {
    return std::any_of(context.windows.begin(), context.windows.end(),
                       [&](std::unique_ptr<WindowState> const& window) {
                           return window->stripApplied && window->tabView.get() == tabView;
                       });
}

WindowState* FindWindowByTabView(ThreadContext& context, muxc::TabView const& tabView) {
    for (auto& window : context.windows) {
        if (window->tabView.get() == tabView) {
            return window.get();
        }
    }
    return nullptr;
}

// Starts following a window's tab strip, in either layout.
WindowState* TrackWindow(ThreadContext& context, wuxc::ContentPresenter const& tabRow) {
    auto tabView = tabRow.Content().try_as<muxc::TabView>();
    if (!tabView) {
        return nullptr;
    }

    auto page = FindDescendant(
        GetTopAncestor(tabRow),
        [](wux::DependencyObject const& element) {
            return IsClass(element, L"TerminalApp.TerminalPage");
        },
        nullptr, 8);
    auto pageRoot = page ? page.as<wuxc::Page>().Content().try_as<wuxc::Grid>()
                         : nullptr;
    if (!pageRoot || !EnsureTemplates(context)) {
        return nullptr;
    }

    auto* window = FindWindowState(context, tabRow);
    if (!window) {
        context.windows.push_back(std::make_unique<WindowState>());
        window = context.windows.back().get();
        window->tabRow = winrt::make_weak(tabRow);
        window->collapsed = g_collapsed;
    }
    window->pageRoot = winrt::make_weak(pageRoot);
    window->tabView = winrt::make_weak(tabView);

    for (auto const& item : tabView.TabItems()) {
        if (auto tab = item.try_as<muxc::TabViewItem>()) {
            AddMenuEntry(*window, tab);
        }
    }

    // New tabs (and tabs put back after a reorder) get the menu entry and,
    // in the vertical layout, the vertical template as they are inserted,
    // before their first layout. This, not the XAML diagnostics, is what
    // follows new tabs: the diagnostics are only attached for a few seconds
    // at a time (see ScheduleDetach).
    if (!window->tabItemsChangedToken) {
        window->tabItemsChangedToken = tabView.TabItemsChanged(
            [](muxc::TabView const& sender, wf::Collections::IVectorChangedEventArgs const& args) {
                auto context = GetThreadContext(false);
                auto window = context ? FindWindowByTabView(*context, sender) : nullptr;
                if (!g_active || !window) {
                    return;
                }
                auto change = args.CollectionChange();
                if (change == wf::Collections::CollectionChange::ItemRemoved ||
                    change == wf::Collections::CollectionChange::Reset) {
                    // After this change settles (a reorder removes and then
                    // inserts), release the menus of tabs that are gone.
                    context->pendingMenuSweep = true;
                    QueueWork();
                }
                if (window->stripApplied && !CurrentTabTemplate(*context, *window) &&
                    CurrentTabTemplateUntried(*context, *window)) {
                    // The strip went vertical before the window had a tab to
                    // try the template on; now it has one.
                    context->pendingTemplateRetry = true;
                    QueueWork();
                }
                std::vector<muxc::TabViewItem> tabs;
                if (args.CollectionChange() == wf::Collections::CollectionChange::ItemInserted) {
                    if (auto tab = sender.TabItems().GetAt(args.Index()).try_as<muxc::TabViewItem>()) {
                        tabs.push_back(tab);
                    }
                } else if (args.CollectionChange() == wf::Collections::CollectionChange::Reset) {
                    for (auto const& item : sender.TabItems()) {
                        if (auto tab = item.try_as<muxc::TabViewItem>()) {
                            tabs.push_back(tab);
                        }
                    }
                }
                for (auto const& tab : tabs) {
                    AddMenuEntry(*window, tab);
                    if (window->stripApplied) {
                        if (auto itemTemplate = CurrentTabTemplate(*context, *window)) {
                            SetTabViewItemTemplate(tab, itemTemplate);
                        }
                    }
                }
            });
    }
    return window;
}

void RemoveLayout(WindowState& window);

// Idempotent: runs whenever a tab row shows up in the tree, including after
// Windows Terminal itself moves it (it moves the tab row into the title bar
// during startup when "Show tabs in title bar" is on).
void ApplyLayout(ThreadContext& context, WindowState& window) {
    auto tabRow = window.tabRow.get();
    auto pageRoot = window.pageRoot.get();
    auto tabView = window.tabView.get();
    if (!tabRow || !pageRoot || !tabView || window.stripFailed) {
        return;
    }

    // Checked before anything moves, so an unfamiliar template leaves the
    // window exactly as Terminal built it. A TabView that isn't templated yet
    // is simply tried again at the next sighting.
    if (!window.stripApplied && !FindStripParts(tabView)) {
        if (wuxm::VisualTreeHelper::GetChildrenCount(tabView) > 0) {
            Wh_Log(L"Unrecognised TabView template, leaving the tabs as they are");
            window.stripFailed = true;
        }
        return;
    }

    LARGE_INTEGER started, frequency;
    QueryPerformanceCounter(&started);
    QueryPerformanceFrequency(&frequency);

    // Set before the first change, so that a failure part way through still
    // gets undone by RemoveLayout.
    window.layoutApplied = true;

    auto parent = GetParent(tabRow);
    if (parent != pageRoot) {
        auto host = parent ? parent.try_as<wuxc::ContentPresenter>() : nullptr;
        if (!host || host.Name() != L"ContentRoot") {
            // Just handed back to the title bar (by RemoveLayout, when a toggle
            // rebuilds the layout): the presenter already holds it as content
            // but hasn't laid it out, so it has no visual parent yet.
            auto known = window.titlebarHost.get();
            host = known && known.Content() == tabRow ? known : nullptr;
        }
        if (!host) {
            Wh_Log(L"The tab row is somewhere unexpected, leaving it");
            return;
        }

        auto titlebar = GetParent(host).try_as<wuxc::Panel>();
        if (titlebar && !window.titlebarBackgroundToken) {
            window.titlebarBackgroundToken = titlebar.RegisterPropertyChangedCallback(
                wuxc::Panel::BackgroundProperty(),
                [weakTabRow = winrt::make_weak(tabRow)](
                    wux::DependencyObject const& sender, wux::DependencyProperty const&) {
                    auto tabRow = weakTabRow.get();
                    auto panel = sender.try_as<wuxc::Panel>();
                    if (tabRow && panel && g_active && g_vertical) {
                        tabRow.Background(panel.Background());
                    }
                });
        }
        window.titlebarHost = winrt::make_weak(host);
        window.titlebar = winrt::make_weak(titlebar);

        // At the bottom of the page's children, where Terminal itself keeps
        // the tab row when it isn't in the title bar, so the command palette
        // and other overlays draw over the sidebar.
        host.Content(nullptr);
        pageRoot.Children().InsertAt(0, tabRow);
    }

    ApplySidebarLayout(window);
    AddPointerHandlers(window, tabRow);

    if (!window.tabViewVisibilityToken) {
        window.tabViewVisibilityToken = tabView.RegisterPropertyChangedCallback(
            wux::UIElement::VisibilityProperty(),
            [](wux::DependencyObject const& sender, wux::DependencyProperty const&) {
                auto context = GetThreadContext(false);
                if (!context || !g_active) {
                    return;
                }
                for (auto& window : context->windows) {
                    if (window->layoutApplied && window->tabView.get() == sender) {
                        if (window->peeking &&
                            sender.as<wux::UIElement>().Visibility() != wux::Visibility::Visible) {
                            Guarded(L"Closing the peek", [&] { SetPeek(*context, *window, false); });
                        }
                        UpdateContentMargins(*window);
                    }
                }
            });
    }

    if (!window.stripApplied) {
        // If the strip can't be finished, put the tab row back where Terminal
        // had it, rather than leave a horizontal strip squeezed into the
        // sidebar's column.
        bool applied = false;
        try {
            applied = ApplyVerticalStrip(context, window, tabView);
        } catch (winrt::hresult_error const&) {
            window.stripFailed = true;
            RemoveLayout(window);
            throw;
        } catch (...) {
            Wh_Log(L"Making the strip vertical failed");
            window.stripFailed = true;
            RemoveLayout(window);
            return;
        }
        if (!applied) {
            Wh_Log(L"Making the strip vertical failed, leaving the tabs as they are");
            window.stripFailed = true;
            RemoveLayout(window);
            return;
        }
        LARGE_INTEGER finished;
        QueryPerformanceCounter(&finished);
        Wh_Log(L"Vertical tabs applied to %u tabs in %.1f ms", tabView.TabItems().Size(),
               (finished.QuadPart - started.QuadPart) * 1000.0 / frequency.QuadPart);
    }
}

// Puts one window's tab strip back the way Windows Terminal built it.
// A stock template put back on a live tab starts with its separator showing
// and its bottom border in the default state. TabView sets both only on
// selection and collection changes (TabView::SetTabSeparatorOpacity and
// UpdateTabBottomBorderLineVisualStates), so do what it would have done.
void RefreshStockTabVisuals(muxc::TabView const& tabView) {
    tabView.UpdateLayout();
    int count = static_cast<int>(tabView.TabItems().Size());
    int selected = tabView.SelectedIndex();
    for (int i = 0; i < count; i++) {
        auto tab = tabView.ContainerFromIndex(i).try_as<muxc::TabViewItem>();
        if (!tab) {
            continue;
        }
        if (auto separator = FindDescendant(
                tab, [](wux::DependencyObject const& e) { return HasName(e, L"TabSeparator"); },
                nullptr, 4)) {
            separator.as<wux::UIElement>().Opacity(i == selected || i + 1 == selected ? 0.0 : 1.0);
        }
        PCWSTR state = L"NormalBottomBorderLine";
        if (selected != -1) {
            if (i == selected) {
                state = L"NoBottomBorderLine";
            } else if (i == selected - 1) {
                state = L"LeftOfSelectedTab";
            } else if (i == selected + 1) {
                state = L"RightOfSelectedTab";
            }
        }
        wux::VisualStateManager::GoToState(tab, state, false);
    }
}

void RemoveLayout(WindowState& window) {
    auto tabRow = window.tabRow.get();
    auto tabView = window.tabView.get();
    auto pageRoot = window.pageRoot.get();

    RemovePointerHandlers(window);

    Guarded(L"Revoking the visibility callback", [&] {
        if (tabView && window.tabViewVisibilityToken) {
            tabView.UnregisterPropertyChangedCallback(wux::UIElement::VisibilityProperty(),
                                                      window.tabViewVisibilityToken);
        }
    });
    window.tabViewVisibilityToken = 0;

    Guarded(L"Revoking the title bar callback", [&] {
        auto titlebar = window.titlebar.get();
        if (titlebar && window.titlebarBackgroundToken) {
            titlebar.UnregisterPropertyChangedCallback(wuxc::Panel::BackgroundProperty(),
                                                       window.titlebarBackgroundToken);
        }
    });
    window.titlebarBackgroundToken = 0;

    RemoveVerticalStrip(window);

    Guarded(L"Restoring values", [&] { RestoreSavedValues(window); });
    window.savedValues.clear();
    window.shiftedContent.clear();

    Guarded(L"Returning the tab row", [&] {
        auto titlebarHost = window.titlebarHost.get();
        if (tabRow && pageRoot && titlebarHost) {
            uint32_t index = 0;
            if (pageRoot.Children().IndexOf(tabRow, index)) {
                pageRoot.Children().RemoveAt(index);
                titlebarHost.Content(tabRow);
            }
        }
    });
    window.layoutApplied = false;

    Guarded(L"Refreshing the stock tabs", [&] {
        if (tabView) {
            RefreshStockTabVisuals(tabView);
        }
    });
}

void UntrackWindow(WindowState& window) {
    RemoveMenuEntries(window);
    Guarded(L"Revoking the tab handler", [&] {
        auto tabView = window.tabView.get();
        if (tabView && window.tabItemsChangedToken) {
            tabView.TabItemsChanged(window.tabItemsChangedToken);
        }
    });
    window.tabItemsChangedToken = {};
    if (window.layoutApplied) {
        RemoveLayout(window);
    }
}

// A window that has closed has nothing left to restore; drop its state (and
// with it the strong references it held).
void PruneClosedWindows(ThreadContext& context) {
    std::erase_if(context.windows, [](std::unique_ptr<WindowState> const& window) {
        if (window->tabRow.get()) {
            return false;
        }
        // Its menus may outlive it: take the entries and handlers out first.
        UntrackWindow(*window);
        return true;
    });
}

void OnTabRowSeen(ThreadContext& context, wuxc::ContentPresenter const& tabRow) {
    PruneClosedWindows(context);
    auto* window = TrackWindow(context, tabRow);
    if (window && g_vertical) {
        ApplyLayout(context, *window);
    }
}

// Rebuilds one window's layout (or removes it) to match the current toggles.
void RefreshWindow(ThreadContext& context, WindowState& window) {
    try {
        if (window.layoutApplied) {
            RemoveLayout(window);
            // Let the restored template build its parts (the list's
            // horizontal items panel in particular) before re-applying,
            // or the re-apply finds the old panel that is about to go.
            if (auto tabView = window.tabView.get()) {
                tabView.UpdateLayout();
            }
        }
        UpdateMenuEntries(window);
        // A toggle or settings change is the one retry a window whose
        // strip failed gets.
        window.stripFailed = false;
        if (g_vertical) {
            ApplyLayout(context, window);
        }
    } catch (winrt::hresult_error const& ex) {
        Wh_Log(L"Refreshing a window failed: %08X %s", ex.code().value,
               ex.message().c_str());
    } catch (...) {
        Wh_Log(L"Refreshing a window failed");
    }
}

// Brings the calling thread's windows in line with the current settings and
// toggles: the layout rebuilt (or removed), the menu entries relabelled.
void RefreshCurrentThread() {
    auto context = GetThreadContext(false);
    if (!context) {
        return;
    }
    PruneClosedWindows(*context);
    for (auto& window : context->windows) {
        RefreshWindow(*context, *window);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Deferred work on the UI thread

void ForEachWindowThread(void (*proc)(void*), void* parameter);

void RunPendingWork(ThreadContext& context) {
    auto pending = std::move(context.pendingTabRows);
    context.pendingTabRows.clear();
    bool toggleVertical = std::exchange(context.pendingToggleVertical, false);
    auto collapseToggles = std::move(context.pendingCollapseToggles);
    context.pendingCollapseToggles.clear();
    bool menuSweep = std::exchange(context.pendingMenuSweep, false);
    bool templateRetry = std::exchange(context.pendingTemplateRetry, false);
    if (!g_active) {
        return;
    }

    if (menuSweep) {
        for (auto& window : context.windows) {
            Guarded(L"Sweeping menu entries", [&] { SweepMenuEntries(*window); });
        }
    }

    if (templateRetry) {
        for (auto& window : context.windows) {
            auto tabView = window->tabView.get();
            if (!tabView || !window->stripApplied) {
                continue;
            }
            Guarded(L"Retrying the tab template",
                    [&] { ApplyCurrentTabTemplate(context, *window, tabView); });
        }
    }

#ifdef WTVT_TEST_HOOKS
    if (int entered = std::exchange(context.pendingTestPointer, -1); entered == 2) {
        g_testPointerOver = -1;  // back to the real cursor
    } else if (entered >= 0) {
        g_testPointerOver = entered;
        for (auto& window : context.windows) {
            if (window->layoutApplied) {
                OnSidebarPointer(*window, entered == 1);
            }
        }
    }
    if (std::exchange(context.pendingTestReport, false)) {
        Wh_Log(L"TEST diagnostics advised: %d", DiagnosticsAdvisedForTest());
    }
    if (std::exchange(context.pendingTestClosePopups, false)) {
        for (auto& window : context.windows) {
            Guarded(L"Closing test popups", [&] {
                auto tabRow = window->tabRow.get();
                auto root = tabRow ? tabRow.XamlRoot() : nullptr;
                if (root) {
                    for (auto const& popup : wuxm::VisualTreeHelper::GetOpenPopupsForXamlRoot(root)) {
                        popup.IsOpen(false);
                    }
                }
            });
        }
    }
    if (int index = std::exchange(context.pendingTestMenuTab, -1); index >= 0) {
        Guarded(L"Opening a test menu", [&] {
            for (auto& window : context.windows) {
                auto tabView = window->tabView.get();
                if (!tabView || static_cast<uint32_t>(index) >= tabView.TabItems().Size()) {
                    continue;
                }
                auto tab = tabView.TabItems().GetAt(index).try_as<muxc::TabViewItem>();
                auto flyout = tab ? tab.ContextFlyout() : nullptr;
                if (!flyout) {
                    break;
                }
                // Shown at a point inside the tab, the way a right-click shows it.
                wuxc::Primitives::FlyoutShowOptions options;
                options.Position(wf::Point{static_cast<float>(tab.ActualWidth() * 0.55),
                                           static_cast<float>(tab.ActualHeight() / 2)});
                options.ShowMode(wuxc::Primitives::FlyoutShowMode::Standard);
                // With no pointer input ever seen, XAML gives the first item
                // keyboard focus and draws its focus ring; a right-click gives
                // it pointer focus.
                RevokeTestMenuHandler(context);
                context.testMenu = flyout;
                context.testMenuOpenedToken = flyout.Opened(
                    [](wf::IInspectable const& sender, wf::IInspectable const&) {
                        try {
                            if (auto context = GetThreadContext(false)) {
                                RevokeTestMenuHandler(*context);
                            }
                            auto menu = sender.try_as<wuxc::MenuFlyout>();
                            if (menu && menu.Items().Size() > 0) {
                                if (auto first = menu.Items().GetAt(0).try_as<wuxc::Control>()) {
                                    first.Focus(wux::FocusState::Pointer);
                                }
                            }
                            Wh_Log(L"TEST opened a tab's context menu");
                        } catch (...) {
                        }
                    });
                flyout.ShowAt(tab, options);
                break;
            }
        });
    }
#endif

    for (auto const& weakTabRow : pending) {
        auto tabRow = weakTabRow.get();
        if (!tabRow) {
            continue;
        }
        try {
            OnTabRowSeen(context, tabRow);
        } catch (winrt::hresult_error const& ex) {
            Wh_Log(L"Applying vertical tabs failed: %08X %s", ex.code().value,
                   ex.message().c_str());
        } catch (...) {
            Wh_Log(L"Applying vertical tabs failed");
        }
    }

    // Collapsing is per window: only the window whose button was clicked is
    // rebuilt. Its new state is also what the next new window starts with.
    for (auto const& weakTabView : collapseToggles) {
        auto tabView = weakTabView.get();
        auto window = tabView ? FindWindowByTabView(context, tabView) : nullptr;
        if (!window || !window->layoutApplied) {
            continue;
        }
        bool collapsed = !window->collapsed;
        window->collapsed = collapsed;
        g_collapsed = collapsed;
        Wh_SetIntValue(L"collapsed", collapsed);
        Wh_Log(L"Window %s", window->collapsed ? L"collapsed" : L"expanded");
        RefreshWindow(context, *window);
        // With the mouse on the button that collapsed the rail, don't open it
        // again under the mouse until the mouse has left once.
        if (auto tabRow = window->tabRow.get()) {
            Guarded(L"Laying out the rail", [&] { tabRow.UpdateLayout(); });
        }
        window->suppressPeek = collapsed && PointerOverSidebar(*window);
    }

    if (toggleVertical) {
        g_vertical = !g_vertical;
        Wh_SetIntValue(L"vertical", g_vertical);
        Wh_Log(L"Layout: %s", g_vertical ? L"vertical" : L"horizontal");
        ForEachWindowThread([](void*) { RefreshCurrentThread(); }, nullptr);
    }
}

void QueueWork() {
    auto context = GetThreadContext(false);
    if (!context) {
        return;
    }

    if (!context->workTimer) {
        auto queue = ws::DispatcherQueue::GetForCurrentThread();
        if (!queue) {
            Wh_Log(L"No dispatcher queue on this thread");
            return;
        }
        context->workTimer = queue.CreateTimer();
        context->workTimer.IsRepeating(false);
        context->workTimer.Interval(std::chrono::milliseconds{1});
        context->workTimerToken = context->workTimer.Tick(
            [](ws::DispatcherQueueTimer const&, wf::IInspectable const&) {
                if (auto context = GetThreadContext(false)) {
                    RunPendingWork(*context);
                }
            });
    }
    context->workTimer.Start();
}

#ifdef WTVT_TEST_HOOKS
LRESULT CALLBACK TestGetMessageHook(int code, WPARAM wParam, LPARAM lParam) {
    auto message = reinterpret_cast<MSG*>(lParam);
    if (code == HC_ACTION && wParam == PM_REMOVE && message->message == TestMessage()) {
        if (auto context = GetThreadContext(false)) {
            context->pendingTestMenuTab = static_cast<int>(message->wParam);
            QueueWork();
        }
    }
    if (code == HC_ACTION && wParam == PM_REMOVE && message->message == TestPointerMessage()) {
        if (auto context = GetThreadContext(false)) {
            context->pendingTestPointer = static_cast<int>(std::min<WPARAM>(message->wParam, 2));
            QueueWork();
        }
    }
    if (code == HC_ACTION && wParam == PM_REMOVE && message->message == TestReportMessage()) {
        if (auto context = GetThreadContext(false)) {
            context->pendingTestReport = true;
            QueueWork();
        }
    }
    if (code == HC_ACTION && wParam == PM_REMOVE && message->message == TestClosePopupsMessage()) {
        if (auto context = GetThreadContext(false)) {
            context->pendingTestClosePopups = true;
            QueueWork();
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
#endif

void QueueTabRow(ThreadContext& context, wuxc::ContentPresenter const& tabRow) {
    context.pendingTabRows.push_back(winrt::make_weak(tabRow));
    QueueWork();
}

// From a click handler: the layout changes once the click (and the menu it came
// from) has finished.
void RequestToggleVertical() {
    auto context = GetThreadContext(true);
    context->pendingToggleVertical = true;
    QueueWork();
}

void RequestToggleCollapsed(winrt::weak_ref<muxc::TabView> const& tabView) {
    auto context = GetThreadContext(true);
    context->pendingCollapseToggles.push_back(tabView);
    QueueWork();
}

////////////////////////////////////////////////////////////////////////////////
// XAML Diagnostics
//
// InitializeXamlDiagnosticsEx loads this module into the XAML runtime as a
// "tool attach point" and hands it an IXamlDiagnostics object; from then on the
// runtime reports every element that enters or leaves a visual tree, on the
// element's UI thread, including the elements that already exist.

// {5F9E3C7A-2B1D-4E6F-9A8C-7D1E0B4F2A63}
constexpr CLSID CLSID_VerticalTabsTap = {
    0x5f9e3c7a, 0x2b1d, 0x4e6f, {0x9a, 0x8c, 0x7d, 0x1e, 0x0b, 0x4f, 0x2a, 0x63}};

// Not in xamlom.h. The runtime keeps a reference to every element it reports
// until the handle is unregistered through this interface, so without it
// every element Windows Terminal ever creates would be kept alive.
constexpr GUID IID_IXamlDiagnosticsTestHooks = {
    0x735941a2, 0x3ee3, 0x495a, {0x8d, 0xa9, 0x97, 0x26, 0x27, 0x00, 0x30, 0x75}};

struct IXamlDiagnosticsTestHooks : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE UnregisterInstance(InstanceHandle handle) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetDispatcherQueueForObject(
        InstanceHandle handle,
        void** dispatcherQueue) = 0;
};

HMODULE GetCurrentModuleHandle() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GetCurrentModuleHandle), &module);
    return module;
}

void NoteTreeActivity();
void ScheduleDetach();

class VisualTreeWatcher;
winrt::com_ptr<VisualTreeWatcher> g_visualTreeWatcher;
std::atomic<int> g_diagnosticsWorkers{0};
// Guards g_visualTreeWatcher and g_tearingDown. Never held while calling into
// the diagnostics: those calls can wait on a UI thread, and UI threads take
// this lock too.
std::mutex g_visualTreeWatcherMutex;
// Set first thing on uninit: from then on nothing attaches the diagnostics,
// and a watcher that arrives late is refused.
bool g_tearingDown = false;

winrt::com_ptr<VisualTreeWatcher> CurrentWatcher() {
    std::lock_guard lock(g_visualTreeWatcherMutex);
    return g_visualTreeWatcher;
}

void QueueDiagnosticsRelease(ThreadContext& context, InstanceHandle handle);

class VisualTreeWatcher
    : public winrt::implements<VisualTreeWatcher, IVisualTreeServiceCallback2,
                               winrt::non_agile> {
   public:
    explicit VisualTreeWatcher(winrt::com_ptr<IUnknown> const& site)
        : m_diagnostics(site.as<IXamlDiagnostics>()) {
        m_diagnostics->QueryInterface(IID_IXamlDiagnosticsTestHooks,
                                      m_testHooks.put_void());
        if (!m_testHooks) {
            Wh_Log(L"No IXamlDiagnosticsTestHooks, elements will stay referenced");
        }
    }

    // While advised, the runtime holds on to state for everything it reports
    // (measured: about 100 KB per terminal tab opened, never given back), so
    // the mod only stays advised long enough to find new windows' tab rows.
    //
    // Advising can deadlock when done from a UI thread (the runtime runs part
    // of it on the UI thread and waits), so every change happens on a short
    // worker thread; the workers are serialised and converge on the most
    // recently requested state.
    //
    // Each worker holds its own reference to this module and exits through
    // FreeLibraryAndExitThread, so it can finish safely even if Windhawk
    // unloads the mod while it runs.
    void SetAdvised(bool advised) {
        m_wantAdvised = advised;

        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(&GetCurrentModuleHandle), &module)) {
            return;
        }
        struct WorkerParam {
            VisualTreeWatcher* watcher;
            HMODULE module;
        };
        auto param = new WorkerParam{this, module};
        AddRef();
        g_diagnosticsWorkers++;
        HANDLE thread = CreateThread(
            nullptr, 0,
            [](LPVOID parameter) -> DWORD {
                auto param = static_cast<WorkerParam*>(parameter);
                HMODULE module = param->module;
                param->watcher->ApplyAdvisedState();
                param->watcher->Release();
                delete param;
                g_diagnosticsWorkers--;
                FreeLibraryAndExitThread(module, 0);
            },
            param, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        } else {
            delete param;
            g_diagnosticsWorkers--;
            Release();
            FreeLibrary(module);
        }
    }

    bool IsAdvised() {
        std::lock_guard lock(m_adviseMutex);
        return m_advised;
    }

    // Synchronous, for teardown (never from a UI thread).
    void UnadviseNow() {
        m_wantAdvised = false;
        ApplyAdvisedState();
    }

    HRESULT ReleaseHandle(InstanceHandle handle) {
        return m_testHooks ? m_testHooks->UnregisterInstance(handle) : E_NOINTERFACE;
    }

   private:
    void ApplyAdvisedState() {
        std::lock_guard lock(m_adviseMutex);
        bool want = m_wantAdvised;
        if (want == m_advised) {
            return;
        }
        auto service = m_diagnostics.as<IVisualTreeService3>();
        HRESULT hr = want ? service->AdviseVisualTreeChange(this)
                          : service->UnadviseVisualTreeChange(this);
        if (SUCCEEDED(hr)) {
            m_advised = want;
            Wh_Log(L"Diagnostics %s", want ? L"attached" : L"detached");
        } else {
            Wh_Log(L"%s failed: %08X",
                   want ? L"AdviseVisualTreeChange" : L"UnadviseVisualTreeChange", hr);
        }
    }

    std::mutex m_adviseMutex;
    std::atomic<bool> m_wantAdvised{false};
    bool m_advised = false;

    // Nothing may escape this method into the XAML runtime, and returning an
    // error stops further notifications, so it always returns S_OK.
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation,
                                                 VisualElement element,
                                                 VisualMutationType mutationType) noexcept override {
        if (!g_active) {
            return S_OK;
        }
        try {
            if (mutationType == Add && element.Type) {
                OnElementAdded(element);
            }
        } catch (winrt::hresult_error const& ex) {
            Wh_Log(L"OnVisualTreeChange failed: %08X %s", ex.code().value,
                   ex.message().c_str());
        } catch (...) {
            Wh_Log(L"OnVisualTreeChange failed");
        }

        // The runtime registers the element and its parent for every report,
        // whichever the mutation, and keeps both until they are released.
        try {
            if (auto context = GetThreadContext(true)) {
                QueueDiagnosticsRelease(*context, element.Handle);
                QueueDiagnosticsRelease(*context, relation.Parent);
            }
        } catch (...) {
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle,
                                                    VisualElementState,
                                                    LPCWSTR) noexcept override {
        return S_OK;
    }

    void OnElementAdded(VisualElement const& element) {
        std::wstring_view type = element.Type;
        if (type == L"TerminalApp.TabRowControl") {
            auto tabRow = FromHandle(element.Handle).try_as<wuxc::ContentPresenter>();
            if (tabRow) {
                NoteTreeActivity();
                QueueTabRow(*GetThreadContext(true), tabRow);
                ScheduleDetach();
            }
        }
    }

    wf::IInspectable FromHandle(InstanceHandle handle) {
        wf::IInspectable object;
        winrt::check_hresult(m_diagnostics->GetIInspectableFromHandle(
            handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(object))));
        return object;
    }

    winrt::com_ptr<IXamlDiagnostics> m_diagnostics;
    winrt::com_ptr<IXamlDiagnosticsTestHooks> m_testHooks;
};

#ifdef WTVT_TEST_HOOKS
int DiagnosticsAdvisedForTest() {
    auto watcher = CurrentWatcher();
    return watcher ? watcher->IsAdvised() : -1;
}
#endif

// The diagnostics detach once no tab row has turned up for this long.
constexpr ULONGLONG kDetachAfterQuietMs = 5000;
std::atomic<ULONGLONG> g_lastTreeActivityTick{0};

void NoteTreeActivity() {
    g_lastTreeActivityTick = GetTickCount64();
}

void RequestAdvised(bool advised) {
    winrt::com_ptr<VisualTreeWatcher> watcher;
    {
        std::lock_guard lock(g_visualTreeWatcherMutex);
        if (g_tearingDown || !g_active) {
            return;
        }
        watcher = g_visualTreeWatcher;
    }
    if (watcher) {
        watcher->SetAdvised(advised);
    }
}

// Arms (or re-arms) the calling UI thread's detach timer.
void ScheduleDetach() {
    auto context = GetThreadContext(true);
    if (!context->detachTimer) {
        auto queue = ws::DispatcherQueue::GetForCurrentThread();
        if (!queue) {
            return;
        }
        context->detachTimer = queue.CreateTimer();
        context->detachTimer.IsRepeating(false);
        context->detachTimer.Interval(std::chrono::milliseconds{kDetachAfterQuietMs});
        context->detachTimerToken = context->detachTimer.Tick(
            [](ws::DispatcherQueueTimer const& timer, wf::IInspectable const&) {
                if (!g_active) {
                    return;
                }
                if (GetTickCount64() - g_lastTreeActivityTick < kDetachAfterQuietMs) {
                    timer.Start();
                    return;
                }
                RequestAdvised(false);
            });
    }
    context->detachTimer.Start();
}

// Releasing a handle while the runtime is still walking the tree that reported
// it can destroy an element mid-walk, so releases are batched and drained from
// a timer once the thread has been quiet for a moment.
constexpr ULONGLONG kReleaseQuietMs = 200;

void DrainDiagnosticsReleases(ThreadContext& context) {
    if (GetTickCount64() - context.lastReleaseQueueTick < kReleaseQuietMs) {
        context.releaseTimer.Start();
        return;
    }

    auto pending = std::move(context.pendingReleases);
    context.pendingReleases.clear();
    if (!g_active) {
        return;  // the thread's uninit releases what is left
    }

    std::sort(pending.begin(), pending.end());
    pending.erase(std::unique(pending.begin(), pending.end()), pending.end());

    if (auto watcher = CurrentWatcher()) {
        for (auto handle : pending) {
            watcher->ReleaseHandle(handle);
        }
    }
}

void QueueDiagnosticsRelease(ThreadContext& context, InstanceHandle handle) {
    if (!handle) {
        return;
    }

    if (!context.releaseTimer) {
        auto queue = ws::DispatcherQueue::GetForCurrentThread();
        if (!queue) {
            return;
        }
        context.releaseTimer = queue.CreateTimer();
        context.releaseTimer.IsRepeating(false);
        context.releaseTimer.Interval(std::chrono::milliseconds{kReleaseQuietMs});
        context.releaseTimerToken = context.releaseTimer.Tick(
            [](ws::DispatcherQueueTimer const&, wf::IInspectable const&) {
                if (auto context = GetThreadContext(false)) {
                    DrainDiagnosticsReleases(*context);
                }
            });
    }

    context.pendingReleases.push_back(handle);
    context.lastReleaseQueueTick = GetTickCount64();
    if (!context.releaseTimer.IsRunning()) {
        context.releaseTimer.Start();
    }
}

class VerticalTabsTap
    : public winrt::implements<VerticalTabsTap, IObjectWithSite, winrt::non_agile> {
   public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override try {
        winrt::com_ptr<VisualTreeWatcher> previous;
        winrt::com_ptr<VisualTreeWatcher> watcher;
        {
            std::lock_guard lock(g_visualTreeWatcherMutex);
            previous = std::exchange(g_visualTreeWatcher, nullptr);
            m_site.copy_from(site);
            if (m_site) {
                if (g_tearingDown) {
                    // Arrived while the mod is unloading. Keep the reference
                    // InitializeXamlDiagnosticsEx took on this module, so the
                    // image stays mapped (and inert) under the runtime's
                    // reference to this object.
                    m_site = nullptr;
                    return S_OK;
                }
                // InitializeXamlDiagnosticsEx loaded this module once more;
                // hand that reference back so Windhawk can still unload it.
                FreeLibrary(GetCurrentModuleHandle());
                g_visualTreeWatcher = winrt::make_self<VisualTreeWatcher>(m_site);
                watcher = g_visualTreeWatcher;
            }
        }
        if (previous) {
            previous->SetAdvised(false);
        }
        if (watcher) {
            watcher->SetAdvised(true);
            NoteTreeActivity();
        }
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** site) noexcept override {
        return m_site.as(riid, site);
    }

   private:
    winrt::com_ptr<IUnknown> m_site;
};

struct TapFactory : winrt::implements<TapFactory, IClassFactory, winrt::non_agile> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,
                                             REFIID riid,
                                             void** object) override try {
        *object = nullptr;
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        return winrt::make<VerticalTabsTap>().as(riid, object);
    } catch (...) {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdll-attribute-on-redeclaration"

__declspec(dllexport) STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* object) try {
    if (clsid != CLSID_VerticalTabsTap) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    *object = nullptr;
    return winrt::make<TapFactory>().as(riid, object);
} catch (...) {
    return winrt::to_hresult();
}

__declspec(dllexport) STDAPI DllCanUnloadNow() {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}

#pragma clang diagnostic pop

std::atomic<bool> g_tapInjected{false};

void InjectTap() {
    if (g_tapInjected.exchange(true)) {
        return;
    }

    WCHAR modulePath[MAX_PATH];
    if (!GetModuleFileNameW(GetCurrentModuleHandle(), modulePath, ARRAYSIZE(modulePath))) {
        g_tapInjected = false;
        return;
    }

    HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr,
                                  LOAD_LIBRARY_SEARCH_SYSTEM32);
    using InitializeXamlDiagnosticsEx_t =
        HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);
    auto initializeXamlDiagnosticsEx =
        xaml ? reinterpret_cast<InitializeXamlDiagnosticsEx_t>(
                   GetProcAddress(xaml, "InitializeXamlDiagnosticsEx"))
             : nullptr;
    if (!initializeXamlDiagnosticsEx) {
        Wh_Log(L"InitializeXamlDiagnosticsEx is unavailable");
        g_tapInjected = false;
        return;
    }

    // Each attached tool uses its own connection name; take the first free one.
    HRESULT hr = E_FAIL;
    for (int i = 1; i <= 10000; i++) {
        WCHAR connectionName[64];
        swprintf_s(connectionName, L"VisualDiagConnection%d", i);
        hr = initializeXamlDiagnosticsEx(connectionName, GetCurrentProcessId(), L"",
                                         modulePath, CLSID_VerticalTabsTap, nullptr);
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            break;
        }
    }

    Wh_Log(L"InitializeXamlDiagnosticsEx: %08X", hr);
    if (FAILED(hr)) {
        g_tapInjected = false;
        return;
    }
    NoteTreeActivity();
    ScheduleDetach();
}

////////////////////////////////////////////////////////////////////////////////
// Running code on a window's UI thread

using RunOnThreadProc = void (*)(void* parameter);

struct RunOnThreadParam {
    RunOnThreadProc proc;
    void* parameter;
};

UINT GetRunOnThreadMessage() {
    static UINT message =
        RegisterWindowMessageW(L"Windhawk_RunOnWindowThread_" WH_MOD_ID);
    return message;
}

// Synchronously runs `proc` on the thread that owns `window`.
bool RunOnWindowThread(HWND window, RunOnThreadProc proc, void* parameter) {
    DWORD threadId = GetWindowThreadProcessId(window, nullptr);
    if (!threadId) {
        return false;
    }
    if (threadId == GetCurrentThreadId()) {
        proc(parameter);
        return true;
    }

    HHOOK hook = SetWindowsHookExW(
        WH_CALLWNDPROC,
        [](int code, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (code == HC_ACTION) {
                auto message = reinterpret_cast<const CWPSTRUCT*>(lParam);
                if (message->message == GetRunOnThreadMessage()) {
                    auto param = reinterpret_cast<RunOnThreadParam*>(message->lParam);
                    param->proc(param->parameter);
                }
            }
            return CallNextHookEx(nullptr, code, wParam, lParam);
        },
        nullptr, threadId);
    if (!hook) {
        return false;
    }

    RunOnThreadParam param{proc, parameter};
    SendMessageW(window, GetRunOnThreadMessage(), 0, reinterpret_cast<LPARAM>(&param));
    UnhookWindowsHookEx(hook);
    return true;
}

std::vector<HWND> GetTerminalWindows() {
    std::vector<HWND> windows;
    EnumWindows(
        [](HWND window, LPARAM lParam) -> BOOL {
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            WCHAR className[64];
            if (processId == GetCurrentProcessId() &&
                GetClassNameW(window, className, ARRAYSIZE(className)) &&
                wcscmp(className, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0) {
                reinterpret_cast<std::vector<HWND>*>(lParam)->push_back(window);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
    return windows;
}

// Runs `proc` once on every thread that owns a terminal window.
void ForEachWindowThread(RunOnThreadProc proc, void* parameter) {
    std::vector<DWORD> done;
    for (HWND window : GetTerminalWindows()) {
        DWORD threadId = GetWindowThreadProcessId(window, nullptr);
        if (std::find(done.begin(), done.end(), threadId) != done.end()) {
            continue;
        }
        done.push_back(threadId);
        RunOnWindowThread(window, proc, parameter);
    }
}

// Final teardown of the calling thread: layout back, timers stopped, templates
// dropped. The context itself is freed here, on its own thread.
void UninitializeCurrentThread() {
    auto context = GetThreadContext(false);
    if (!context) {
        return;
    }

    for (auto& window : context->windows) {
        try {
            UntrackWindow(*window);
        } catch (winrt::hresult_error const& ex) {
            Wh_Log(L"Restoring a window failed: %08X %s", ex.code().value,
                   ex.message().c_str());
        }
    }
    context->windows.clear();

    if (context->workTimer) {
        context->workTimer.Stop();
        context->workTimer.Tick(context->workTimerToken);
        context->workTimer = nullptr;
    }
    context->pendingTabRows.clear();

    if (context->releaseTimer) {
        context->releaseTimer.Stop();
        context->releaseTimer.Tick(context->releaseTimerToken);
        context->releaseTimer = nullptr;
    }

    if (context->detachTimer) {
        context->detachTimer.Stop();
        context->detachTimer.Tick(context->detachTimerToken);
        context->detachTimer = nullptr;
    }

#ifdef WTVT_TEST_HOOKS
    if (context->testHook) {
        UnhookWindowsHookEx(context->testHook);
        context->testHook = nullptr;
    }
    try {
        RevokeTestMenuHandler(*context);
    } catch (...) {
    }
#endif
    for (auto timer : context->windowTimers) {
        KillTimer(nullptr, timer);
    }
    context->windowTimers.clear();

    if (auto watcher = CurrentWatcher()) {
        for (auto handle : context->pendingReleases) {
            watcher->ReleaseHandle(handle);
        }
    }
    context->pendingReleases.clear();

    {
        std::lock_guard lock(g_contextsMutex);
        std::erase(g_contexts, context);
    }
    t_context = nullptr;
    delete context;
}

////////////////////////////////////////////////////////////////////////////////
// Attaching once XAML is up

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Original;

// Windows Terminal creates each window (CASCADIA_HOSTING_WINDOW_CLASS) and
// then, on the same thread and before it next pumps messages, the XAML island
// that fills it. A zero-delay thread timer set at window creation therefore
// fires once that window's XAML exists: the point to attach the diagnostics
// for the first time, or to attach them again for a new window's tab row.
HWND WINAPI CreateWindowExW_Hook(DWORD exStyle,
                                 LPCWSTR className,
                                 LPCWSTR windowName,
                                 DWORD style,
                                 int x,
                                 int y,
                                 int width,
                                 int height,
                                 HWND parent,
                                 HMENU menu,
                                 HINSTANCE instance,
                                 LPVOID param) {
    HWND window = CreateWindowExW_Original(exStyle, className, windowName, style, x, y,
                                           width, height, parent, menu, instance, param);

    if (window && g_active && !IS_INTRESOURCE(className) &&
        wcscmp(className, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0) {
        // The timer's callback is in this module, so its id is kept with the
        // thread's context, where the thread's uninit kills it if it hasn't
        // fired yet.
        auto context = GetThreadContext(true);
        UINT_PTR timer = SetTimer(nullptr, 0, 0, [](HWND, UINT, UINT_PTR timerId, DWORD) {
            KillTimer(nullptr, timerId);
            if (auto context = GetThreadContext(false)) {
                std::erase(context->windowTimers, timerId);
            }
            if (!g_active) {
                return;
            }
            if (!g_tapInjected) {
                InjectTap();
            } else {
                NoteTreeActivity();
                RequestAdvised(true);
                ScheduleDetach();
            }
        });
        if (timer) {
            context->windowTimers.push_back(timer);
        }
    }

    return window;
}

////////////////////////////////////////////////////////////////////////////////
// Windhawk entry points

BOOL Wh_ModInit() {
    Wh_Log(L">");

    LoadSettings();
    g_vertical = Wh_GetIntValue(L"vertical", 0) != 0;
    g_collapsed = Wh_GetIntValue(L"collapsed", 0) != 0;
    {
        // The image can be reused if an earlier unload had to keep it mapped.
        std::lock_guard lock(g_visualTreeWatcherMutex);
        g_tearingDown = false;
    }
    g_tapInjected = false;
    g_active = true;

    Wh_SetFunctionHook(reinterpret_cast<void*>(CreateWindowExW),
                       reinterpret_cast<void*>(CreateWindowExW_Hook),
                       reinterpret_cast<void**>(&CreateWindowExW_Original));
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");

    // Loaded into a terminal that is already running.
    auto windows = GetTerminalWindows();
    if (!windows.empty()) {
        RunOnWindowThread(windows.front(), [](void*) { InjectTap(); }, nullptr);
    }
}

void BeginTeardown() {
    std::lock_guard lock(g_visualTreeWatcherMutex);
    g_tearingDown = true;
    g_active = false;
}

void WaitForDiagnosticsWorkers() {
    for (int i = 0; i < 500 && g_diagnosticsWorkers > 0; i++) {
        Sleep(10);
    }
}

void Wh_ModBeforeUninit() {
    Wh_Log(L">");
    BeginTeardown();
}

void Wh_ModUninit() {
    Wh_Log(L">");
    BeginTeardown();

    // No lock held here: unadvising can wait on a UI thread, and UI threads
    // take g_visualTreeWatcherMutex.
    if (auto watcher = CurrentWatcher()) {
        watcher->UnadviseNow();
    }
    WaitForDiagnosticsWorkers();

    ForEachWindowThread([](void*) { UninitializeCurrentThread(); }, nullptr);

    // Once more, in case a worker queued before teardown attached it again.
    auto watcher = CurrentWatcher();
    if (watcher) {
        watcher->UnadviseNow();
        WaitForDiagnosticsWorkers();
        if (watcher->IsAdvised()) {
            // The runtime would call into this module after it is unloaded.
            // Keep it mapped instead; it stays inert because g_active is off.
            Wh_Log(L"Diagnostics could not be detached; keeping the module loaded");
            HMODULE pinned;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&GetCurrentModuleHandle), &pinned);
        }
    }

    std::lock_guard lock(g_visualTreeWatcherMutex);
    g_visualTreeWatcher = nullptr;
}

void Wh_ModSettingsChanged() {
    Wh_Log(L">");
    LoadSettings();
    ForEachWindowThread([](void*) { RefreshCurrentThread(); }, nullptr);
}
