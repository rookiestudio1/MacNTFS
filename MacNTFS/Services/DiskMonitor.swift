import Foundation
import Combine
import DiskArbitration

/// 透過 DiskArbitration 監看系統中的 NTFS 磁碟。
///
/// DiskArbitration 的回呼會在私有序列佇列上執行，這裡會先把 `DADisk` 轉成純值的
/// `NTFSVolume`，再切換到主執行緒更新 `volumes`，以符合 Swift 6 的並行安全需求。
@MainActor
final class DiskMonitor: ObservableObject {
  /// 目前偵測到的 NTFS 磁碟，依 BSD 名稱排序。
  @Published private(set) var volumes: [NTFSVolume] = []

  /// 有新的 NTFS 磁碟出現時呼叫（供控制器做自動掛載）。
  var onVolumeAppeared: ((NTFSVolume) -> Void)?
  /// NTFS 磁碟移除時呼叫，參數為 BSD 名稱。
  var onVolumeDisappeared: ((String) -> Void)?

  private var session: DASession?
  /// DiskArbitration 回呼專用的序列佇列。
  private let queue = DispatchQueue(label: "com.rookiestudio.MacNTFS.DiskMonitor")
  /// 是否已註冊回呼。
  private var isRunning = false

  init() {}

  /// 開始監看磁碟。DiskArbitration 會對已連接的磁碟重播「出現」事件，
  /// 因此不需要另外列舉現有磁碟。
  func start() {
    guard !isRunning else { return }
    guard let session = DASessionCreate(kCFAllocatorDefault) else {
      return
    }
    self.session = session
    isRunning = true

    // 只比對葉節點媒體，其餘條件（是否為 NTFS）在程式碼中判斷。
    let match: [String: Any] = [kDADiskDescriptionMediaLeafKey as String: true]
    let matching = match as CFDictionary

    // 要監看的描述欄位：掛載路徑、磁碟區名稱、檔案系統種類。
    let watchedKeys: [String] = [
      kDADiskDescriptionVolumePathKey as String,
      kDADiskDescriptionVolumeNameKey as String,
      kDADiskDescriptionVolumeKindKey as String,
    ]
    let watched = watchedKeys as CFArray

    // 以未持有的指標作為 context，回呼中再轉回 DiskMonitor。
    // DiskMonitor 的生命週期由 AppState 持有，會比 session 更長。
    let context = Unmanaged.passUnretained(self).toOpaque()

    DARegisterDiskAppearedCallback(session, matching, diskAppearedCallback, context)
    DARegisterDiskDisappearedCallback(session, matching, diskDisappearedCallback, context)
    DARegisterDiskDescriptionChangedCallback(session, matching, watched, diskDescriptionChangedCallback, context)
    DASessionSetDispatchQueue(session, queue)
  }

  /// 停止監看並釋放 session。
  func stop() {
    guard isRunning, let session else { return }
    let context = Unmanaged.passUnretained(self).toOpaque()
    DAUnregisterCallback(session, unsafeBitCast(diskAppearedCallback as DADiskAppearedCallback, to: UnsafeMutableRawPointer.self), context)
    DAUnregisterCallback(session, unsafeBitCast(diskDisappearedCallback as DADiskDisappearedCallback, to: UnsafeMutableRawPointer.self), context)
    DAUnregisterCallback(session, unsafeBitCast(diskDescriptionChangedCallback as DADiskDescriptionChangedCallback, to: UnsafeMutableRawPointer.self), context)
    DASessionSetDispatchQueue(session, nil)
    self.session = nil
    isRunning = false
  }

