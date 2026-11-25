//
//  ModelStore.swift
//  「设置 → 模型」的两个槽位（对应后端 data/models/{vl_layout,vl_vlm}/）。
//
//  iOS 沙盒里没有后端那种目录约定，因此这里做两件事：
//    1. 用户从「文件」App 选中的模型统一复制进 Documents/Models/（已开启
//       UIFileSharingEnabled，可用 Finder / 文件 App 直接拖入大文件）；
//    2. 只持久化文件名 —— 复制后路径稳定，llama.cpp / ONNX Runtime 可直接按路径 mmap。
//

import Combine
import Foundation

enum ModelSlot: String, Identifiable {
    case layout
    case vlm

    var id: String { rawValue }

    var title: String {
        switch self {
        case .layout: return "版面分析模型"
        case .vlm: return "视觉语言识别模型"
        }
    }

    var subtitle: String {
        switch self {
        case .layout: return "PP-DocLayoutV3（.onnx）"
        case .vlm: return "PaddleOCR-VL-1.6（.gguf）"
        }
    }

    var fileExtension: String {
        switch self {
        case .layout: return "onnx"
        case .vlm: return "gguf"
        }
    }
}

/// VLM 的来源。共享核心同时支持「本地 GGUF」与「远程回调」，两者走同一条编排。
enum VLMEngineMode: String, CaseIterable, Identifiable {
    case local
    case remote

    var id: String { rawValue }

    var title: String {
        switch self {
        case .local: return "本地 GGUF（llama.cpp）"
        case .remote: return "远程服务（OpenAI 兼容）"
        }
    }
}

@MainActor
final class ModelStore: ObservableObject {

    @Published private(set) var layoutName: String? = nil
    @Published private(set) var vlmName: String? = nil
    @Published private(set) var mmprojName: String? = nil
    @Published private(set) var mode: VLMEngineMode = .local
    @Published var remoteBaseURL: String = ""

    private let defaults = UserDefaults.standard
    private let layoutKey = "model.layout"
    private let vlmKey = "model.vlm"
    private let mmprojKey = "model.mmproj"
    private let modeKey = "engine.mode"
    private let remoteKey = "engine.remote"

    static let modelsDirectory: URL = {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let directory = documents.appendingPathComponent("Models", isDirectory: true)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }()

    init() {
        layoutName = defaults.string(forKey: layoutKey)
        vlmName = defaults.string(forKey: vlmKey)
        mmprojName = defaults.string(forKey: mmprojKey)
        mode = VLMEngineMode(rawValue: defaults.string(forKey: modeKey) ?? "") ?? .local
        remoteBaseURL = defaults.string(forKey: remoteKey) ?? "http://192.168.1.10:8118/v1"
        // 上次记录的模型被删掉时自动清空，避免用旧名字去加载。
        if let name = layoutName, url(for: name) == nil { layoutName = nil }
        if let name = vlmName, url(for: name) == nil { vlmName = nil }
        if let name = mmprojName, url(for: name) == nil { mmprojName = nil }
        // iTunes 文件共享把模型丢进 Documents 根目录时，这里自动收纳并选好槽位
        rescanLooseModels()
    }

    var layoutURL: URL? { layoutName.flatMap(url(for:)) }
    var vlmURL: URL? { vlmName.flatMap(url(for:)) }
    var mmprojURL: URL? { mmprojName.flatMap(url(for:)) }

    var isReady: Bool {
        guard layoutURL != nil, VLOCREngine.layoutEngineAvailable() else { return false }
        switch mode {
        case .local:
            return vlmURL != nil && mmprojURL != nil && VLOCREngine.localVlmAvailable()
        case .remote:
            return !remoteBaseURL.isEmpty && URL(string: remoteBaseURL) != nil
        }
    }

    func setMode(_ value: VLMEngineMode) {
        mode = value
        defaults.set(value.rawValue, forKey: modeKey)
    }

    func saveRemoteURL() {
        defaults.set(remoteBaseURL, forKey: remoteKey)
    }

    // MARK: - 导入

    /// 把选中的文件复制进 App 文档目录；同名文件直接覆盖。返回落地后的文件名。
    @discardableResult
    private func copyIntoModels(_ source: URL) async throws -> String {
        let needsScope = source.startAccessingSecurityScopedResource()
        defer {
            if needsScope { source.stopAccessingSecurityScopedResource() }
        }
        let fileName = source.lastPathComponent
        let destination = Self.modelsDirectory.appendingPathComponent(fileName)
        if source.standardizedFileURL.path != destination.standardizedFileURL.path {
            try await Task.detached(priority: .userInitiated) {
                let manager = FileManager.default
                if manager.fileExists(atPath: destination.path) {
                    try manager.removeItem(at: destination)
                }
                try manager.copyItem(at: source, to: destination)
            }.value
        }
        return fileName
    }

    func importFile(from source: URL, into slot: ModelSlot) async throws {
        let fileName = try await copyIntoModels(source)
        switch slot {
        case .layout:
            layoutName = fileName
            defaults.set(fileName, forKey: layoutKey)
        case .vlm:
            vlmName = fileName
            defaults.set(fileName, forKey: vlmKey)
            autoDetectMMProj()
        }
    }

