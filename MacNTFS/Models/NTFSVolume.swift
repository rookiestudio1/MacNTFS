import Foundation
import DiskArbitration

/// NTFS 磁碟目前的掛載狀態。
enum MountState: Hashable, Sendable {
  /// 尚未掛載。
  case notMounted
  /// 由 MacNTFS 的 FSKit 延伸功能（ntfs3g）掛載。
  case mountedByMacNTFS(readOnly: Bool)
  /// 由 macOS 內建的唯讀 NTFS 驅動程式掛載。
  case mountedBySystemReadOnly
  /// 由其他驅動程式掛載（例如第三方 NTFS 驅動）。
  case mountedByOther(kind: String)

  /// 顯示在 UI 上的狀態文字。
  var label: String {
    switch self {
    case .notMounted:
      return String(localized: "未掛載")
    case .mountedByMacNTFS(let readOnly):
      return readOnly ? String(localized: "已掛載（唯讀）") : String(localized: "已掛載（讀寫）")
    case .mountedBySystemReadOnly:
      return String(localized: "系統唯讀掛載")
    case .mountedByOther:
      return String(localized: "其他驅動")
    }
  }
}

/// 代表一顆（或一個分割區的）NTFS 磁碟。
///
/// 所有欄位都是從 DiskArbitration 的描述字典（`DADiskCopyDescription`）擷取而來，
/// 為純值型別，可安全地跨執行緒傳遞。
struct NTFSVolume: Identifiable, Hashable, Sendable {
  /// 以 BSD 名稱（例如 `disk4s1`）作為識別碼。
  var id: String { bsdName }

  /// BSD 名稱，例如 `disk4s1`。
  let bsdName: String
  /// 裝置節點路徑，例如 `/dev/disk4s1`。
  let deviceNode: String
  /// 磁碟區名稱（`DAVolumeName`）。
  let volumeName: String?
  /// 媒體名稱（`DAMediaName`）。
  let mediaName: String?
  /// 媒體容量（位元組）。
  let mediaSize: Int64?
  /// 媒體內容型別（分割區型別識別碼，`DAMediaContent`）。
  let mediaContent: String?
  /// 媒體本身是否可寫入。
  let isWritableMedia: Bool
  /// 是否為可移除媒體。
  let isRemovable: Bool
  /// 是否可退出。
  let isEjectable: Bool
  /// 掛載路徑（`DAVolumePath`），未掛載時為 nil。
  let mountPoint: URL?
  /// 檔案系統種類（`DAVolumeKind`），例如 `ntfs`、`ntfs3g`。
  let volumeKind: String?
  /// 掛載點是否以唯讀方式掛載（透過 `statfs` 判斷）。
  let isReadOnlyMount: Bool

  /// 是否已掛載。
  var isMounted: Bool { mountPoint != nil }

  /// 依據掛載狀態與驅動程式種類推導出的狀態。
  var mountState: MountState {
    guard isMounted else { return .notMounted }
    switch volumeKind?.lowercased() {
    case Self.macNTFSVolumeKind:
      return .mountedByMacNTFS(readOnly: isReadOnlyMount)
    case Self.systemNTFSVolumeKind:
      return .mountedBySystemReadOnly
    case let kind?:
      return .mountedByOther(kind: kind)
    case nil:
      return .mountedByOther(kind: String(localized: "未知"))
    }
  }

  /// 顯示名稱：優先使用磁碟區名稱，其次是媒體名稱，最後退回 BSD 名稱。
  var displayName: String {
    if let volumeName, !volumeName.isEmpty { return volumeName }
    if let mediaName, !mediaName.isEmpty { return mediaName }
    return bsdName
  }

  /// 格式化後的容量字串，例如「512 GB」。
  var formattedSize: String {
    guard let mediaSize else { return String(localized: "容量未知") }
    return ByteCountFormatter.string(fromByteCount: mediaSize, countStyle: .file)
  }

