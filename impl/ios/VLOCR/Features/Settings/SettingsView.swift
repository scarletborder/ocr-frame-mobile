//
//  SettingsView.swift
//  「设置」tab：只做两件事 —— 选两个模型、看预设。
//  模型文件通过系统「文件」选择器导入，导入后复制进 App 文档目录（可在 Finder 里直接拖入）。
//

import SwiftUI
import UniformTypeIdentifiers

struct SettingsView: View {

    @EnvironmentObject private var store: ModelStore
    @EnvironmentObject private var host: EngineHost

    @State private var pickLayout = false
    @State private var pickVLM = false
    @State private var pickMMProj = false
    @State private var errorMessage: String?

    private var layoutTypes: [UTType] { [UTType(filenameExtension: "onnx") ?? .data] }
    private var ggufTypes: [UTType] { [UTType(filenameExtension: "gguf") ?? .data] }

    var body: some View {
        NavigationStack {
            List {
                presetSection
                modelSection
                engineSection
                if let errorMessage {
                    Section {
                        Text(errorMessage)
                            .font(.footnote)
                            .foregroundStyle(.red)
                    }
                }
                aboutSection
            }
            .navigationTitle("设置")
            .fileImporter(isPresented: $pickLayout,
                          allowedContentTypes: layoutTypes,
                          allowsMultipleSelection: false) { result in
                importModel(result, slot: .layout)
            }
            .fileImporter(isPresented: $pickVLM,
                          allowedContentTypes: ggufTypes,
                          allowsMultipleSelection: false) { result in
                importModel(result, slot: .vlm)
            }
            .fileImporter(isPresented: $pickMMProj,
                          allowedContentTypes: ggufTypes,
                          allowsMultipleSelection: false) { result in
                importMMProj(result)
            }
        }
    }

    // MARK: - 预设

    private var presetSection: some View {
        Section {
            LabeledContent("预设", value: "\(VLPreset.name) \(VLPreset.version)")
            LabeledContent("产线版本", value: VLPreset.pipelineVersion)
            LabeledContent("生成长度上限", value: "\(VLPreset.maxNewTokens) tokens")
        } header: {
            Text("预设（写死为当前 VL 线做法）")
        } footer: {
            Text("这些参数由共享核心 impl/shard 提供，与后端 VL 线同源。")
        }
    }

    // MARK: - 模型

    private var modelSection: some View {
        Section {
            modelRow(slot: .layout, selected: store.layoutName) {
                pickLayout = true
            }
            .swipeActions {
                Button("清除", role: .destructive) {
                    store.clear(slot: .layout)
                    host.invalidate()
                }
            }

            modelRow(slot: .vlm, selected: store.vlmName) {
                pickVLM = true
            }
            .swipeActions {
                Button("清除", role: .destructive) {
                    store.clear(slot: .vlm)
                    host.invalidate()
                }
            }

            Button {
                pickMMProj = true
            } label: {
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("视觉投影器")
                        Text("*-mmproj.gguf（与识别模型成对使用）")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Text(store.mmprojName ?? "未选择")
                            .font(.caption)
                            .foregroundStyle(store.mmprojName == nil ? .orange : .secondary)
                            .lineLimit(1)
                    }
                    Spacer()
                    Image(systemName: "chevron.right").foregroundStyle(.tertiary)
                }
            }
            .buttonStyle(.plain)

            Button {
                store.rescanLooseModels()
                host.invalidate()
            } label: {
                HStack {
                    Text("重新扫描 Documents 目录")
                    Spacer()
                    Image(systemName: "arrow.clockwise").foregroundStyle(.tertiary)
                }
            }
            .buttonStyle(.plain)
        } header: {
            Text("模型")
        } footer: {
            Text("VL 线必须两个模型成对：版面分析（PP-DocLayoutV3）与视觉语言模型（PaddleOCR-VL 1.6）。选视觉语言模型后会自动在同一目录里找 mmproj；用 iTunes 拖进 App 根目录的模型会被自动收进 Models/。")
        }
    }

    private func modelRow(slot: ModelSlot, selected: String?, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(slot.title)
                    Text(slot.subtitle)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text(selected ?? "未选择")
                        .font(.caption)
                        .foregroundStyle(selected == nil ? .orange : .secondary)
                        .lineLimit(1)
                }
                Spacer()
                Image(systemName: "chevron.right").foregroundStyle(.tertiary)
            }
        }
        .buttonStyle(.plain)
    }

    // MARK: - 运行方式

    private var engineSection: some View {
        Section {
            Picker("运行方式", selection: Binding(
                get: { store.mode },
                set: { store.setMode($0); host.invalidate() }
            )) {
                ForEach(VLMEngineMode.allCases) { mode in
                    Text(mode.title).tag(mode)
                }
            }

            if store.mode == .remote {
                TextField("http://主机:8118/v1", text: $store.remoteBaseURL)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .keyboardType(.URL)
                Button("保存地址") {
                    store.saveRemoteURL()
                    host.invalidate()
                }
            }

            LabeledContent("版面分析引擎", value: VLOCREngine.layoutEngineAvailable() ? "已链接" : "未链接")
            LabeledContent("VLM 引擎", value: VLOCREngine.localVlmAvailable() ? "已链接" : "未链接")
        } header: {
            Text("运行方式")
        } footer: {
            Text("默认在设备内跑 GGUF；远程模式用于在未构建 llama.xcframework 时验证整条链路（llama.cpp server / mlx-vlm server 的 OpenAI 兼容接口）。")
        }
    }

    private var aboutSection: some View {
        Section("关于") {
            Text("本 demo 只实现「选图 → 一次性识别」，不做保存、项目与校对。模型文件放在 App 文档目录的 Models/ 下，可通过「文件」App 或 Finder 直接拷入。")
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
    }

    // MARK: - 导入

    private func importModel(_ result: Result<[URL], Error>, slot: ModelSlot) {
        switch result {
        case .failure(let error):
            errorMessage = "文件选择失败：\(error.localizedDescription)"
        case .success(let urls):
            guard let url = urls.first else { return }
            errorMessage = nil
            Task {
                do {
                    try await store.importFile(from: url, into: slot)
                    host.invalidate()
                } catch {
                    errorMessage = "导入失败：\(error.localizedDescription)"
                }
            }
        }
    }

    private func importMMProj(_ result: Result<[URL], Error>) {
        switch result {
        case .failure(let error):
            errorMessage = "文件选择失败：\(error.localizedDescription)"
        case .success(let urls):
            guard let url = urls.first else { return }
            errorMessage = nil
            Task {
                do {
                    try await store.selectMMProj(from: url)
                    host.invalidate()
                } catch {
                    errorMessage = "导入失败：\(error.localizedDescription)"
                }
            }
        }
    }
}
