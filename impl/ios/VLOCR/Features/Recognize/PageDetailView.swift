//
//  PageDetailView.swift
//  单页结果：Markdown / 版面块 / 原图 三个 tab。不做编辑与保存。
//
//  · Markdown 只放文本，不再嵌原图（原图在「原图」tab 里单独看）
//  · 版面块展示标签、阅读顺序、识别状态与失败原因
//  · 原图可拖拽缩放，双击框跳到对应版面块
//

import SwiftUI

struct PageDetailView: View {

    let output: PageOutput

    @State private var tab = 0
    @State private var focusedID: UUID?

    var body: some View {
        VStack(spacing: 0) {
            Picker("展示方式", selection: $tab) {
                Text("Markdown").tag(0)
                Text("版面块 \(output.blocks.count)").tag(1)
                Text("原图").tag(2)
            }
            .pickerStyle(.segmented)
            .padding(.horizontal, 16)
            .padding(.vertical, 8)

            switch tab {
            case 0:
                markdownView
            case 1:
                blockList
            default:
                PageImageView(output: output) { block in
                    focusedID = block.id
                    tab = 1
                }
            }
        }
        .navigationTitle("识别结果")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Text(String(format: "%.1fs", output.seconds))
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var markdownView: some View {
        ScrollView {
            // 阿拉伯文 / 波斯文 / 普什图文按逻辑顺序渲染，字号 ≥17、行距放宽，
            // 不加 letter-spacing（AGENTS §6.2）。
            Text(output.markdown.isEmpty ? "（未识别出文本）" : output.markdown)
                .font(.system(size: 17))
                .lineSpacing(7)
                .frame(maxWidth: .infinity, alignment: .leading)
                .textSelection(.enabled)
                .padding(16)
        }
    }

    private var blockList: some View {
        ScrollViewReader { proxy in
            List(output.blocks) { block in
                blockRow(block)
                    .id(block.id)
                    .listRowBackground(block.id == focusedID ? Color.accentColor.opacity(0.10) : Color.clear)
            }
            .listStyle(.plain)
            .task(id: focusedID) {
                guard let id = focusedID else { return }
                try? await Task.sleep(for: .milliseconds(60))
                withAnimation { proxy.scrollTo(id, anchor: .center) }
            }
        }
    }

    private func blockRow(_ block: RecognizedBlock) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Text(block.label)
                    .font(.caption2.weight(.semibold))
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(Color.accentColor.opacity(0.12), in: Capsule())
                Text(block.order > 0 ? "#\(block.order)" : "不排序")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                Spacer()
                Text(block.recognized ? "已识别" : (block.isImageLike ? "插图" : "未识别"))
                    .font(.caption2)
                    .foregroundStyle(block.recognized || block.isImageLike ? .secondary : Color.orange)
            }
            Text(block.text.isEmpty ? "—" : block.text)
                .font(.system(size: 16))
                .lineSpacing(4)
                .lineLimit(8)
                .textSelection(.enabled)
            // 未识别时必须给出原因，否则只能看到「未识别」而无从下手
            if !block.recognized, !block.error.isEmpty {
                Text(block.error)
                    .font(.caption2)
                    .foregroundStyle(.orange)
            }
        }
        .padding(.vertical, 4)
    }
}