    /// 视觉投影器是独立文件（*-mmproj.gguf），优先自动在同目录找，找不到再手动选。
    func autoDetectMMProj() {
        guard let vlmURL else { return }
        let directory = vlmURL.deletingLastPathComponent()
        let candidates = (try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? []
        let match = candidates.first { url in
            let lower = url.lastPathComponent.lowercased()
            return lower.hasSuffix(".gguf") && (lower.contains("mmproj") || lower.contains("mm_proj"))
        }
        guard let match else { return }
        mmprojName = match.lastPathComponent
        defaults.set(match.lastPathComponent, forKey: mmprojKey)
    }

    func selectMMProj(from source: URL) async throws {
        let fileName = try await copyIntoModels(source)
        mmprojName = fileName
        defaults.set(fileName, forKey: mmprojKey)
    }

    func clear(slot: ModelSlot) {
        switch slot {
        case .layout:
            layoutName = nil
            defaults.removeObject(forKey: layoutKey)
        case .vlm:
            vlmName = nil
            mmprojName = nil
            defaults.removeObject(forKey: vlmKey)
            defaults.removeObject(forKey: mmprojKey)
        }
    }

    private func url(for name: String) -> URL? {
        let candidate = Self.modelsDirectory.appendingPathComponent(name)
        return FileManager.default.fileExists(atPath: candidate.path) ? candidate : nil
    }

    /// Windows + iTunes 文件共享只能把文件放进 Documents 根目录（不能建子目录），
    /// 所以启动时/手动扫描时把根目录里的 .onnx / .gguf 收进 Models/，并自动填槽。
    func rescanLooseModels() {
        let manager = FileManager.default
        guard let documents = manager.urls(for: .documentDirectory, in: .userDomainMask).first,
              let items = try? manager.contentsOfDirectory(at: documents, includingPropertiesForKeys: [.isDirectoryKey]) else {
            return
        }

        for item in items {
            let isDirectory = (try? item.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory ?? false
            guard !isDirectory else { continue }
            let ext = item.pathExtension.lowercased()
            guard ext == "onnx" || ext == "gguf" else { continue }
            let destination = Self.modelsDirectory.appendingPathComponent(item.lastPathComponent)
            try? manager.removeItem(at: destination)
            try? manager.moveItem(at: item, to: destination)
        }

        let models = (try? manager.contentsOfDirectory(at: Self.modelsDirectory, includingPropertiesForKeys: nil)) ?? []

        if layoutName == nil,
           let onnx = models.first(where: { $0.pathExtension.lowercased() == "onnx" }) {
            layoutName = onnx.lastPathComponent
            defaults.set(onnx.lastPathComponent, forKey: layoutKey)
        }

        let ggufs = models.filter { $0.pathExtension.lowercased() == "gguf" }
        if vlmName == nil,
           let main = ggufs.first(where: { !$0.lastPathComponent.lowercased().contains("mmproj") }) {
            vlmName = main.lastPathComponent
            defaults.set(main.lastPathComponent, forKey: vlmKey)
        }
        if mmprojName == nil,
           let mmproj = ggufs.first(where: { $0.lastPathComponent.lowercased().contains("mmproj") }) {
            mmprojName = mmproj.lastPathComponent
            defaults.set(mmproj.lastPathComponent, forKey: mmprojKey)
        }
    }
}

// MARK: - 引擎装配

/// 引擎只创建一次并缓存（对应后端「模型常驻、绝不在请求里实例化」）。
@MainActor
final class EngineHost: ObservableObject {

    @Published private(set) var isLoading = false

    private var pipeline: VLPipeline?
    private var signature = ""

    func makePipeline(for store: ModelStore) async throws -> VLPipeline {
        let current = signature(for: store)
        if let pipeline, current == signature {
            return pipeline
        }

        guard let layoutURL = store.layoutURL else {
            throw VLError("请先在「设置」里选择版面分析模型。")
        }
        guard VLOCREngine.layoutEngineAvailable() else {
            throw VLError("当前构建未链接 onnxruntime.xcframework，无法进行版面分析。请先运行 scripts/bootstrap.sh。")
        }

        let localVlm = store.mode == .local
        if localVlm {
            guard store.vlmURL != nil, store.mmprojURL != nil else {
                throw VLError("请先在「设置」里选择视觉语言识别模型与视觉投影器。")
            }
            guard VLOCREngine.localVlmAvailable() else {
                throw VLError("当前构建未链接 llama.xcframework（含 libmtmd）。请先运行 scripts/bootstrap.sh，或改用远程服务模式。")
            }
        }

        isLoading = true
        defer { isLoading = false }

        let vlmPath = localVlm ? store.vlmURL?.path : nil
        let mmprojPath = localVlm ? store.mmprojURL?.path : nil
        let engine = try await Task.detached(priority: .userInitiated) {
            try VLOCREngine(layoutPath: layoutURL.path, vlmPath: vlmPath, mmprojPath: mmprojPath)
        }.value

        if !localVlm {
            guard let url = URL(string: store.remoteBaseURL) else {
                throw VLError("远程服务地址无效。")
            }
            _ = try engine.useRemoteService(baseURL: url, model: "PaddleOCR-VL")
        }

        let created = VLPipeline(engine: engine)
        pipeline = created
        signature = current
        return created
    }

    func invalidate() {
        pipeline = nil
        signature = ""
    }

    private func signature(for store: ModelStore) -> String {
        [
            store.layoutURL?.path ?? "-",
            store.vlmURL?.path ?? "-",
            store.mmprojURL?.path ?? "-",
            store.mode.rawValue,
            store.remoteBaseURL,
        ].joined(separator: "|")
    }
}