  /// 重新讀取所有已知磁碟的描述，更新掛載狀態等資訊。
  func refresh() {
    guard let session else { return }
    var updated: [NTFSVolume] = []
    for volume in volumes {
      guard let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, volume.bsdName) else {
        // 無法建立 DADisk，代表磁碟可能已經移除。
        continue
      }
      // 描述可能為 nil（磁碟剛被移除），此時視同已消失。
      if let refreshed = NTFSVolume(disk: disk) {
        updated.append(refreshed)
      }
    }
    // 重新整理時才發現已消失的磁碟，同樣要通知出去；否則之後 DiskArbitration 的
    // 消失回呼找不到它，掛載控制器的重試與忙碌狀態就不會清除。
    let remaining = Set(updated.map(\.bsdName))
    let removed = volumes.map(\.bsdName).filter { !remaining.contains($0) }
    replaceVolumes(with: updated)
    for bsdName in removed {
      onVolumeDisappeared?(bsdName)
    }
  }

  /// 取得指定 BSD 名稱的最新磁碟資訊（會即時向 DiskArbitration 重新查詢）。
  func currentVolume(bsdName: String) -> NTFSVolume? {
    guard let session,
          let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsdName) else {
      return volumes.first { $0.bsdName == bsdName }
    }
    return NTFSVolume(disk: disk) ?? volumes.first { $0.bsdName == bsdName }
  }

  // MARK: - 由回呼轉交的事件（主執行緒）

  fileprivate func handleAppeared(_ volume: NTFSVolume) {
    let isNew = !volumes.contains { $0.bsdName == volume.bsdName }
    upsert(volume)
    if isNew {
      onVolumeAppeared?(volume)
    }
  }

  fileprivate func handleDisappeared(bsdName: String) {
    let before = volumes.count
    volumes.removeAll { $0.bsdName == bsdName }
    if volumes.count != before {
      onVolumeDisappeared?(bsdName)
    }
  }

  fileprivate func handleDescriptionChanged(bsdName: String, volume: NTFSVolume?) {
    if let volume {
      let isNew = !volumes.contains { $0.bsdName == volume.bsdName }
      upsert(volume)
      // 磁碟原本因尚未探測而被排除，探測完成後才確定是 NTFS，視同新出現。
      if isNew {
        onVolumeAppeared?(volume)
      }
    } else {
      // 描述改變後不再符合 NTFS 條件（例如重新格式化），從清單移除。
      handleDisappeared(bsdName: bsdName)
    }
  }

  private func upsert(_ volume: NTFSVolume) {
    var list = volumes
    if let index = list.firstIndex(where: { $0.bsdName == volume.bsdName }) {
      list[index] = volume
    } else {
      list.append(volume)
    }
    replaceVolumes(with: list)
  }

  private func replaceVolumes(with list: [NTFSVolume]) {
    let sorted = list.sorted { $0.bsdName.localizedStandardCompare($1.bsdName) == .orderedAscending }
    if sorted != volumes {
      volumes = sorted
    }
  }
}

// MARK: - C 回呼（在 DiskArbitration 佇列上執行）

/// 從 context 指標取回 DiskMonitor。
private func monitor(from context: UnsafeMutableRawPointer?) -> DiskMonitor? {
  guard let context else { return nil }
  return Unmanaged<DiskMonitor>.fromOpaque(context).takeUnretainedValue()
}

/// 取得 DADisk 的 BSD 名稱。
private func bsdName(of disk: DADisk) -> String? {
  guard let name = DADiskGetBSDName(disk) else { return nil }
  return String(cString: name)
}

private func diskAppearedCallback(_ disk: DADisk, _ context: UnsafeMutableRawPointer?) {
  guard let monitor = monitor(from: context) else { return }
  // 在回呼執行緒上完成描述擷取，再以純值切換到主執行緒。
  guard let volume = NTFSVolume(disk: disk) else { return }
  Task { @MainActor in
    monitor.handleAppeared(volume)
  }
}

private func diskDisappearedCallback(_ disk: DADisk, _ context: UnsafeMutableRawPointer?) {
  guard let monitor = monitor(from: context) else { return }
  guard let name = bsdName(of: disk) else { return }
  Task { @MainActor in
    monitor.handleDisappeared(bsdName: name)
  }
}

private func diskDescriptionChangedCallback(_ disk: DADisk, _ keys: CFArray, _ context: UnsafeMutableRawPointer?) {
  guard let monitor = monitor(from: context) else { return }
  guard let name = bsdName(of: disk) else { return }
  let volume = NTFSVolume(disk: disk)
  Task { @MainActor in
    monitor.handleDescriptionChanged(bsdName: name, volume: volume)
  }
}
