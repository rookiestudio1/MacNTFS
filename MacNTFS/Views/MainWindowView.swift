import SwiftUI

/// 主視窗內容：延伸功能狀態、磁碟清單與操作紀錄。
struct MainWindowView: View {
  @EnvironmentObject private var appState: AppState
  @EnvironmentObject private var controller: MountController
  @EnvironmentObject private var extensionStatus: ExtensionStatus

  @State private var isLogExpanded = true

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Text("NTFS 磁碟")
          .font(.title2.weight(.semibold))
        Spacer()
        Toggle("自動掛載新的 NTFS 磁碟", isOn: $controller.autoMountEnabled)
          .toggleStyle(.switch)
          .controlSize(.small)
        Button("重新整理") {
          appState.refreshAll()
        }
      }

      ExtensionStatusBanner()

      ScrollView {
        VolumeListView()
          .frame(maxWidth: .infinity, alignment: .leading)
      }
      .frame(minHeight: 160)

      if let lastError = controller.lastError {
        Text(lastError)
          .font(.caption)
          .foregroundStyle(.red)
          .fixedSize(horizontal: false, vertical: true)
      }

      DisclosureGroup(isExpanded: $isLogExpanded) {
        LogView(entries: controller.log)
          .frame(height: 180)
        HStack {
          Spacer()
          Button("清除紀錄") {
            controller.clearLog()
          }
          .controlSize(.small)
        }
      } label: {
        Text("操作紀錄（\(controller.log.count)）")
          .font(.headline)
      }
    }
    .padding(16)
    .frame(minWidth: 520, minHeight: 440)
    .onAppear { appState.viewDidAppear() }
    .onDisappear { appState.viewDidDisappear() }
  }
}

/// 以等寬字型顯示的操作紀錄，並自動捲動到最新一筆。
struct LogView: View {
  let entries: [LogEntry]

  var body: some View {
    ScrollViewReader { proxy in
      ScrollView {
        LazyVStack(alignment: .leading, spacing: 2) {
          ForEach(entries) { entry in
            HStack(alignment: .top, spacing: 8) {
              Text(entry.date.formatted(date: .omitted, time: .standard))
                .foregroundStyle(.secondary)
              Text(entry.message)
                .foregroundStyle(entry.isError ? Color.red : Color.primary)
                .textSelection(.enabled)
            }
            .font(.system(.caption, design: .monospaced))
            .id(entry.id)
          }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(8)
      }
      .background(.quaternary.opacity(0.3), in: RoundedRectangle(cornerRadius: 6))
      .onChange(of: entries.count) { _, _ in
        if let last = entries.last {
          proxy.scrollTo(last.id, anchor: .bottom)
        }
      }
    }
  }
}