  // MARK: - 常數

  /// MacNTFS FSKit 延伸功能的 `FSShortName`，掛載後會成為 `DAVolumeKind`。
  static let macNTFSVolumeKind = "ntfs3g"
  /// macOS 內建唯讀 NTFS 驅動程式的 `DAVolumeKind`。
  static let systemNTFSVolumeKind = "ntfs"
  /// 會被視為 NTFS 的分割區型別（MBR 的 Windows_NTFS 與 GPT 的 Microsoft Basic Data）。
  static let ntfsMediaContents: Set<String> = [
    "Windows_NTFS",
    "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7",
  ]

  // MARK: - 從 DiskArbitration 建立

  /// 從 `DADisk` 建立；若該磁碟不是 NTFS 或不是葉節點媒體，回傳 nil。
  init?(disk: DADisk) {
    guard let description = DADiskCopyDescription(disk) as? [String: Any] else { return nil }
    self.init(description: description)
  }

  /// 從 DiskArbitration 描述字典建立；若該磁碟不符合 NTFS 條件，回傳 nil。
  init?(description: [String: Any]) {
    // 只考慮葉節點媒體（實際的分割區或無分割表的整顆磁碟）。
    guard description[kDADiskDescriptionMediaLeafKey as String] as? Bool == true else { return nil }
    guard let bsdName = description[kDADiskDescriptionMediaBSDNameKey as String] as? String,
          !bsdName.isEmpty else { return nil }

    let mediaContent = description[kDADiskDescriptionMediaContentKey as String] as? String
    let volumeKind = (description[kDADiskDescriptionVolumeKindKey as String] as? String)?.lowercased()

    // 判斷是否為 NTFS：
    // 1. 已探測到檔案系統種類時，以種類為準（避免把 exFAT / FAT32 誤判為 NTFS，
    //    因為它們也常使用 Windows_NTFS 或 Microsoft Basic Data 分割區型別）。
    // 2. 尚未探測到種類時，退回以分割區型別判斷。
    let isNTFS: Bool
    if let volumeKind {
      isNTFS = volumeKind == Self.macNTFSVolumeKind || volumeKind == Self.systemNTFSVolumeKind
    } else if let mediaContent {
      isNTFS = Self.ntfsMediaContents.contains(mediaContent)
    } else {
      isNTFS = false
    }
    guard isNTFS else { return nil }

    let mountPoint = description[kDADiskDescriptionVolumePathKey as String] as? URL

    self.bsdName = bsdName
    self.deviceNode = "/dev/" + bsdName
    self.volumeName = description[kDADiskDescriptionVolumeNameKey as String] as? String
    self.mediaName = description[kDADiskDescriptionMediaNameKey as String] as? String
    self.mediaSize = (description[kDADiskDescriptionMediaSizeKey as String] as? NSNumber)?.int64Value
    self.mediaContent = mediaContent
    self.isWritableMedia = description[kDADiskDescriptionMediaWritableKey as String] as? Bool ?? false
    self.isRemovable = description[kDADiskDescriptionMediaRemovableKey as String] as? Bool ?? false
    self.isEjectable = description[kDADiskDescriptionMediaEjectableKey as String] as? Bool ?? false
    self.mountPoint = mountPoint
    self.volumeKind = volumeKind
    self.isReadOnlyMount = mountPoint.map(Self.isReadOnlyMountPoint) ?? false
  }

  /// 以 `statfs` 檢查掛載點是否為唯讀（`MNT_RDONLY`）。
  static func isReadOnlyMountPoint(_ url: URL) -> Bool {
    var stat = statfs()
    let result = url.withUnsafeFileSystemRepresentation { path -> Int32 in
      guard let path else { return -1 }
      return statfs(path, &stat)
    }
    guard result == 0 else { return false }
    return (stat.f_flags & UInt32(MNT_RDONLY)) != 0
  }
}
