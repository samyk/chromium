// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/media/capture/aura_window_video_capture_device.h"

#include <tuple>

#include "base/macros.h"
#include "base/run_loop.h"
#include "base/task/post_task.h"
#include "build/build_config.h"
#include "cc/test/pixel_test_utils.h"
#include "components/viz/common/features.h"
#include "content/browser/media/capture/content_capture_device_browsertest_base.h"
#include "content/browser/media/capture/fake_video_capture_stack.h"
#include "content/browser/media/capture/frame_test_util.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/desktop_media_id.h"
#include "content/public/browser/web_contents.h"
#include "content/shell/browser/shell.h"
#include "media/base/video_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/aura/window.h"
#include "ui/base/ui_base_features.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rect_f.h"

namespace content {
namespace {

class AuraWindowVideoCaptureDeviceBrowserTest
    : public ContentCaptureDeviceBrowserTestBase,
      public FrameTestUtil {
 public:
  AuraWindowVideoCaptureDeviceBrowserTest() = default;
  ~AuraWindowVideoCaptureDeviceBrowserTest() override = default;

  aura::Window* GetCapturedWindow() const {
#if defined(OS_CHROMEOS)
    // Since the LameWindowCapturerChromeOS will be used, just return the normal
    // shell window.
    return shell()->window();
#else
    // Note: The Window with an associated compositor frame sink (required for
    // capture) is the root window, which is an immediate ancestor of the
    // aura::Window provided by shell()->window().
    return shell()->window()->GetRootWindow();
#endif
  }

  // Returns the location of the content within the window.
  gfx::Rect GetWebContentsRect() const {
    aura::Window* const contents_window =
        shell()->web_contents()->GetNativeView();
    gfx::Rect rect = gfx::Rect(contents_window->bounds().size());
    aura::Window::ConvertRectToTarget(contents_window, GetCapturedWindow(),
                                      &rect);
    return rect;
  }

  // Runs the browser until a frame whose content matches the given |color| is
  // found in the captured frames queue, or until a testing failure has
  // occurred.
  void WaitForFrameWithColor(SkColor color) {
    VLOG(1) << "Waiting for frame content area filled with color: red="
            << SkColorGetR(color) << ", green=" << SkColorGetG(color)
            << ", blue=" << SkColorGetB(color);

    while (!testing::Test::HasFailure()) {
      EXPECT_TRUE(capture_stack()->started());
      EXPECT_FALSE(capture_stack()->error_occurred());
      capture_stack()->ExpectNoLogMessages();

      while (capture_stack()->has_captured_frames() &&
             !testing::Test::HasFailure()) {
        // Pop the next frame from the front of the queue and convert to a RGB
        // bitmap for analysis.
        const SkBitmap rgb_frame = capture_stack()->NextCapturedFrame();
        EXPECT_FALSE(rgb_frame.empty());

        // Three regions of the frame will be analyzed: 1) the WebContents
        // region containing a solid color, 2) the remaining part of the
        // captured window containing the content shell UI, and 3) the
        // solid-black letterboxed region surrounding them.
        const gfx::Size frame_size(rgb_frame.width(), rgb_frame.height());
        const gfx::Size window_size = GetExpectedSourceSize();
        const gfx::Rect webcontents_rect = GetWebContentsRect();

        // Compute the Rects representing where the three regions would be in
        // the frame.
        const gfx::RectF window_in_frame_rect_f(
            media::ComputeLetterboxRegion(gfx::Rect(frame_size), window_size));
        const gfx::RectF webcontents_in_frame_rect_f = TransformSimilarly(
            gfx::Rect(window_size), window_in_frame_rect_f, webcontents_rect);

#if defined(OS_CHROMEOS)
        // Browser window capture on ChromeOS uses the
        // LameWindowCapturerChromeOS, which takes RGB snapshots and then
        // software-converts them to YUV, and color accuracy is greatly reduced.
        // See comments in viz::CopyOutputResult::ReadI420Planes() for further
        // details on why this has to be.
        constexpr int max_color_diff = kVeryLooseMaxColorDifference;
#else
        // viz::SoftwareRenderer does not do color space management. Otherwise
        // (normal case), be strict about color differences.
        // TODO(crbug/795132): SkiaRenderer temporarily uses same code as
        // software compositor. Fix plumbing for SkiaRenderer.
        const int max_color_diff =
            (IsSoftwareCompositingTest() || features::IsUsingSkiaRenderer())
                ? kVeryLooseMaxColorDifference
                : kMaxColorDifference;
#endif

        // Determine the average RGB color in the three regions of the frame.
        const auto average_webcontents_rgb = ComputeAverageColor(
            rgb_frame, ToSafeIncludeRect(webcontents_in_frame_rect_f),
            gfx::Rect());
        const auto average_window_rgb = ComputeAverageColor(
            rgb_frame, ToSafeIncludeRect(window_in_frame_rect_f),
            ToSafeExcludeRect(webcontents_in_frame_rect_f));
        const auto average_letterbox_rgb =
            ComputeAverageColor(rgb_frame, gfx::Rect(frame_size),
                                ToSafeExcludeRect(window_in_frame_rect_f));

        VLOG(1) << "Video frame analysis: size=" << frame_size.ToString()
                << ", captured webcontents should be bound by approx. "
                << ToSafeIncludeRect(webcontents_in_frame_rect_f).ToString()
                << " and has average color " << average_webcontents_rgb
                << ", captured window should be bound by approx. "
                << ToSafeIncludeRect(window_in_frame_rect_f).ToString()
                << " and has average color " << average_window_rgb
                << ", letterbox region has average color "
                << average_letterbox_rgb;

        // The letterboxed region should always be black.
        if (IsFixedAspectRatioTest()) {
          EXPECT_TRUE(IsApproximatelySameColor(
              SK_ColorBLACK, average_letterbox_rgb, max_color_diff));
        }

        if (testing::Test::HasFailure()) {
          ADD_FAILURE() << "Test failure occurred at this frame; PNG dump: "
                        << cc::GetPNGDataUrl(rgb_frame);
          return;
        }

        // Return if the WebContents region now has the new |color|.
        if (IsApproximatelySameColor(color, average_webcontents_rgb,
                                     max_color_diff)) {
          VLOG(1) << "Observed desired frame.";
          return;
        } else {
          VLOG(3) << "PNG dump of undesired frame: "
                  << cc::GetPNGDataUrl(rgb_frame);
        }
      }

      // Wait for at least the minimum capture period before checking for more
      // captured frames.
      base::RunLoop run_loop;
      base::PostDelayedTaskWithTraits(FROM_HERE, {BrowserThread::UI},
                                      run_loop.QuitClosure(),
                                      GetMinCapturePeriod());
      run_loop.Run();
    }
  }

 protected:
  // Note: Test code should call <BaseClass>::GetExpectedSourceSize() instead of
  // this method since it has extra code to sanity-check that the source size is
  // not changing during the test.
  gfx::Size GetCapturedSourceSize() const final {
    return GetCapturedWindow()->bounds().size();
  }

  std::unique_ptr<FrameSinkVideoCaptureDevice> CreateDevice() final {
    const DesktopMediaID source_id = DesktopMediaID::RegisterNativeWindow(
        DesktopMediaID::TYPE_WINDOW, GetCapturedWindow());
    EXPECT_TRUE(DesktopMediaID::GetNativeWindowById(source_id));
    return std::make_unique<AuraWindowVideoCaptureDevice>(source_id);
  }

  void WaitForFirstFrame() final { WaitForFrameWithColor(SK_ColorBLACK); }

 private:
  DISALLOW_COPY_AND_ASSIGN(AuraWindowVideoCaptureDeviceBrowserTest);
};

// Tests that the device refuses to start if the target window was destroyed
// before the device could start.
IN_PROC_BROWSER_TEST_F(AuraWindowVideoCaptureDeviceBrowserTest,
                       ErrorsOutIfWindowHasGoneBeforeDeviceStart) {
  NavigateToInitialDocument();

  const DesktopMediaID source_id = DesktopMediaID::RegisterNativeWindow(
      DesktopMediaID::TYPE_WINDOW, GetCapturedWindow());
  EXPECT_TRUE(DesktopMediaID::GetNativeWindowById(source_id));
  const auto capture_params = SnapshotCaptureParams();

  // Close the Shell. This should close the window it owned, making the capture
  // target invalid.
  shell()->Close();

  // Create the device.
  auto device = std::make_unique<AuraWindowVideoCaptureDevice>(source_id);
  // Running the pending UI tasks should cause the device to realize the window
  // is gone.
  RunUntilIdle();

  // Attempt to start the device, and expect the video capture stack to have
  // been notified of the error.
  device->AllocateAndStartWithReceiver(capture_params,
                                       capture_stack()->CreateFrameReceiver());
  EXPECT_FALSE(capture_stack()->started());
  EXPECT_TRUE(capture_stack()->error_occurred());
  capture_stack()->ExpectHasLogMessages();

  device->StopAndDeAllocate();
  RunUntilIdle();
}

// Tests that the device starts, captures a frame, and then gracefully
// errors-out because the target window is destroyed before the device is
// stopped.
IN_PROC_BROWSER_TEST_F(AuraWindowVideoCaptureDeviceBrowserTest,
                       ErrorsOutWhenWindowIsDestroyed) {
  // TODO(crbug.com/877172): CopyOutputRequests not allowed.
  if (features::IsSingleProcessMash())
    return;
  NavigateToInitialDocument();
  AllocateAndStartAndWaitForFirstFrame();

  // Initially, the device captures any content changes normally.
  ChangePageContentColor(SK_ColorRED);
  WaitForFrameWithColor(SK_ColorRED);

  // Close the Shell. This should close the window it owned, causing a "target
  // permanently lost" error to propagate to the video capture stack.
  shell()->Close();
  RunUntilIdle();
  EXPECT_TRUE(capture_stack()->error_occurred());
  capture_stack()->ExpectHasLogMessages();

  StopAndDeAllocate();
}

// Tests that the device stops delivering frames while suspended. When resumed,
// any content changes that occurred during the suspend should cause a new frame
// to be delivered, to ensure the client is up-to-date.
IN_PROC_BROWSER_TEST_F(AuraWindowVideoCaptureDeviceBrowserTest,
                       SuspendsAndResumes) {
  // TODO(crbug.com/877172): CopyOutputRequests not allowed.
  if (features::IsSingleProcessMash())
    return;
  NavigateToInitialDocument();
  AllocateAndStartAndWaitForFirstFrame();

  // Initially, the device captures any content changes normally.
  ChangePageContentColor(SK_ColorRED);
  WaitForFrameWithColor(SK_ColorRED);

  // Suspend the device.
  device()->MaybeSuspend();
  RunUntilIdle();
  ClearCapturedFramesQueue();

  // Change the page content and run the browser for five seconds. Expect no
  // frames were queued because the device should be suspended.
  ChangePageContentColor(SK_ColorGREEN);
  base::RunLoop run_loop;
  base::PostDelayedTaskWithTraits(FROM_HERE, {BrowserThread::UI},
                                  run_loop.QuitClosure(),
                                  base::TimeDelta::FromSeconds(5));
  run_loop.Run();
  EXPECT_FALSE(HasCapturedFramesInQueue());

  // Resume the device and wait for an automatic refresh frame containing the
  // content that was updated while the device was suspended.
  device()->Resume();
  WaitForFrameWithColor(SK_ColorGREEN);

  StopAndDeAllocate();
}

// Tests that the device delivers refresh frames when asked, while the source
// content is not changing.
IN_PROC_BROWSER_TEST_F(AuraWindowVideoCaptureDeviceBrowserTest,
                       DeliversRefreshFramesUponRequest) {
  // TODO(crbug.com/877172): CopyOutputRequests not allowed.
  if (features::IsSingleProcessMash())
    return;
  NavigateToInitialDocument();
  AllocateAndStartAndWaitForFirstFrame();

  // Set the page content to a known color.
  ChangePageContentColor(SK_ColorRED);
  WaitForFrameWithColor(SK_ColorRED);

  // Without making any further changes to the source (which would trigger
  // frames to be captured), request and wait for ten refresh frames.
  for (int i = 0; i < 10; ++i) {
    ClearCapturedFramesQueue();
    device()->RequestRefreshFrame();
    WaitForFrameWithColor(SK_ColorRED);
  }

  StopAndDeAllocate();
}

#if defined(OS_CHROMEOS)
// On ChromeOS, another window may occlude a window that is being captured.
// Make sure the visibility is set to visible during capture if it's occluded.
IN_PROC_BROWSER_TEST_F(AuraWindowVideoCaptureDeviceBrowserTest,
                       CapturesOccludedWindows) {
  // TODO(crbug.com/877172): CopyOutputRequests not allowed.
  if (features::IsSingleProcessMash())
    return;
  NavigateToInitialDocument();
  AllocateAndStartAndWaitForFirstFrame();

  ASSERT_EQ(aura::Window::OcclusionState::VISIBLE,
            shell()->web_contents()->GetNativeView()->occlusion_state());
  // Create a window on top of the window being captured with same size so that
  // it is occluded.
  auto window = std::make_unique<aura::Window>(nullptr);
  window->Init(ui::LAYER_TEXTURED);
  shell()->window()->GetRootWindow()->AddChild(window.get());
  window->SetBounds(shell()->window()->bounds());
  window->Show();
  EXPECT_EQ(aura::Window::OcclusionState::VISIBLE,
            shell()->web_contents()->GetNativeView()->occlusion_state());

  window.reset();
  StopAndDeAllocate();
}
#endif  // defined(OS_CHROMEOS)

class AuraWindowVideoCaptureDeviceBrowserTestP
    : public AuraWindowVideoCaptureDeviceBrowserTest,
      public testing::WithParamInterface<std::tuple<bool, bool>> {
 public:
  bool IsSoftwareCompositingTest() const override {
    return std::get<0>(GetParam());
  }
  bool IsFixedAspectRatioTest() const override {
    return std::get<1>(GetParam());
  }
};

#if defined(OS_CHROMEOS)
INSTANTIATE_TEST_SUITE_P(
    ,
    AuraWindowVideoCaptureDeviceBrowserTestP,
    testing::Combine(
        // Note: On ChromeOS, software compositing is not an option.
        testing::Values(false /* GPU-accelerated compositing */),
        testing::Values(false /* variable aspect ratio */,
                        true /* fixed aspect ratio */)));
#else
INSTANTIATE_TEST_SUITE_P(
    ,
    AuraWindowVideoCaptureDeviceBrowserTestP,
    testing::Combine(testing::Values(false /* GPU-accelerated compositing */,
                                     true /* software compositing */),
                     testing::Values(false /* variable aspect ratio */,
                                     true /* fixed aspect ratio */)));
#endif  // defined(OS_CHROMEOS)

// Tests that the device successfully captures a series of content changes,
// whether the browser is running with software compositing or GPU-accelerated
// compositing.
IN_PROC_BROWSER_TEST_P(AuraWindowVideoCaptureDeviceBrowserTestP,
                       CapturesContentChanges) {
  // TODO(crbug.com/877172): CopyOutputRequests not allowed.
  if (features::IsSingleProcessMash())
    return;
  SCOPED_TRACE(testing::Message()
               << "Test parameters: "
               << (IsSoftwareCompositingTest() ? "Software Compositing"
                                               : "GPU Compositing")
               << " with "
               << (IsFixedAspectRatioTest() ? "Fixed Video Aspect Ratio"
                                            : "Variable Video Aspect Ratio"));

  NavigateToInitialDocument();
  AllocateAndStartAndWaitForFirstFrame();

  ASSERT_EQ(shell()->web_contents()->GetVisibility(),
            content::Visibility::VISIBLE);

  static constexpr SkColor kColorsToCycleThrough[] = {
      SK_ColorRED,  SK_ColorGREEN,   SK_ColorBLUE,  SK_ColorYELLOW,
      SK_ColorCYAN, SK_ColorMAGENTA, SK_ColorWHITE,
  };
  for (SkColor color : kColorsToCycleThrough) {
    ChangePageContentColor(color);
    WaitForFrameWithColor(color);
  }

  StopAndDeAllocate();
}

}  // namespace
}  // namespace content
