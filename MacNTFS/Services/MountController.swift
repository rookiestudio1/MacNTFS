import Foundation
import Combine
import AppKit

/// 掛載／卸載操作可能發生的錯誤。
enum MountError: LocalizedError {
  /// 外部指令執行失敗。
  case commandFailed(exitCode: Int32, stderr: String)
  /// 管理員權限的指令執行失敗（使用者取消或指令錯誤）。
  case privilegedCommandFailed(String)
  /// 磁碟目前的狀態不允許此操作。
  case invalidState(String)

  var errorDescription: String? {
    switch self {
    case .commandFailed(let exitCode, let stderr):
      let trimmed = stderr.trimmingCharacters(in: .whitespacesAndNewlines)
      return trimmed.isEmpty ? "指令執行失敗（結束代碼 \(exitCode)）" : "指令執行失敗（結束代碼 \(exitCode)）：\(trimmed)"
    case .privilegedCommandFailed(let message):
      return "需要管理員權限的指令執行失敗：\(message)"
    case .invalidState(let message):
      return message
    }
  }
}

/// 卸載時擋住的程式（diskutil 回報的 dissenter）。
struct UnmountBlocker: Equatable, Sendable {
  /// 程式的 PID；diskutil 沒有提供時為 nil。
  let pid: Int32?
  /// 程式的完整路徑；diskutil 沒有提供時為 nil。
  let path: String?

  /// 顯示用的程式名稱。
  var name: String? {
    path.map { ($0 as NSString).lastPathComponent }
  }

  /// 是否為 macOS 的 SMB 檔案分享伺服器。
  var isSMBServer: Bool {
    name == "smbd"
  }

  /// 給使用者看的說明，以及強制卸載的後果。
  var explanation: String {
    if isSMBServer {
      return "有其他裝置正透過 SMB 檔案分享使用這顆磁碟（smbd）。強制卸載會中斷這些連線；磁碟上的資料會先寫回，不會損壞。"
    }
    if let name {
      let pidText = pid.map { "（PID \($0)）" } ?? ""
      return "「\(name)」\(pidText)正在使用這顆磁碟。強制卸載可能讓它遺失尚未儲存的資料。"
    }
    return "有程式正在使用這顆磁碟。強制卸載可能讓它遺失尚未儲存的資料。"
  }
}

/// 一筆操作紀錄。
struct LogEntry: Identifiable, Hashable, Sendable {
  let id = UUID()
  let date: Date
  let message: String
  let isError: Bool

  init(message: String, isError: Bool = false, date: Date = Date()) {
    self.date = date
    self.message = message
    self.isError = isError
  }
}

/// 負責執行掛載、卸載等操作，並處理自動掛載邏輯。
///
/// 一般操作透過 `/usr/sbin/diskutil` 執行，會經由 diskarbitrationd 處理，
/// 主控台使用者不需要 root 權限。
@MainActor
final class MountController: ObservableObject {
  /// 正在進行操作的裝置（BSD 名稱），UI 會顯示進度指示。
  @Published private(set) var busyDevices: Set<String> = []
  /// 最後一次發生的錯誤訊息。
  @Published var lastError: String?
  /// 卸載被其他程式擋下的裝置（BSD 名稱 → 擋住的程式），UI 會提供強制卸載。
  @Published private(set) var unmountBlockers: [String: UnmountBlocker] = [:]
  /// 操作紀錄，最多保留 200 筆。
  @Published private(set) var log: [LogEntry] = []
  /// 是否自動掛載新的 NTFS 磁碟（儲存在 UserDefaults）。
  @Published var autoMountEnabled: Bool {
    didSet {
      UserDefaults.standard.set(autoMountEnabled, forKey: Self.autoMountDefaultsKey)
    }
  }

  /// 用來在自動掛載前重新查詢磁碟的最新狀態。
  weak var diskMonitor: DiskMonitor?

  /// 已嘗試過自動掛載的裝置，避免無限重試；裝置移除後會清除。
  private var attemptedDevices: Set<String> = []

  private static let autoMountDefaultsKey = "autoMountEnabled"
  private static let maxLogEntries = 200
  private static let diskutilPath = "/usr/sbin/diskutil"
  /// 自動掛載前的緩衝時間，讓 diskarbitrationd 有機會先完成自己的自動掛載，避免互相搶先。
  private static let autoMountGraceDelay: Duration = .seconds(2)

