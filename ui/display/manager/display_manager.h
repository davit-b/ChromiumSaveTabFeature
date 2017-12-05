// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_DISPLAY_MANAGER_DISPLAY_MANAGER_H_
#define UI_DISPLAY_MANAGER_DISPLAY_MANAGER_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/callback.h"
#include "base/compiler_specific.h"
#include "base/gtest_prod_util.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "ui/display/display.h"
#include "ui/display/display_layout.h"
#include "ui/display/display_observer.h"
#include "ui/display/manager/display_manager_export.h"
#include "ui/display/manager/managed_display_info.h"
#include "ui/display/mojo/dev_display_controller.mojom.h"
#include "ui/display/types/display_constants.h"
#include "ui/display/unified_desktop_utils.h"

#if defined(OS_CHROMEOS)
#include "base/optional.h"
#include "ui/display/manager/chromeos/display_configurator.h"
#include "ui/display/manager/chromeos/touch_device_manager.h"
#endif

namespace gfx {
class Insets;
class Rect;
}

namespace display {
using DisplayInfoList = std::vector<ManagedDisplayInfo>;

class DisplayLayoutStore;
class DisplayObserver;
class Screen;

namespace test {
class DisplayManagerTestApi;
}

// DisplayManager maintains the current display configurations,
// and notifies observers when configuration changes.
class DISPLAY_MANAGER_EXPORT DisplayManager
#if defined(OS_CHROMEOS)
    : public DisplayConfigurator::SoftwareMirroringController
#endif
{
 public:
  class DISPLAY_MANAGER_EXPORT Delegate {
   public:
    virtual ~Delegate() {}

    // Create or updates the mirroring window with |display_info_list|.
    virtual void CreateOrUpdateMirroringDisplay(
        const DisplayInfoList& display_info_list) = 0;

    // Closes the mirror window if not necessary.
    virtual void CloseMirroringDisplayIfNotNecessary() = 0;

    // Called before and after the display configuration changes.  When
    // |clear_focus| is true, the implementation should deactivate the active
    // window and set the focus window to NULL.
    virtual void PreDisplayConfigurationChange(bool clear_focus) = 0;
    virtual void PostDisplayConfigurationChange() = 0;

#if defined(OS_CHROMEOS)
    // Get the DisplayConfigurator.
    virtual DisplayConfigurator* display_configurator() = 0;
#endif
  };

  // How secondary displays will be used.
  // 1) EXTENDED mode extends the desktop onto additional displays, creating one
  //    root window for each display. Each display has a shelf and status tray,
  //    and each user window is only rendered on a single display.
  // 2) MIRRORING mode copies the content of the primary display to the second
  //    display via software mirroring. This only supports 2 displays for now.
  // 3) UNIFIED mode creates a virtual desktop with a *single* root window that
  //    spans multiple physical displays via software mirroring. The primary
  //    physical display has a shelf and status tray, and user windows may
  //    render spanning across multiple displays.
  enum MultiDisplayMode {
    EXTENDED = 0,
    MIRRORING,
    UNIFIED,
  };

  explicit DisplayManager(std::unique_ptr<Screen> screen);
#if defined(OS_CHROMEOS)
  ~DisplayManager() override;
#else
  ~DisplayManager();
#endif

  DisplayLayoutStore* layout_store() { return layout_store_.get(); }

  void set_delegate(Delegate* delegate) { delegate_ = delegate; }

  // When set to true, the DisplayManager calls OnDisplayMetricsChanged even if
  // the display's bounds didn't change. Used to swap primary display.
  void set_force_bounds_changed(bool force_bounds_changed) {
    force_bounds_changed_ = force_bounds_changed;
  }

  void set_configure_displays(bool configure_displays) {
    configure_displays_ = configure_displays;
  }

  void set_internal_display_has_accelerometer(bool has_accelerometer) {
    internal_display_has_accelerometer_ = has_accelerometer;
  }

  // Returns the display id of the first display in the outupt list.
  int64_t first_display_id() const { return first_display_id_; }

#if defined(OS_CHROMEOS)
  TouchDeviceManager* touch_device_manager() const {
    return touch_device_manager_.get();
  }
#endif

  bool is_multi_mirroring_enabled() const {
    return is_multi_mirroring_enabled_;
  }

  const UnifiedDesktopLayoutMatrix& current_unified_desktop_matrix() const {
    return current_unified_desktop_matrix_;
  }

  // Sets controller used to add/remove fake displays. If this is set then
  // AddRemoveDisplay() will delegate out to |dev_display_controller_| instead
  // of adding/removing a ManagedDisplayInfo.
  void SetDevDisplayController(mojom::DevDisplayControllerPtr controller);

  // Initializes displays using command line flag. Returns false if no command
  // line flag was provided.
  bool InitFromCommandLine();

  // Initialize default display.
  void InitDefaultDisplay();

  // Update the internal display's display info.
  void UpdateInternalDisplay(const ManagedDisplayInfo& display_info);

  // Initializes font related params that depends on display configuration.
  void RefreshFontParams();

  // Returns the display layout used for current displays.
  const DisplayLayout& GetCurrentDisplayLayout() const;

  // Returns the actual display layout after it has been resolved and applied.
  const DisplayLayout& GetCurrentResolvedDisplayLayout() const;

  // Returns the current display list.
  DisplayIdList GetCurrentDisplayIdList() const;

  // Sets the layout for the current display pair. The |layout| specifies the
  // locaion of the displays relative to their parents.
  void SetLayoutForCurrentDisplays(std::unique_ptr<DisplayLayout> layout);

  // Returns display for given |display_id|.
  const Display& GetDisplayForId(int64_t display_id) const;

  // Checks the validity of given |display_id|.
  bool IsDisplayIdValid(int64_t display_id) const;

  // Finds the display that contains |point| in screeen coordinates.  Returns
  // invalid display if there is no display that can satisfy the condition.
  const Display& FindDisplayContainingPoint(
      const gfx::Point& point_in_screen) const;

  // Sets the work area's |insets| to the display given by |display_id|.
  bool UpdateWorkAreaOfDisplay(int64_t display_id, const gfx::Insets& insets);

  // Registers the overscan insets for the display of the specified ID. Note
  // that the insets size should be specified in DIP size. It also triggers the
  // display's bounds change.
  void SetOverscanInsets(int64_t display_id, const gfx::Insets& insets_in_dip);

  // Sets the display's rotation for the given |source|. The new |rotation| will
  // also become active.
  void SetDisplayRotation(int64_t display_id,
                          Display::Rotation rotation,
                          Display::RotationSource source);

  // Resets the UI scale of the display with |display_id| to the one defined in
  // the default mode.
  bool ResetDisplayToDefaultMode(int64_t display_id);

  // Sets the external display's configuration, including resolution change,
  // ui-scale change, and device scale factor change. Returns true if it changes
  // the display resolution so that the caller needs to show a notification in
  // case the new resolution actually doesn't work.
  bool SetDisplayMode(int64_t display_id,
                      const ManagedDisplayMode& display_mode);

  // Register per display properties.
  // |overscan_insets| is null if the display has no custom overscan insets.
  // |touch_calibration_data| is null if the display has no touch calibration
  // associated data.
  void RegisterDisplayProperty(int64_t display_id,
                               Display::Rotation rotation,
                               float ui_scale,
                               const gfx::Insets* overscan_insets,
                               const gfx::Size& resolution_in_pixels,
                               float device_scale_factor);

  // Register stored rotation properties for the internal display.
  void RegisterDisplayRotationProperties(bool rotation_lock,
                                         Display::Rotation rotation);

  // Returns the stored rotation lock preference if it has been loaded,
  // otherwise false.
  bool registered_internal_display_rotation_lock() const {
    return registered_internal_display_rotation_lock_;
  }

  // Returns the stored rotation preference for the internal display if it has
  // been loaded, otherwise |Display::Rotate_0|.
  Display::Rotation registered_internal_display_rotation() const {
    return registered_internal_display_rotation_;
  }

  // Fills in the display |mode| currently in use in |display_id| if found,
  // returning true in that case, otherwise false.
  bool GetActiveModeForDisplayId(int64_t display_id,
                                 ManagedDisplayMode* mode) const;

  // Returns true and fills in the display's selected |mode| if found, or false.
  bool GetSelectedModeForDisplayId(int64_t display_id,
                                   ManagedDisplayMode* mode) const;

  // Sets the selected mode of |display_id| to |display_mode| if it's a
  // supported mode. This doesn't trigger reconfiguration or observers
  // notifications. This is suitable to be used from within an observer
  // notification to prevent reentrance to UpdateDisplaysWith().
  void SetSelectedModeForDisplayId(int64_t display_id,
                                   const ManagedDisplayMode& display_mode);

  // Tells if the virtual resolution feature is enabled.
  bool IsDisplayUIScalingEnabled() const;

  // Returns the current overscan insets for the specified |display_id|.
  // Returns an empty insets (0, 0, 0, 0) if no insets are specified for the
  // display.
  gfx::Insets GetOverscanInsets(int64_t display_id) const;

  // Called when display configuration has changed. The new display
  // configurations is passed as a vector of Display object, which contains each
  // display's new infomration.
  void OnNativeDisplaysChanged(
      const std::vector<ManagedDisplayInfo>& display_info_list);

  // Updates the internal display data and notifies observers about the changes.
  void UpdateDisplaysWith(
      const std::vector<ManagedDisplayInfo>& display_info_list);

  // Updates current displays using current |display_info_|.
  void UpdateDisplays();

  // Returns the display at |index|. The display at 0 is no longer considered
  // "primary".
  const Display& GetDisplayAt(size_t index) const;

  const Display& GetPrimaryDisplayCandidate() const;

  // Returns the logical number of displays. This returns 1 when displays are
  // mirrored.
  size_t GetNumDisplays() const;

  // Returns only the currently active displays. This list does not include the
  // displays that will be removed if |UpdateDisplaysWith| is currently
  // executing.
  // See https://crbug.com/632755
  const Displays& active_only_display_list() const {
    return is_updating_display_list_ ? active_only_display_list_
                                     : active_display_list();
  }

  const Displays& active_display_list() const { return active_display_list_; }

  // Returns true if the display specified by |display_id| is currently
  // connected and active. (mirroring display isn't active, for example).
  bool IsActiveDisplayId(int64_t display_id) const;

  // Returns the number of connected displays. This returns 2 when displays are
  // mirrored.
  size_t num_connected_displays() const { return num_connected_displays_; }

  // Returns true if either software or hardware mirror mode is active.
  bool IsInMirrorMode() const;

  // Returns true if software mirror mode is active. Note that when
  // SoftwareMirroringEnabled() returns true, it only means software mirroring
  // mode is requested, but it does not guarantee that the mode is active. The
  // mode will be active after UpdateDisplaysWith() is called.
  bool IsInSoftwareMirrorMode() const;

  // Returns true if hardware mirror mode is active.
  bool IsInHardwareMirrorMode() const;

  int64_t mirroring_source_id() const { return mirroring_source_id_; }

  // Returns a list of mirroring destination display ids.
  DisplayIdList GetMirroringDestinationDisplayIdList() const;

  const Displays& software_mirroring_display_list() const {
    return software_mirroring_display_list_;
  }

  // Remove mirroring source and destination displays, so that they will be
  // updated when UpdateDisplaysWith() is called.
  void ClearMirroringSourceAndDestination();

  // Sets/gets if the unified desktop feature is enabled.
  void SetUnifiedDesktopEnabled(bool enabled);
  bool unified_desktop_enabled() const { return unified_desktop_enabled_; }

  // Returns true if it's in unified desktop mode.
  bool IsInUnifiedMode() const;

  // Sets the Unified Desktop layout using the given |matrix| and sets the
  // current mode to Unified Desktop.
  void SetUnifiedDesktopMatrix(const UnifiedDesktopLayoutMatrix& matrix);

  // In Unified Desktop mode, we consider the first mirroring display to be the
  // primary. It's also the top-left display in the layout matrix, and it's
  // where the shelf is placed.
  // This returns nullptr if we're not in unified desktop mode.
  const Display* GetPrimaryMirroringDisplayForUnifiedDesktop() const;

  // Returns the index of the row in the Unified Mode layout matrix which
  // contains the display with |display_id|.
  int GetMirroringDisplayRowIndexInUnifiedMatrix(int64_t display_id) const;

  // Returns the maximum display height of the row with |row_index| in the
  // Unified Mode layout matrix.
  int GetUnifiedDesktopRowMaxHeight(int row_index) const;

  // Returns the display used for software mirrroring. Returns invalid display
  // if not found.
  const Display GetMirroringDisplayById(int64_t id) const;

  // Retuns the display info associated with |display_id|.
  const ManagedDisplayInfo& GetDisplayInfo(int64_t display_id) const;

  // Returns the human-readable name for the display |id|.
  std::string GetDisplayNameForId(int64_t id);

  // Returns the display id that is capable of UI scaling. On device, this
  // returns internal display's ID if its device scale factor is 2, or invalid
  // ID if such internal display doesn't exist. On linux desktop, this returns
  // the first display ID.
  int64_t GetDisplayIdForUIScaling() const;

  // Change the mirror mode.
  void SetMirrorMode(bool mirrored);

  // Used to emulate display change when run in a desktop environment instead
  // of on a device.
  void AddRemoveDisplay();
  void ToggleDisplayScaleFactor();

// SoftwareMirroringController override:
#if defined(OS_CHROMEOS)
  void SetSoftwareMirroring(bool enabled) override;
  bool SoftwareMirroringEnabled() const override;
  void SetTouchCalibrationData(
      int64_t display_id,
      const TouchCalibrationData::CalibrationPointPairQuad& point_pair_quad,
      const gfx::Size& display_bounds,
      const TouchDeviceIdentifier& touch_device_identifier);
  void ClearTouchCalibrationData(
      int64_t display_id,
      base::Optional<TouchDeviceIdentifier> touch_device_identifier);
#endif

  // Sets/gets default multi display mode.
  void SetDefaultMultiDisplayModeForCurrentDisplays(MultiDisplayMode mode);
  MultiDisplayMode current_default_multi_display_mode() const {
    return current_default_multi_display_mode_;
  }

  // Sets multi display mode.
  void SetMultiDisplayMode(MultiDisplayMode mode);

  // Reconfigure display configuration using the same physical display.
  // TODO(oshima): Refactor and move this impl to |SetDefaultMultiDisplayMode|.
  void ReconfigureDisplays();

  // Update the bounds of the display given by |display_id|.
  bool UpdateDisplayBounds(int64_t display_id, const gfx::Rect& new_bounds);

  // Creates mirror window asynchronously if the software mirror mode is
  // enabled.
  void CreateMirrorWindowAsyncIfAny();

  // A unit test may change the internal display id (which never happens on a
  // real device). This will update the mode list for internal display for this
  // test scenario.
  void UpdateInternalManagedDisplayModeListForTest();

  // Zoom the internal display.
  bool ZoomInternalDisplay(bool up);

  // Reset the internal display zoom.
  void ResetInternalDisplayZoom();

  // Notifies observers of display configuration changes.
  void NotifyMetricsChanged(const Display& display, uint32_t metrics);
  void NotifyDisplayAdded(const Display& display);
  void NotifyDisplayRemoved(const Display& display);

  // Delegated from the Screen implementation.
  void AddObserver(DisplayObserver* observer);
  void RemoveObserver(DisplayObserver* observer);

  // Returns a Display object for a secondary display if it exists or returns
  // invalid display if there is no secondary display.  TODO(rjkroege): Display
  // swapping is an obsolete feature pre-dating multi-display support so remove
  // it.
  const Display& GetSecondaryDisplay() const;

 private:
  friend class test::DisplayManagerTestApi;

  // See description above |notify_depth_| for details.
  class BeginEndNotifier {
   public:
    explicit BeginEndNotifier(DisplayManager* display_manager);
    ~BeginEndNotifier();

   private:
    DisplayManager* display_manager_;

    DISALLOW_COPY_AND_ASSIGN(BeginEndNotifier);
  };

  void set_change_display_upon_host_resize(bool value) {
    change_display_upon_host_resize_ = value;
  }

  // Creates software mirroring display related information. The display used to
  // mirror the content is removed from the |display_info_list|.
  void CreateSoftwareMirroringDisplayInfo(DisplayInfoList* display_info_list);

  // Same as above but for Unified Desktop.
  void CreateUnifiedDesktopDisplayInfo(DisplayInfoList* display_info_list);

  Display* FindDisplayForId(int64_t id);

  // Add the mirror display's display info if the software based mirroring is in
  // use. This should only be called before UpdateDisplaysWith().
  void AddMirrorDisplayInfoIfAny(DisplayInfoList* display_info_list);

  // Inserts and update the ManagedDisplayInfo according to the overscan state.
  // Note that The ManagedDisplayInfo stored in the |internal_display_info_| can
  // be different from |new_info| (due to overscan state), so you must use
  // |GetDisplayInfo| to get the correct ManagedDisplayInfo for a display.
  void InsertAndUpdateDisplayInfo(const ManagedDisplayInfo& new_info);

  // Creates a display object from the ManagedDisplayInfo for
  // |display_id|.
  Display CreateDisplayFromDisplayInfoById(int64_t display_id);

  // Creates a display object from the ManagedDisplayInfo for |display_id| for
  // mirroring. The size of the display will be scaled using |scale| with the
  // offset using |origin|.
  Display CreateMirroringDisplayFromDisplayInfoById(int64_t display_id,
                                                    const gfx::Point& origin,
                                                    float scale);

  // Updates the bounds of all non-primary displays in |display_list| and append
  // the indices of displays updated to |updated_indices|.  When the size of
  // |display_list| equals 2, the bounds are updated using the layout registered
  // for the display pair. For more than 2 displays, the bounds are updated
  // using horizontal layout.
  void UpdateNonPrimaryDisplayBoundsForLayout(
      Displays* display_list,
      std::vector<size_t>* updated_indices);

  void CreateMirrorWindowIfAny();

  void RunPendingTasksForTest();

  // Applies the |layout| and updates the bounds of displays in |display_list|.
  // |updated_ids| contains the ids for displays whose bounds have changed.
  void ApplyDisplayLayout(DisplayLayout* layout,
                          Displays* display_list,
                          std::vector<int64_t>* updated_ids);

  Delegate* delegate_ = nullptr;  // not owned.

  // When set to true, DisplayManager will use DisplayConfigurator to configure
  // displays. By default, this is set to true when running on device and false
  // when running off device.
  bool configure_displays_ = false;

  std::unique_ptr<Screen> screen_;

  std::unique_ptr<DisplayLayoutStore> layout_store_;

  std::unique_ptr<DisplayLayout> current_resolved_layout_;

  // The matrix that's used to layout the displays in Unified Desktop mode.
  UnifiedDesktopLayoutMatrix current_unified_desktop_matrix_;

  std::map<int64_t, int> mirroring_display_id_to_unified_matrix_row_;

  std::vector<int> unified_display_rows_heights_;

  int64_t first_display_id_ = kInvalidDisplayId;

  // List of current active displays.
  Displays active_display_list_;
  // This list does not include the displays that will be removed if
  // |UpdateDisplaysWith| is under execution.
  // See https://crbug.com/632755
  Displays active_only_display_list_;

  // True if active_display_list is being modified and has displays that are not
  // presently active.
  // See https://crbug.com/632755
  bool is_updating_display_list_ = false;

  size_t num_connected_displays_ = 0;

  bool force_bounds_changed_ = false;

  // The mapping from the display ID to its internal data.
  std::map<int64_t, ManagedDisplayInfo> display_info_;

  // Selected display modes for displays. Key is the displays' ID.
  std::map<int64_t, ManagedDisplayMode> display_modes_;

  // When set to true, the host window's resize event updates the display's
  // size. This is set to true when running on desktop environment (for
  // debugging) so that resizing the host window will update the display
  // properly. This is set to false on device as well as during the unit tests.
  bool change_display_upon_host_resize_ = false;

  MultiDisplayMode multi_display_mode_ = EXTENDED;
  MultiDisplayMode current_default_multi_display_mode_ = EXTENDED;

  // This is used in two distinct ways:
  // 1. The source display id when software mirroring is active.
  // 2. There's no source and destination display in hardware mirroring, so we
  // treat the first mirroring display id as source id when hardware mirroring
  // is active.
  int64_t mirroring_source_id_ = kInvalidDisplayId;

  // This is used in two distinct ways:
  // 1. when software mirroring is active this contains the destination
  // displays.
  // 2. when unified mode is enabled this is the set of physical displays.
  Displays software_mirroring_display_list_;

  // There's no source and destination display in hardware mirroring, so we
  // treat the first mirroring display as source and store its id in
  // |mirroring_source_id_| and treat the rest of mirroring displays as
  // destination and store their ids in this list.
  DisplayIdList hardware_mirroring_display_id_list_;

  // Cached mirror mode for metrics changed notification.
  bool mirror_mode_for_metrics_ = false;

  // User preference for rotation lock of the internal display.
  bool registered_internal_display_rotation_lock_ = false;

  // User preference for the rotation of the internal display.
  Display::Rotation registered_internal_display_rotation_ = Display::ROTATE_0;

  bool unified_desktop_enabled_ = false;

  bool internal_display_has_accelerometer_ = false;

  base::Closure created_mirror_window_;

  base::ObserverList<DisplayObserver> observers_;

  display::mojom::DevDisplayControllerPtr dev_display_controller_;

  // This is incremented whenever a BeginEndNotifier is created and decremented
  // when destroyed. BeginEndNotifier uses this to track when it should call
  // OnWillProcessDisplayChanges() and OnDidProcessDisplayChanges().
  int notify_depth_ = 0;

#if defined(OS_CHROMEOS)
  std::unique_ptr<TouchDeviceManager> touch_device_manager_;
#endif

  // Whether mirroring across multiple displays is enabled.
  bool is_multi_mirroring_enabled_;

  base::WeakPtrFactory<DisplayManager> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(DisplayManager);
};

}  // namespace display

#endif  // UI_DISPLAY_MANAGER_DISPLAY_MANAGER_H_