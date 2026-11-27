//
//  LogStore.swift
//  App 内日志缓冲。
//
//  三个来源汇进同一路日志，按时间顺序交错，才能回答「到底卡在哪一段」：
//    1. 共享核心 impl/shard（版面后处理、逐块编排、Markdown 合并）；
//    2. llama.cpp / ggml 自己打的日志（Metal 分配失败、KV 槽位不足、mmproj 不匹配……）；
//    3. 平台事件（引擎创建参数、选图、识别耗时）。
//
//  C 回调可能来自推理线程甚至 llama.cpp 内部线程，所以写入走锁 + 合并到主线程发布，
//  避免每来一行就刷新一次 SwiftUI（模型加载时会一次刷几百行）。
//

import Combine
import Foundation
import UIKit

enum VLOCRLogLevel: Int, CaseIterable, Identifiable {
    case debug = 0
    case info = 1
    case warn = 2
    case error = 3

    var id: Int { rawValue }

    var title: String {
        switch self {
        case .debug: return "调试"
        case .info: return "信息"
        case .warn: return "警告"
        case .error: return "错误"
        }
    }
}

struct VLOCRLogEntry: Identifiable {
    let id: Int
    let date: Date
    let level: VLOCRLogLevel
    let text: String
}

final class LogStore: ObservableObject {

    static let shared = LogStore()

    /// 按行数封顶：llama.cpp 加载模型时会刷几百行，不设上限会白白吃掉几十 MB。
    static let capacity = 4000

    @Published private(set) var entries: [VLOCRLogEntry] = []

    private let lock = NSLock()
    private var pending: [VLOCRLogEntry] = []
    private var nextId = 0
    private var flushScheduled = false

    private let timeFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss.SSS"
        formatter.locale = Locale(identifier: "en_US_POSIX")
        return formatter
    }()

    private init() {}

    // MARK: - 写入

    /// 任意线程可调。
    func append(level: VLOCRLogLevel, text: String) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }

        lock.lock()
        pending.append(VLOCRLogEntry(id: nextId, date: Date(), level: level, text: trimmed))
        nextId += 1
        let needsFlush = !flushScheduled
        flushScheduled = true
        lock.unlock()

        if needsFlush {
            DispatchQueue.main.async { [weak self] in self?.flush() }
        }
    }

    func clear() {
        lock.lock()
        pending.removeAll()
        lock.unlock()
        entries.removeAll()
    }

    // MARK: - 导出

    /// 纯文本快照：复制与下载共用同一份内容，避免两边不一致。
    ///
    /// 只导出**传进来的这批**（即当前筛选结果），并在头部写明筛选口径与比例——
    /// 否则拿到一份「只有错误」的日志会误以为是全量，反而更难定位。
    func snapshot(of selected: [VLOCRLogEntry], scope: String) -> String {
        var lines: [String] = []
        lines.append("VLOCR 日志")
        lines.append("导出时间：\(Self.fileTimeFormatter.string(from: Date()))")
        lines.append(contentsOf: Self.environmentLines)
        lines.append("筛选：\(scope)（\(selected.count) 行 / 缓冲区共 \(entries.count) 行）")
        lines.append(String(repeating: "-", count: 48))
        for entry in selected {
            lines.append("\(timeFormatter.string(from: entry.date)) [\(entry.level.title)] \(entry.text)")
        }
        return lines.joined(separator: "\n")
    }

    /// 导出文件名（不含扩展名）。冒号在部分目标上非法，统一换成短横线。
    static func suggestedFileName(suffix: String? = nil) -> String {
        let stamp = fileTimeFormatter.string(from: Date())
            .replacingOccurrences(of: ":", with: "-")
            .replacingOccurrences(of: " ", with: "-")
        guard let suffix, !suffix.isEmpty else { return "VLOCR-log-\(stamp)" }
        return "VLOCR-log-\(stamp)-\(suffix)"
    }

    // MARK: - 环境信息

    /// 日志开头固定写一遍环境：同一份日志发给别人时不用再问「什么机型、什么版本」。
    static var environmentLines: [String] {
        let version = ProcessInfo.processInfo.operatingSystemVersion
        let app = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
        let build = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?"
        return [
            "设备：\(UIDevice.current.model) · iOS \(version.majorVersion).\(version.minorVersion).\(version.patchVersion)",
            "App：\(app) (\(build))",
            "预设：\(VLPreset.name) \(VLPreset.version) · 产线 \(VLPreset.pipelineVersion) · 生成上限 \(VLPreset.maxNewTokens) tokens",
            "引擎：版面 \(VLOCREngine.layoutEngineAvailable() ? "已链接" : "未链接") · VLM \(VLOCREngine.localVlmAvailable() ? "已链接" : "未链接")",
        ]
    }

    private static let fileTimeFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        formatter.locale = Locale(identifier: "en_US_POSIX")
        return formatter
    }()

    // MARK: - 内部

    private func flush() {
        lock.lock()
        let batch = pending
        pending.removeAll()
        flushScheduled = false
        lock.unlock()

        guard !batch.isEmpty else { return }
        entries.append(contentsOf: batch)
        if entries.count > Self.capacity {
            entries.removeFirst(entries.count - Self.capacity)
        }
    }
}

enum VLOCRLogging {

    /// 进程内装一次即可：C 侧未安装 sink 时，核心与 llama.cpp 的日志都是空实现。
    static func install() {
        vlcore_set_log({ level, message, _ in
            guard let message else { return }
            let text = String(cString: message)
            LogStore.shared.append(level: VLOCRLogLevel(rawValue: Int(level)) ?? .info, text: text)
        }, nil)

        LogStore.shared.append(level: .info,
                               text: "日志已启用（缓冲区 \(LogStore.capacity) 行，含核心与 llama.cpp 输出）")
        for line in LogStore.environmentLines {
            LogStore.shared.append(level: .info, text: line)
        }
    }

    /// 平台侧事件也写进同一路日志。
    static func note(_ text: String, level: VLOCRLogLevel = .info) {
        LogStore.shared.append(level: level, text: text)
    }
}
