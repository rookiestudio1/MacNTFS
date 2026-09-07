import SwiftUI
import AppKit

/// 選單列彈出視窗的內容。
struct MenuBarView: View {
  @EnvironmentObject private var appState: AppState
  @EnvironmentObject private var controller: MountController
  @EnvironmentObject private var extensionStatus: ExtensionStatus
  @Environment(\.openWindow) private var openWindow

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack {
        Image(systemName: "externaldrive.badge.checkmark")
        Text("MacNTFS")
          .font(.headline)
        Spacer()
      }

      ExtensionStatusBanner(compact: true)

      ScrollView {
        VolumeListView()
      }
      .frame(maxHeight: 320)

      if let lastError = controller.lastError {
        Text(lastError)
          .font(.caption)
          .foregroundStyle(.red)
          .lineLimit(3)
          .fixedSize(horizontal: false, vertical: true)
      }

      Divider()

      Toggle("自動掛載新的 NTFS 磁碟", isOn: $controller.autoMountEnabled)
        .toggleStyle(.switch)
        .controlSize(.small)

      Divider()

      HStack {
        Button("重新整理") {
          appState.refreshAll()
        }
        Button("開啟主視窗") {
          openWindow(id: "main")
          NSApp.activate(ignoringOtherApps: true)
        }
        Spacer()
        Button("結束 MacNTFS") {
          NSApp.terminate(nil)
        }
      }
      .controlSize(.small)
    }
    .padding(12)
    .frame(width: 360)
    .onAppear { appState.viewDidAppear() }
    .onDisappear { appState.viewDidDisappear() }
  }
}

/// 延伸功能狀態橫幅；未啟用時顯示提示與「開啟系統設定」按鈕。
struct ExtensionStatusBanner: View {
  @EnvironmentObject private var extensionStatus: ExtensionStatus

  /// 精簡模式（選單列使用）。
  var compact: Bool = false

  var body: some View {
    HStack(spacing: 8) {
      Image(systemName: iconName)
        .foregroundStyle(iconColor)
      VStack(alignment: .leading, spacing: 2) {
        Text(extensionStatus.state.label)
          .font(compact ? .caption : .body)
          .lineLimit(2)
          .fixedSize(horizontal: false, vertical: true)
        if !compact, extensionStatus.state.needsAttention {
          Text("請在「系統設定 > 一般 > 登入項目與延伸功能 > 檔案系統延伸功能」中啟用 MacNTFS。")
            .font(.caption)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        }
      }
      Spacer()
      if extensionStatus.state.needsAttention {
        Button("開啟系統設定") {
          extensionStatus.openSystemSettings()
        }
        .controlSize(.small)
      }
    }
    .padding(compact ? 8 : 10)
    .background(iconColor.opacity(0.12), in: RoundedRectangle(cornerRadius: 8))
  }

  private var iconName: String {
    switch extensionStatus.state {
    case .enabled:
      return "checkmark.circle.fill"
    case .unknown:
      return "questionmark.circle"
    case .notInstalled, .installedDisabled:
      return "exclamationmark.triangle.fill"
    case .error:
      return "xmark.octagon.fill"
    }
  }

  private var iconColor: Color {
    switch extensionStatus.state {
    case .enabled:
      return .green
    case .unknown:
      return .secondary
    case .notInstalled, .installedDisabled:
      return .orange
    case .error:
      return .red
    }
  }
}
