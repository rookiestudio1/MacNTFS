import SwiftUI

/// NTFS 磁碟清單，選單列彈出視窗與主視窗共用。
struct VolumeListView: View {
  @EnvironmentObject private var diskMonitor: DiskMonitor
  @EnvironmentObject private var controller: MountController

  var body: some View {
    if diskMonitor.volumes.isEmpty {
      VStack(spacing: 8) {
        Image(systemName: "externaldrive.badge.questionmark")
          .font(.system(size: 28))
          .foregroundStyle(.secondary)
        Text("目前沒有偵測到 NTFS 磁碟")
          .foregroundStyle(.secondary)
      }
      .frame(maxWidth: .infinity)
      .padding(.vertical, 20)
    } else {
      VStack(spacing: 8) {
        ForEach(diskMonitor.volumes) { volume in
          VolumeRowView(volume: volume, isBusy: controller.busyDevices.contains(volume.bsdName))
        }
      }
    }
  }
}

/// 單一磁碟的列：名稱、狀態標籤、掛載路徑與對應的操作按鈕。
struct VolumeRowView: View {
  @EnvironmentObject private var controller: MountController

  let volume: NTFSVolume
  let isBusy: Bool

  var body: some View {
    VStack(alignment: .leading, spacing: 6) {
      HStack(alignment: .firstTextBaseline) {
        Image(systemName: volume.isMounted ? "externaldrive.fill" : "externaldrive")
          .foregroundStyle(volume.isMounted ? Color.accentColor : Color.secondary)
        Text(volume.displayName)
          .font(.headline)
          .lineLimit(1)
        Spacer()
        StatusBadge(state: volume.mountState)
      }

      HStack(spacing: 8) {
        Text(volume.bsdName)
        Text("·")
        Text(volume.formattedSize)
        if !volume.isWritableMedia {
          Text("·")
          Text("媒體唯讀")
        }
      }
      .font(.caption)
      .foregroundStyle(.secondary)

      if let mountPoint = volume.mountPoint {
        Text(mountPoint.path)
          .font(.caption)
          .foregroundStyle(.secondary)
          .lineLimit(1)
          .truncationMode(.middle)
          .help(mountPoint.path)
      }

      HStack(spacing: 8) {
        if isBusy {
          ProgressView()
            .controlSize(.small)
          Text("處理中…")
            .font(.caption)
            .foregroundStyle(.secondary)
        } else {
          actionButtons
        }
        Spacer()
      }
      .controlSize(.small)

      if !isBusy, volume.isMounted, let blocker = controller.unmountBlockers[volume.bsdName] {
        unmountBlockedPrompt(blocker)
      }
    }
    .padding(10)
    .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 8))
  }

  /// 卸載被其他程式擋下時，說明原因並詢問是否強制卸載。
  private func unmountBlockedPrompt(_ blocker: UnmountBlocker) -> some View {
    VStack(alignment: .leading, spacing: 6) {
      Label(blocker.explanation, systemImage: "exclamationmark.triangle.fill")
        .font(.caption)
        .foregroundStyle(.orange)
        .fixedSize(horizontal: false, vertical: true)
      HStack(spacing: 8) {
        Button("強制卸載", role: .destructive) {
          run { await controller.unmount(volume, force: true) }
        }
        Button("取消") {
          controller.dismissUnmountBlocker(volume)
        }
        Spacer()
      }
      .controlSize(.small)
    }
    .padding(8)
    .background(.orange.opacity(0.1), in: RoundedRectangle(cornerRadius: 6))
  }

  /// 依掛載狀態顯示不同的操作按鈕。
  @ViewBuilder
  private var actionButtons: some View {
    switch volume.mountState {
    case .notMounted:
      Button("掛載") {
        run { await controller.mount(volume, readOnly: false) }
      }
      .disabled(!volume.isWritableMedia)
      .help(volume.isWritableMedia ? "以讀寫模式掛載" : "媒體本身為唯讀，無法以讀寫模式掛載")
      Button("唯讀掛載") {
        run { await controller.mount(volume, readOnly: true) }
      }

    case .mountedByMacNTFS(let readOnly):
      Button("卸載") {
        run { await controller.unmount(volume) }
      }
      if readOnly {
        Button("改為讀寫") {
          run { await controller.remount(volume, readOnly: false) }
        }
        .disabled(!volume.isWritableMedia)
      } else {
        Button("改為唯讀") {
          run { await controller.remount(volume, readOnly: true) }
        }
      }

    case .mountedBySystemReadOnly:
      Button("卸載") {
        run { await controller.unmount(volume) }
      }
      Button("改用 MacNTFS 掛載（需要管理員權限）") {
        run { await controller.takeOverFromSystemDriver(volume, readOnly: false) }
      }

    case .mountedByOther(let kind):
      Button("卸載") {
        run { await controller.unmount(volume) }
      }
      Text("由「\(kind)」驅動掛載")
        .font(.caption)
        .foregroundStyle(.secondary)
    }
  }

  private func run(_ operation: @escaping @MainActor () async -> Void) {
    Task { await operation() }
  }
}

/// 掛載狀態標籤。
struct StatusBadge: View {
  let state: MountState

  var body: some View {
    Text(state.label)
      .font(.caption2.weight(.medium))
      .padding(.horizontal, 6)
      .padding(.vertical, 2)
      .background(color.opacity(0.18), in: Capsule())
      .foregroundStyle(color)
  }

  private var color: Color {
    switch state {
    case .notMounted:
      return .secondary
    case .mountedByMacNTFS(let readOnly):
      return readOnly ? .orange : .green
    case .mountedBySystemReadOnly:
      return .orange
    case .mountedByOther:
      return .purple
    }
  }
}
