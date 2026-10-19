import Foundation
import Combine
import AppKit
import FSKit

/// 查詢 MacNTFS 的 FSKit 延伸功能是否已安裝並啟用。
@MainActor
final class ExtensionStatus: ObservableObject {
  /// 延伸功能的 bundle identifier。
  static let extensionBundleIdentifier = "com.rookiestudio.MacNTFS.NTFSFSExtension"

  enum State: Equatable, Sendable {
    /// 尚未查詢。
    case unknown
    /// 系統中找不到此延伸功能（通常是 App 尚未被 macOS 註冊）。
    case notInstalled
    /// 已安裝但使用者尚未在「系統設定」中啟用。
    case installedDisabled
    /// 已啟用，可正常掛載。
    case enabled
    /// 查詢失敗。
    case error(String)

    /// 顯示於 UI 的說明文字。
    var label: String {
      switch self {
      case .unknown:
        return String(localized: "正在檢查延伸功能狀態…")
      case .notInstalled:
        return String(localized: "尚未安裝 NTFS 延伸功能")
      case .installedDisabled:
        return String(localized: "NTFS 延伸功能尚未啟用")
      case .enabled:
        return String(localized: "NTFS 延伸功能已啟用")
      case .error(let message):
        return String(localized: "無法檢查延伸功能狀態：\(message)")
      }
    }

    /// 是否需要使用者到系統設定處理。
    var needsAttention: Bool {
      switch self {
      case .enabled, .unknown:
        return false
      case .notInstalled, .installedDisabled, .error:
        return true
      }
    }
  }

  /// 可跨執行緒傳遞的延伸功能摘要。
  private struct ModuleInfo: Sendable {
    let bundleIdentifier: String
    let isEnabled: Bool
  }

  @Published private(set) var state: State = .unknown
  /// 最後一次成功查詢的時間。
  @Published private(set) var lastChecked: Date?

  private var isRefreshing = false

  init() {}

  /// 重新查詢延伸功能狀態。
  func refresh() async {
    guard !isRefreshing else { return }
    isRefreshing = true
    defer { isRefreshing = false }

    do {
      let modules = try await Self.fetchInstalledModules()
      if let module = modules.first(where: { $0.bundleIdentifier == Self.extensionBundleIdentifier }) {
        state = module.isEnabled ? .enabled : .installedDisabled
      } else {
        state = .notInstalled
      }
      lastChecked = Date()
    } catch {
      state = .error(error.localizedDescription)
    }
  }

  /// 開啟「系統設定 > 一般 > 登入項目與延伸功能」，讓使用者啟用延伸功能。
  func openSystemSettings() {
    let candidates = [
      "x-apple.systempreferences:com.apple.LoginItems-Settings.extension",
      "x-apple.systempreferences:com.apple.ExtensionsPreferences",
    ]
    for candidate in candidates {
      if let url = URL(string: candidate), NSWorkspace.shared.open(url) {
        return
      }
    }
  }

  /// 在非主執行緒的隔離區呼叫 FSKit，並轉為可 Sendable 的摘要後回傳。
  nonisolated private static func fetchInstalledModules() async throws -> [ModuleInfo] {
    let identities = try await FSClient.shared.installedExtensions
    return identities.map { ModuleInfo(bundleIdentifier: $0.bundleIdentifier, isEnabled: $0.isEnabled) }
  }
}