  init() {
    UserDefaults.standard.register(defaults: [Self.autoMountDefaultsKey: true])
    autoMountEnabled = UserDefaults.standard.bool(forKey: Self.autoMountDefaultsKey)
  }

  // MARK: - 掛載操作

  /// 以 `diskutil mount` 掛載磁碟。
  func mount(_ volume: NTFSVolume, readOnly: Bool) async {
    await perform(on: volume, description: readOnly ? "唯讀掛載" : "掛載") {
      var args = ["mount"]
      if readOnly { args.append("readOnly") }
      args.append(volume.deviceNode)
      let output = try await Self.runProcess(Self.diskutilPath, args)
      return output
    }
  }

  /// 以 `diskutil unmount` 卸載磁碟。
  ///
  /// 一般卸載若被其他程式擋下（例如 SMB 分享中的 smbd），會記錄在 `unmountBlockers`，
  /// 由 UI 詢問使用者是否改用強制卸載。
  func unmount(_ volume: NTFSVolume, force: Bool = false) async {
    unmountBlockers[volume.bsdName] = nil
    await perform(on: volume, description: force ? "強制卸載" : "卸載") {
      var args = ["unmount"]
      if force { args.append("force") }
      args.append(volume.deviceNode)
      do {
        return try await Self.runProcess(Self.diskutilPath, args)
      } catch MountError.commandFailed(let exitCode, let output) where !force {
        if let blocker = Self.parseUnmountBlocker(output) {
          self.unmountBlockers[volume.bsdName] = blocker
        }
        throw MountError.commandFailed(exitCode: exitCode, stderr: output)
      }
    }
  }

  /// 使用者決定不強制卸載，收起提示。
  func dismissUnmountBlocker(_ volume: NTFSVolume) {
    unmountBlockers[volume.bsdName] = nil
  }

  /// 從 `diskutil unmount` 的輸出找出擋住卸載的程式。
  ///
  /// diskutil 的格式為
  /// `Volume X on disk7s2 failed to unmount: dissented by PID 4601 (/usr/sbin/smbd)`；
  /// 沒有 PID 時只會說被 dissent 或 busy。不是「被佔用」造成的失敗回傳 nil。
  nonisolated static func parseUnmountBlocker(_ output: String) -> UnmountBlocker? {
    let pattern = #"dissented by PID (\d+) \(([^)]*)\)"#
    if let regex = try? NSRegularExpression(pattern: pattern),
       let match = regex.firstMatch(in: output, range: NSRange(output.startIndex..., in: output)),
       let pidRange = Range(match.range(at: 1), in: output),
       let pathRange = Range(match.range(at: 2), in: output) {
      let path = String(output[pathRange])
      return UnmountBlocker(pid: Int32(output[pidRange]), path: path.isEmpty ? nil : path)
    }
    let lowered = output.lowercased()
    if lowered.contains("dissented") || lowered.contains("resource busy") {
      return UnmountBlocker(pid: nil, path: nil)
    }
    return nil
  }

  /// 先卸載再以指定模式重新掛載。
  func remount(_ volume: NTFSVolume, readOnly: Bool) async {
    await perform(on: volume, description: readOnly ? "改為唯讀掛載" : "改為讀寫掛載") {
      _ = try await Self.runProcess(Self.diskutilPath, ["unmount", volume.deviceNode])
      var args = ["mount"]
      if readOnly { args.append("readOnly") }
      args.append(volume.deviceNode)
      return try await Self.runProcess(Self.diskutilPath, args)
    }
  }

  /// 將由系統內建唯讀驅動掛載的磁碟，改由 MacNTFS 的 FSKit 延伸功能掛載。
  ///
  /// 這是備援路徑：先以 `diskutil unmount` 卸載，再透過 AppleScript 的
  /// `do shell script ... with administrator privileges` 直接執行 `/sbin/mount -F -t ntfs3g`。
  /// 直接呼叫 `mount` 需要 root 權限，因此會跳出系統的管理員密碼對話框。
  func takeOverFromSystemDriver(_ volume: NTFSVolume, readOnly: Bool) async {
    await perform(on: volume, description: "改用 MacNTFS 掛載") {
      guard case .mountedBySystemReadOnly = volume.mountState else {
        throw MountError.invalidState("「\(volume.displayName)」目前不是由系統唯讀驅動掛載，無法接手。")
      }

      _ = try await Self.runProcess(Self.diskutilPath, ["unmount", volume.deviceNode])

      let mountPoint = Self.availableMountPoint(for: volume)
      let mountOptions = readOnly ? " -o rdonly" : ""
      let shellCommand = "mkdir -p \(Self.shellQuote(mountPoint)) && /sbin/mount -F -t ntfs3g\(mountOptions) \(Self.shellQuote(volume.deviceNode)) \(Self.shellQuote(mountPoint))"
      let output = try await Self.runPrivilegedShell(shellCommand)
      return output.isEmpty ? "已掛載於 \(mountPoint)" : output
    }
  }

