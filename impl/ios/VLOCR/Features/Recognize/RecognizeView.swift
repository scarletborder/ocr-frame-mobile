//
//  RecognizeView.swift
//  「识别」tab：一个大的相机按钮 + 相册 / 文件两条上传途径，选完即一次性识别。
//

import PhotosUI
import SwiftUI
import UniformTypeIdentifiers

struct RecognizeView: View {

    @EnvironmentObject private var store: ModelStore
    @EnvironmentObject private var host: EngineHost
    @StateObject private var model = RecognizeViewModel()

    @State private var showCamera = false
    @State private var showFileImporter = false
    @State private var photoItems: [PhotosPickerItem] = []

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 20) {
                    statusCard
                    cameraButton
                    HStack(spacing: 12) {
                        photoPicker
                        fileButton
                    }
                    if let message = model.errorMessage {
                        errorCard(message)
                    }
                    if !model.items.isEmpty {
                        itemList
                    }
                }
                .padding(16)
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("VL 识别")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                if !model.items.isEmpty {
                    ToolbarItem(placement: .topBarTrailing) {
                        Button("清空") { model.clear() }
                            .disabled(model.isRunning)
                    }
                }
            }
            .fullScreenCover(isPresented: $showCamera) {
                CameraPicker { image in
                    model.recognize([image], store: store, host: host)
                }
                .ignoresSafeArea()
            }
            .fileImporter(isPresented: $showFileImporter,
                          allowedContentTypes: [.image],
                          allowsMultipleSelection: true) { result in
                handleFileImport(result)
            }
            .onChange(of: photoItems) { _, newValue in
                loadPhotos(newValue)
            }
        }
    }

    // MARK: - 状态

    private var statusCard: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Image(systemName: store.isReady ? "checkmark.seal.fill" : "exclamationmark.triangle.fill")
                    .foregroundStyle(store.isReady ? Color.green : Color.orange)
                Text(store.isReady ? "模型已就绪" : "模型未就绪")
                    .font(.headline)
            }
            Text("预设：\(VLPreset.name) \(VLPreset.version)")
                .font(.subheadline)
                .foregroundStyle(.secondary)
            Text("产线 \(VLPreset.pipelineVersion) · \(store.mode.title)")
                .font(.subheadline)
                .foregroundStyle(.secondary)
            if !store.isReady {
                Text("请到「设置」里选择版面分析模型与视觉语言识别模型。")
                    .font(.footnote)
                    .foregroundStyle(.orange)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 16))
    }

    private func errorCard(_ message: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .top, spacing: 8) {
                Image(systemName: "xmark.octagon.fill").foregroundStyle(.red)
                Text(message).font(.footnote).frame(maxWidth: .infinity, alignment: .leading)
            }
            // 技术细节（llama.cpp 的真实报错、token 数、上下文数值）只在日志页出现，
            // 这里给一句指路，避免用户只能盯着这句兜底文案。
            Text("详细原因见「日志」tab，可直接复制或导出。")
                .font(.caption2)
                .foregroundStyle(.secondary)
        }
        .padding(12)
        .background(Color.red.opacity(0.08), in: RoundedRectangle(cornerRadius: 12))
    }

    // MARK: - 入口

    private var cameraButton: some View {
        Button {
            showCamera = true
        } label: {
            VStack(spacing: 12) {
                Image(systemName: "camera.fill")
                    .font(.system(size: 42, weight: .semibold))
                Text("拍摄识别")
                    .font(.headline)
            }
            .foregroundStyle(.white)
            .frame(width: 200, height: 200)
            .background(
                LinearGradient(colors: [Color.accentColor, Color.accentColor.opacity(0.7)],
                               startPoint: .topLeading,
                               endPoint: .bottomTrailing),
                in: Circle()
            )
            .shadow(color: Color.accentColor.opacity(0.3), radius: 16, y: 8)
        }
        .buttonStyle(.plain)
        .disabled(model.isRunning)
        .opacity(model.isRunning ? 0.5 : 1)
    }

    private var photoPicker: some View {
        PhotosPicker(selection: $photoItems, maxSelectionCount: 20, matching: .images) {
            entryLabel(title: "从相册选择", systemImage: "photo.on.rectangle")
        }
        .disabled(model.isRunning)
    }

    private var fileButton: some View {
        Button {
            showFileImporter = true
        } label: {
            entryLabel(title: "从文件选择", systemImage: "folder")
        }
        .buttonStyle(.plain)
        .disabled(model.isRunning)
    }

    private func entryLabel(title: String, systemImage: String) -> some View {
        HStack(spacing: 8) {
            Image(systemName: systemImage)
            Text(title).font(.subheadline.weight(.medium))
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 14)
        .background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 14))
    }

    // MARK: - 结果列表

    private var itemList: some View {
        VStack(spacing: 12) {
            ForEach(model.items) { item in
                if case .done(let output) = item.status {
                    NavigationLink {
                        PageDetailView(output: output)
                    } label: {
                        itemRow(item)
                    }
                    .buttonStyle(.plain)
                } else {
                    itemRow(item)
                }
            }
        }
    }

    private func itemRow(_ item: RecognizeViewModel.Item) -> some View {
        HStack(spacing: 12) {
            Image(uiImage: item.thumbnail)
                .resizable()
                .scaledToFill()
                .frame(width: 56, height: 72)
                .clipShape(RoundedRectangle(cornerRadius: 8))

            VStack(alignment: .leading, spacing: 4) {
                Text(title(for: item.status))
                    .font(.subheadline.weight(.medium))
                Text(detail(for: item.status))
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
            Spacer(minLength: 0)
            switch item.status {
            case .pending:
                Image(systemName: "clock").foregroundStyle(.secondary)
            case .running(let done, let total):
                ProgressView(value: total > 0 ? Double(done) / Double(total) : 0)
                    .frame(width: 44)
            case .done:
                Image(systemName: "chevron.right").foregroundStyle(.tertiary)
            case .failed:
                Image(systemName: "exclamationmark.circle.fill").foregroundStyle(.red)
            }
        }
        .padding(12)
        .background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 14))
    }

    private func title(for status: RecognizeViewModel.Status) -> String {
        switch status {
        case .pending: return "等待识别"
        case .running: return "识别中…"
        case .done: return "识别完成"
        case .failed: return "识别失败"
        }
    }

    private func detail(for status: RecognizeViewModel.Status) -> String {
        switch status {
        case .pending:
            return "排队中"
        case .running(let done, let total):
            return total > 0 ? "已处理 \(done)/\(total) 个版面块" : "正在分析版面…"
        case .done(let output):
            return String(format: "%.1f 秒 · %d 个版面块", output.seconds, output.blocks.count)
        case .failed(let message):
            return message
        }
    }

    // MARK: - 输入

    private func loadPhotos(_ selections: [PhotosPickerItem]) {
        guard !selections.isEmpty else { return }
        Task {
            var images: [UIImage] = []
            for item in selections {
                if let data = try? await item.loadTransferable(type: Data.self),
                   let image = UIImage(data: data) {
                    images.append(image)
                }
            }
            photoItems = []
            if images.isEmpty {
                model.errorMessage = "选中的图片无法读取。"
            } else {
                model.recognize(images, store: store, host: host)
            }
        }
    }

    private func handleFileImport(_ result: Result<[URL], Error>) {
        switch result {
        case .failure(let error):
            model.errorMessage = "文件选择失败：\(error.localizedDescription)"
        case .success(let urls):
            Task {
                var images: [UIImage] = []
                for url in urls {
                    let scoped = url.startAccessingSecurityScopedResource()
                    defer {
                        if scoped { url.stopAccessingSecurityScopedResource() }
                    }
                    if let data = try? Data(contentsOf: url), let image = UIImage(data: data) {
                        images.append(image)
                    }
                }
                if images.isEmpty {
                    model.errorMessage = "选中的文件不是可识别的图片。"
                } else {
                    model.recognize(images, store: store, host: host)
                }
            }
        }
    }
}
