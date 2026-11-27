//
//  LogsView.swift
//  「日志」tab：把共享核心、llama.cpp 与平台事件的输出按时间顺序摊开看。
//
//  存在的意义是排错：真机上「全部提示词推理失败」这类现象的根因（Metal 分配失败、
//  KV 槽位不足、mmproj 与权重不匹配……）只出现在 llama.cpp 自己的输出里。
//  所以这里必须能看全部、能筛错误、能复制、能存成文件发给别人。
//

import SwiftUI
import UniformTypeIdentifiers
import UIKit

struct LogsView: View {

    @ObservedObject private var store = LogStore.shared

    @State private var filter: Filter = .all
    @State private var autoScroll = true
    @State private var toast: String?
    @State private var exportDocument: LogTextDocument?
    @State private var exportFileName = "VLOCR-log"
    @State private var showExporter = false

    enum Filter: String, CaseIterable, Identifiable {
        case all = "全部"
        case warn = "警告及以上"
        case error = "仅错误"

        var id: String { rawValue }

        var minLevel: VLOCRLogLevel {
            switch self {
            case .all: return .debug
            case .warn: return .warn
            case .error: return .error
            }
        }
    }

    private var visible: [VLOCRLogEntry] {
        store.entries.filter { $0.level.rawValue >= filter.minLevel.rawValue }
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                filterBar
                Divider()
                if store.entries.isEmpty {
                    emptyState
                } else if visible.isEmpty {
                    noMatchState
                } else {
                    logList
                }
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("日志")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button {
                        autoScroll.toggle()
                    } label: {
                        Image(systemName: autoScroll ? "arrow.down.circle.fill" : "arrow.down.circle")
                    }
                    .accessibilityLabel(autoScroll ? "关闭自动滚动" : "开启自动滚动")
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Button {
                            copyAll()
                        } label: {
                            Label("复制全部（\(store.entries.count) 行）", systemImage: "doc.on.doc")
                        }
                        Button {
                            prepareExport()
                        } label: {
                            Label("导出 / 下载", systemImage: "square.and.arrow.down")
                        }
                        Divider()
                        Button(role: .destructive) {
                            store.clear()
                            toast = "日志已清空"
                        } label: {
                            Label("清空日志", systemImage: "trash")
                        }
                    } label: {
                        Image(systemName: "ellipsis.circle")
                    }
                    .accessibilityLabel("日志操作")
                }
            }
            .fileExporter(isPresented: $showExporter,
                          document: exportDocument,
                          contentType: .plainText,
                          defaultFilename: exportFileName) { result in
                switch result {
                case .success(let url):
                    toast = "已导出到 \(url.lastPathComponent)"
                case .failure(let error):
                    toast = "导出失败：\(error.localizedDescription)"
                }
            }
            .overlay(alignment: .bottom) {
                if let toast {
                    Text(toast)
                        .font(.footnote)
                        .padding(.horizontal, 14)
                        .padding(.vertical, 9)
                        .background(.ultraThinMaterial, in: Capsule())
                        .padding(.bottom, 16)
                        .transition(.opacity)
                }
            }
        }
    }

    // MARK: - 顶部

    private var filterBar: some View {
        HStack(spacing: 10) {
            Picker("筛选", selection: $filter) {
                ForEach(Filter.allCases) { item in
                    Text(item.rawValue).tag(item)
                }
            }
            .pickerStyle(.segmented)

            Text("\(visible.count)")
                .font(.caption.monospacedDigit())
                .foregroundStyle(.secondary)
                .frame(minWidth: 32, alignment: .trailing)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Color(.secondarySystemGroupedBackground))
    }

    // MARK: - 列表

    private var logList: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 3) {
                    ForEach(visible) { entry in
                        row(entry)
                            .id(entry.id)
                    }
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .onAppear { scrollToBottom(proxy, animated: false) }
            .onChange(of: store.entries.count) { _, _ in
                guard autoScroll else { return }
                scrollToBottom(proxy, animated: false)
            }
            .onChange(of: filter) { _, _ in
                scrollToBottom(proxy, animated: false)
            }
        }
    }

    private func row(_ entry: VLOCRLogEntry) -> some View {
        HStack(alignment: .top, spacing: 6) {
            Text(time(entry.date))
                .foregroundStyle(.secondary)
            Circle()
                .fill(color(entry.level))
                .frame(width: 6, height: 6)
                .padding(.top, 4)
            // 日志正文可能混排阿拉伯文与数字，交给系统按字符属性排版，不做任何顺序变换。
            Text(entry.text)
                .foregroundStyle(entry.level == .error ? Color.red : Color.primary)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .font(.system(size: 11, design: .monospaced))
        .padding(.vertical, 1)
    }

    private var emptyState: some View {
        VStack(spacing: 10) {
            Image(systemName: "text.alignleft")
                .font(.system(size: 34))
                .foregroundStyle(.tertiary)
            Text("暂无日志")
                .font(.headline)
            Text("去「识别」跑一页，或在「设置」里切换一次模型，这里会出现核心与推理库的完整输出。")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 40)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var noMatchState: some View {
        VStack(spacing: 8) {
            Text("当前筛选下没有日志")
                .font(.subheadline)
            Text("共 \(store.entries.count) 行，切到「全部」查看。")
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // MARK: - 动作

    private func scrollToBottom(_ proxy: ScrollViewProxy, animated: Bool) {
        guard let last = visible.last else { return }
        if animated {
            withAnimation(.easeOut(duration: 0.15)) { proxy.scrollTo(last.id, anchor: .bottom) }
        } else {
            proxy.scrollTo(last.id, anchor: .bottom)
        }
    }

    private func copyAll() {
        guard !store.entries.isEmpty else {
            flash("暂无日志可复制")
            return
        }
        UIPasteboard.general.string = store.snapshot()
        flash("已复制 \(store.entries.count) 行")
    }

    private func prepareExport() {
        guard !store.entries.isEmpty else {
            flash("暂无日志可导出")
            return
        }
        exportFileName = LogStore.suggestedFileName()
        exportDocument = LogTextDocument(text: store.snapshot())
        showExporter = true
    }

    private func flash(_ message: String) {
        toast = message
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.6) {
            if toast == message { toast = nil }
        }
    }

    private func time(_ date: Date) -> String {
        Self.timeFormatter.string(from: date)
    }

    private func color(_ level: VLOCRLogLevel) -> Color {
        switch level {
        case .debug: return .secondary
        case .info: return .blue
        case .warn: return .orange
        case .error: return .red
        }
    }

    private static let timeFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss.SSS"
        formatter.locale = Locale(identifier: "en_US_POSIX")
        return formatter
    }()
}

/// fileExporter 需要的最小纯文本文档。
struct LogTextDocument: FileDocument {

    static var readableContentTypes: [UTType] { [.plainText] }

    var text: String

    init(text: String = "") {
        self.text = text
    }

    init(configuration: ReadConfiguration) throws {
        let data = configuration.file.regularFileContents ?? Data()
        text = String(data: data, encoding: .utf8) ?? ""
    }

    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        FileWrapper(regularFileWithContents: Data(text.utf8))
    }
}

#Preview {
    LogsView()
}