  // MARK: - 自動掛載

  /// 新磁碟出現時的處理：視設定自動掛載，或在系統已唯讀掛載時提示使用者。
  func handleAppeared(_ volume: NTFSVolume) {
    appendLog("偵測到 NTFS 磁碟「\(volume.displayName)」（\(volume.bsdName)），狀態：\(volume.mountState.label)")

    switch volume.mountState {
    case .notMounted:
      guard autoMountEnabled else { return }
      guard !busyDevices.contains(volume.bsdName), !attemptedDevices.contains(volume.bsdName) else { return }
      attemptedDevices.insert(volume.bsdName)
      Task { [weak self] in
        guard let self else { return }
        // 等待一小段時間，讓 diskarbitrationd 先完成它自己的自動掛載流程。
        try? await Task.sleep(for: Self.autoMountGraceDelay)
        let latest = self.diskMonitor?.currentVolume(bsdName: volume.bsdName) ?? volume
        switch latest.mountState {
        case .notMounted:
          self.appendLog("自動掛載「\(latest.displayName)」…")
          await self.mount(latest, readOnly: false)
        case .mountedBySystemReadOnly:
          self.appendLog("「\(latest.displayName)」已由系統內建唯讀驅動掛載，可在清單中選擇「改用 MacNTFS 掛載」。")
        default:
          self.appendLog("「\(latest.displayName)」已由其他方式掛載，略過自動掛載。")
        }
      }
    case .mountedBySystemReadOnly:
      appendLog("「\(volume.displayName)」已由系統內建唯讀驅動掛載，可在清單中選擇「改用 MacNTFS 掛載」以取得寫入能力。")
    case .mountedByMacNTFS, .mountedByOther:
      break
    }
  }

  /// 磁碟移除時清除重試狀態，下次再插入時可再度自動掛載。
  func handleDisappeared(_ bsdName: String) {
    attemptedDevices.remove(bsdName)
    busyDevices.remove(bsdName)
    unmountBlockers.removeValue(forKey: bsdName)
  }

  /// 清除操作紀錄。
  func clearLog() {
    log.removeAll()
  }

  // MARK: - 私有輔助

  /// 統一處理忙碌狀態、紀錄與錯誤。
  private func perform(
    on volume: NTFSVolume,
    description: String,
    _ operation: () async throws -> String
  ) async {
    guard !busyDevices.contains(volume.bsdName) else {
      appendLog("「\(volume.displayName)」正在處理其他操作，略過「\(description)」。", isError: true)
      return
    }
    busyDevices.insert(volume.bsdName)
    defer { busyDevices.remove(volume.bsdName) }

    appendLog("\(description)「\(volume.displayName)」（\(volume.deviceNode)）…")
    do {
      let output = try await operation()
      let trimmed = output.trimmingCharacters(in: .whitespacesAndNewlines)
      appendLog(trimmed.isEmpty ? "\(description)完成。" : "\(description)完成：\(trimmed)")
      lastError = nil
      // 操作完成後立即重新讀取狀態，不必等待 DiskArbitration 通知。
      diskMonitor?.refresh()
    } catch {
      let message = "\(description)「\(volume.displayName)」失敗：\(error.localizedDescription)"
      appendLog(message, isError: true)
      lastError = message
      diskMonitor?.refresh()
    }
  }

  private func appendLog(_ message: String, isError: Bool = false) {
    log.append(LogEntry(message: message, isError: isError))
    if log.count > Self.maxLogEntries {
      log.removeFirst(log.count - Self.maxLogEntries)
    }
  }

