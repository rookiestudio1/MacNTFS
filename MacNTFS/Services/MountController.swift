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
  func unmount(_ volume: NTFSVolume, force: Bool = false) async {
    await perform(on: volume, description: force ? "強制卸載" : "卸載") {
      var args = ["unmount"]
      if force { args.append("force") }
      args.append(volume.deviceNode)
      return try await Self.runProcess(Self.diskutilPath, args)
    }
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

  /// 為磁碟計算可用的掛載點：清理名稱中的 `/` 與 `:`，
  /// 若資料夾已存在且非空則依序加上「 1」「 2」…。
  nonisolated private static func availableMountPoint(for volume: NTFSVolume) -> String {
    var name = (volume.volumeName ?? "")
      .replacingOccurrences(of: "/", with: "-")
      .replacingOccurrences(of: ":", with: "-")
      .trimmingCharacters(in: .whitespacesAndNewlines)
    if name.isEmpty || name == "." || name == ".." {
      name = "NTFS Volume"
    }

    let fileManager = FileManager.default
    var candidate = "/Volumes/\(name)"
    var suffix = 1
    while isNonEmptyDirectory(candidate, fileManager: fileManager) {
      candidate = "/Volumes/\(name) \(suffix)"
      suffix += 1
    }
    return candidate
  }

  /// 路徑是否為已存在且非空的資料夾（或已是掛載點）。
  nonisolated private static func isNonEmptyDirectory(_ path: String, fileManager: FileManager) -> Bool {
    var isDirectory: ObjCBool = false
    guard fileManager.fileExists(atPath: path, isDirectory: &isDirectory) else { return false }
    // 已存在但不是資料夾（例如檔案），也視為不可用。
    guard isDirectory.boolValue else { return true }
    let contents = (try? fileManager.contentsOfDirectory(atPath: path)) ?? []
    return !contents.isEmpty
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
