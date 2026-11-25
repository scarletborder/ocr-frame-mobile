//
//  PageDetailView.swift
//  单页结果：整页 Markdown 与逐块明细。不做编辑与保存。
//

import SwiftUI

struct PageDetailView: View {

    let output: PageOutput

    @State private var tab = 0

    var body: some View {
        VStack(spacing: 0) {
            Picker("展示方式", selection: $tab) {
                Text("Markdown").tag(0)
                Text("版面块 \(output.blocks.count)").tag(1)
            }
            .pickerStyle(.segmented)
            .padding(.horizontal, 16)
            .padding(.vertical, 8)

            if tab == 0 {
                markdownView
            } else {
                blockList
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
            VStack(alignment: .leading, spacing: 12) {
                Image(uiImage: output.image)
                    .resizable()
                    .scaledToFit()
                    .frame(maxHeight: 180)
                    .clipShape(RoundedRectangle(cornerRadius: 10))

                // 阿拉伯文 / 波斯文 / 普什图文按逻辑顺序渲染，字号 ≥17、行距放宽，
                // 不加 letter-spacing（AGENTS §6.2）。
                Text(output.markdown.isEmpty ? "（未识别出文本）" : output.markdown)
                    .font(.system(size: 17))
                    .lineSpacing(7)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
            }
            .padding(16)
        }
    }

    private var blockList: some View {
        List(output.blocks) { block in
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
                    Text(block.recognized ? "已识别" : "插图 / 未识别")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
                Text(block.text.isEmpty ? "—" : block.text)
                    .font(.system(size: 16))
                    .lineSpacing(4)
                    .lineLimit(8)
                    .textSelection(.enabled)
            }
            .padding(.vertical, 4)
        }
        .listStyle(.plain)
    }
}