  /// 執行外部指令並回傳標準輸出；結束代碼非 0 時擲出 `MountError.commandFailed`。
  nonisolated private static func runProcess(_ path: String, _ args: [String]) async throws -> String {
    let result = try await ProcessRunner.run(path, args)
    guard result.exitCode == 0 else {
      let stderr = result.stderr.isEmpty ? result.stdout : result.stderr
      throw MountError.commandFailed(exitCode: result.exitCode, stderr: stderr)
    }
    return result.stdout
  }

  /// 透過 AppleScript 以管理員權限執行 shell 指令，回傳輸出內容。
  nonisolated private static func runPrivilegedShell(_ command: String) async throws -> String {
    // NSAppleScript 會阻塞直到使用者輸入密碼並執行完畢，因此在背景執行緒進行。
    let source = "do shell script \"\(appleScriptEscape(command))\" with administrator privileges"
    return try await Task.detached(priority: .userInitiated) {
      guard let script = NSAppleScript(source: source) else {
        throw MountError.privilegedCommandFailed("無法建立 AppleScript。")
      }
      var errorInfo: NSDictionary?
      let descriptor = script.executeAndReturnError(&errorInfo)
      if let errorInfo {
        let message = (errorInfo[NSAppleScript.errorMessage] as? String)
          ?? (errorInfo[NSAppleScript.errorBriefMessage] as? String)
          ?? "未知錯誤"
        throw MountError.privilegedCommandFailed(message)
      }
      return descriptor.stringValue ?? ""
    }.value
  }

  /// 將字串轉為 shell 的單引號字面值。
  nonisolated private static func shellQuote(_ value: String) -> String {
    "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
  }

  /// 將字串轉義為 AppleScript 字串字面值的內容。
  nonisolated private static func appleScriptEscape(_ value: String) -> String {
    value
      .replacingOccurrences(of: "\\", with: "\\\\")
      .replacingOccurrences(of: "\"", with: "\\\"")
  }

  /// 掛載點名稱最多嘗試的流水號，避免在異常情況下無限迴圈。
  nonisolated static let maximumMountPointSuffix = 999

  /// 為磁碟計算可用的掛載點：清理名稱中的 `/` 與 `:`，
  /// 若該路徑已被占用則依序加上「 1」「 2」…。
  ///
  /// 只有這條備援路徑需要自己算掛載點；走 `diskutil` 時是由 diskarbitrationd
  /// 決定掛載點並自行去重，不會經過這裡。
  ///
  /// 內部可見（非 private）以便單獨驗證這段邏輯。
  nonisolated static func availableMountPoint(for volume: NTFSVolume) -> String {
    availableMountPoint(name: volume.volumeName ?? "")
  }

  nonisolated static func availableMountPoint(name rawName: String) -> String {
    var name = rawName
      .replacingOccurrences(of: "/", with: "-")
      .replacingOccurrences(of: ":", with: "-")
      .trimmingCharacters(in: .whitespacesAndNewlines)
    if name.isEmpty || name == "." || name == ".." {
      name = "NTFS Volume"
    }

    // 一次取得目前所有掛載點，迴圈中重複比對即可。
    let mounted = mountedPaths()
    let fileManager = FileManager.default
    var candidate = "/Volumes/\(name)"
    var suffix = 1
    while isPathOccupied(candidate, mountedPaths: mounted, fileManager: fileManager) {
      guard suffix <= maximumMountPointSuffix else {
        // 極端情況：加上 UUID 確保不會撞名。
        return "/Volumes/\(name) \(UUID().uuidString.prefix(8))"
      }
      candidate = "/Volumes/\(name) \(suffix)"
      suffix += 1
    }
    return candidate
  }

  /// 路徑是否已被占用，不可作為新的掛載點。
  ///
  /// 判斷順序很重要：**先看它是不是活的掛載點**。只看「資料夾是否為空」並不可靠，
  /// 因為一個已掛載但內容為空的卷（例如剛格式化又以唯讀掛載的磁碟，macOS 無法在上面
  /// 建立 `.fseventsd`）看起來就是空資料夾，若據此判定可用，`mount` 會蓋在活的掛載點上，
  /// 把原本那顆磁碟藏起來。
  nonisolated static func isPathOccupied(_ path: String,
                                         mountedPaths: Set<String>,
                                         fileManager: FileManager) -> Bool {
    // 1. 已經是掛載點：無論內容為何都不可使用。
    if mountedPaths.contains(path) { return true }
    // 2. 不存在：可以使用（稍後由 mkdir 建立）。
    var isDirectory: ObjCBool = false
    guard fileManager.fileExists(atPath: path, isDirectory: &isDirectory) else { return false }
    // 3. 存在但不是資料夾（例如同名檔案）：不可使用。
    guard isDirectory.boolValue else { return true }
    // 4. 存在且是資料夾：空的可以重複使用（macOS 卸載後常留下空資料夾），非空則不行。
    let contents = (try? fileManager.contentsOfDirectory(atPath: path)) ?? []
    return !contents.isEmpty
  }

