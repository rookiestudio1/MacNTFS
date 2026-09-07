import SwiftUI
import AppKit

/// MacNTFS：常駐於選單列的 NTFS 磁碟掛載工具。
@main
struct MacNTFSApp: App {
  @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

  private let appState = AppState.shared

  var body: some Scene {
    MenuBarExtra("MacNTFS", systemImage: "externaldrive") {
      MenuBarView()
        .environmentObject(appState)
        .environmentObject(appState.diskMonitor)
        .environmentObject(appState.mountController)
        .environmentObject(appState.extensionStatus)
    }
    .menuBarExtraStyle(.window)

    Window("MacNTFS", id: "main") {
      MainWindowView()
        .environmentObject(appState)
        .environmentObject(appState.diskMonitor)
        .environmentObject(appState.mountController)
        .environmentObject(appState.extensionStatus)
    }
    .defaultSize(width: 600, height: 520)
    .windowResizability(.contentMinSize)
  }
}

/// 應用程式委派：啟動時開始監看磁碟，並確保以選單列輔助程式的方式執行。
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
  func applicationDidFinishLaunching(_ notification: Notification) {
    // Info.plist 已設定 LSUIElement，這裡再保險設定一次，避免出現在 Dock。
    NSApp.setActivationPolicy(.accessory)
    AppState.shared.start()
  }

  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    // 關閉主視窗後仍保留在選單列。
    false
  }

  func applicationWillTerminate(_ notification: Notification) {
    AppState.shared.diskMonitor.stop()
  }
}
