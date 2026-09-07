import Foundation
import Combine

/// 整合 `DiskMonitor`、`MountController` 與 `ExtensionStatus` 的應用程式狀態。
///
/// 由 `AppDelegate` 在啟動時呼叫 `start()`，並透過 `.environmentObject` 注入各個視圖。
@MainActor
final class AppState: ObservableObject {
  /// 全域共用的單一實例（App 與 AppDelegate 都需要存取同一份狀態）。
  static let shared = AppState()

  let diskMonitor: DiskMonitor
  let mountController: MountController
  let extensionStatus: ExtensionStatus

  /// 延伸功能狀態的定期更新計時器。
  private var refreshTimer: Timer?
  /// 目前可見的視圖數量（選單列彈出視窗或主視窗），大於 0 時才啟用定期更新。
  private var visibleViewCount = 0
  private var started = false

  init() {
    let monitor = DiskMonitor()
    let controller = MountController()
    let status = ExtensionStatus()

    self.diskMonitor = monitor
    self.mountController = controller
    self.extensionStatus = status

    // 將磁碟事件轉交給掛載控制器，處理自動掛載與重試狀態。
    controller.diskMonitor = monitor
    monitor.onVolumeAppeared = { [weak controller] volume in
      controller?.handleAppeared(volume)
    }
    monitor.onVolumeDisappeared = { [weak controller] bsdName in
      controller?.handleDisappeared(bsdName)
    }
  }

  /// 啟動磁碟監控並讀取延伸功能狀態。應在應用程式啟動完成時呼叫一次。
  func start() {
    guard !started else { return }
    started = true
    diskMonitor.start()
    Task { await extensionStatus.refresh() }
  }

  /// 視圖出現時呼叫：立即更新延伸功能狀態，並開始每 30 秒定期更新。
  func viewDidAppear() {
    visibleViewCount += 1
    Task { await extensionStatus.refresh() }
    diskMonitor.refresh()
    startPeriodicRefreshIfNeeded()
  }

  /// 視圖消失時呼叫：沒有任何視圖可見時停止定期更新。
  func viewDidDisappear() {
    visibleViewCount = max(0, visibleViewCount - 1)
    if visibleViewCount == 0 {
      stopPeriodicRefresh()
    }
  }

  /// 手動重新整理磁碟清單與延伸功能狀態。
  func refreshAll() {
    diskMonitor.refresh()
    Task { await extensionStatus.refresh() }
  }

  private func startPeriodicRefreshIfNeeded() {
    guard refreshTimer == nil else { return }
    refreshTimer = Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { _ in
      Task { @MainActor in
        await AppState.shared.extensionStatus.refresh()
        AppState.shared.diskMonitor.refresh()
      }
    }
  }

  private func stopPeriodicRefresh() {
    refreshTimer?.invalidate()
    refreshTimer = nil
  }
}