  /// 目前系統上所有的掛載點路徑。
  ///
  /// 使用 `getmntinfo_r_np` 而非 `getmntinfo`，後者回傳共用的靜態緩衝區，
  /// 在非主執行緒呼叫並不安全。
  nonisolated static func mountedPaths() -> Set<String> {
    var buffer: UnsafeMutablePointer<statfs>?
    let count = getmntinfo_r_np(&buffer, MNT_NOWAIT)
    guard count > 0, let buffer else { return [] }
    defer { free(buffer) }

    var paths = Set<String>()
    for index in 0..<Int(count) {
      var entry = buffer[index]
      let path = withUnsafePointer(to: &entry.f_mntonname) { pointer in
        pointer.withMemoryRebound(to: CChar.self, capacity: Int(MAXPATHLEN)) {
          String(cString: $0)
        }
      }
      paths.insert(path)
    }
    return paths
  }
}

// MARK: - 外部指令執行

/// 以 `Process` 執行外部指令，透過管線收集輸出。
enum ProcessRunner {
  struct Result: Sendable {
    let exitCode: Int32
    let stdout: String
    let stderr: String
  }

  /// 執行緒安全的資料緩衝區，用來在 readabilityHandler 中累積輸出。
  private final class DataBuffer: @unchecked Sendable {
    private let lock = NSLock()
    private var data = Data()

    func append(_ chunk: Data) {
      lock.lock()
      defer { lock.unlock() }
      data.append(chunk)
    }

    var string: String {
      lock.lock()
      defer { lock.unlock() }
      return String(decoding: data, as: UTF8.self)
    }
  }

  /// 執行指令並等待結束。啟動失敗時擲出錯誤。
  static func run(_ path: String, _ arguments: [String]) async throws -> Result {
    try await withCheckedThrowingContinuation { continuation in
      let process = Process()
      process.executableURL = URL(fileURLWithPath: path)
      process.arguments = arguments

      let stdoutPipe = Pipe()
      let stderrPipe = Pipe()
      process.standardOutput = stdoutPipe
      process.standardError = stderrPipe
      process.standardInput = FileHandle.nullDevice

      let stdoutBuffer = DataBuffer()
      let stderrBuffer = DataBuffer()
      // 兩條管線各自讀到 EOF 後才算完成，避免遺漏程序結束前最後一段輸出。
      let group = DispatchGroup()
      group.enter()
      group.enter()

      stdoutPipe.fileHandleForReading.readabilityHandler = { handle in
        let chunk = handle.availableData
        if chunk.isEmpty {
          handle.readabilityHandler = nil
          group.leave()
        } else {
          stdoutBuffer.append(chunk)
        }
      }
      stderrPipe.fileHandleForReading.readabilityHandler = { handle in
        let chunk = handle.availableData
        if chunk.isEmpty {
          handle.readabilityHandler = nil
          group.leave()
        } else {
          stderrBuffer.append(chunk)
        }
      }

      process.terminationHandler = { finished in
        let exitCode = finished.terminationStatus
        group.notify(queue: .global()) {
          // Pipe 必須活到兩條管線都讀到 EOF。Process 結束後會被釋放，連帶釋放它持有的
          // Pipe，讀取端的 fd 若在 EOF 事件送達前就被關閉，readabilityHandler 永遠等不到
          // EOF，continuation 不會 resume，呼叫端（例如自動掛載）就一直停在「處理中」。
          // 多個 diskutil 並行時特別容易發生。
          withExtendedLifetime((stdoutPipe, stderrPipe)) {}
          continuation.resume(returning: Result(
            exitCode: exitCode,
            stdout: stdoutBuffer.string,
            stderr: stderrBuffer.string
          ))
        }
      }

      do {
        try process.run()
      } catch {
        stdoutPipe.fileHandleForReading.readabilityHandler = nil
        stderrPipe.fileHandleForReading.readabilityHandler = nil
        continuation.resume(throwing: error)
      }
    }
  }
}
