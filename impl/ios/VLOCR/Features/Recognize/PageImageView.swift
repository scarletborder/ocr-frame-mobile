//
//  PageImageView.swift
//  「原图」tab：可拖拽/缩放的整页图 + 可向左展开的叠加层工具栏。
//
//  叠加层语义对齐 PC 文档页：
//    · 文本区域 —— 版面检测到的非插图区域
//    · 行       —— 文本类区域的行框（VL 线只有块级坐标，这里按块框虚线近似，见 docs/03-ui-ux.md）
//    · 阅读顺序 —— 块右上角的序号
//  双击某个框 → 跳到「版面块」里对应的块；双击空白 → 复位缩放。
//

import SwiftUI

struct PageImageView: View {

    let output: PageOutput
    var onSelectBlock: (RecognizedBlock) -> Void

    @State private var showRegions = true
    @State private var showLines = false
    @State private var showOrder = false
    @State private var toolbarOpen = false

    @State private var scale: CGFloat = 1
    @State private var baseScale: CGFloat = 1
    @State private var offset: CGSize = .zero
    @State private var baseOffset: CGSize = .zero

    private let minScale: CGFloat = 1
    private let maxScale: CGFloat = 6

    var body: some View {
        GeometryReader { geo in
            let fitted = fittedSize(in: geo.size)
            ZStack {
                Color(.systemGroupedBackground)
                content(size: fitted)
                    .scaleEffect(scale)
                    .offset(offset)
                    .gesture(panGesture(image: fitted))
                    .simultaneousGesture(magnifyGesture(image: fitted))
            }
            .clipped()
            .overlay(alignment: .bottomTrailing) {
                toolbar.padding(16)
            }
            .overlay(alignment: .topLeading) {
                Text("双击框跳到版面块 · 双击空白复位")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .background(.regularMaterial, in: Capsule())
                    .padding(12)
            }
        }
    }

    // MARK: - 内容

    private func fittedSize(in container: CGSize) -> CGSize {
        let image = output.image.size
        guard image.width > 0, image.height > 0, container.width > 0, container.height > 0 else {
            return container
        }
        let ratio = min(container.width / image.width, container.height / image.height)
        return CGSize(width: image.width * ratio, height: image.height * ratio)
    }

    private func content(size: CGSize) -> some View {
        ZStack(alignment: .topLeading) {
            Image(uiImage: output.image)
                .resizable()
                .frame(width: size.width, height: size.height)
            Canvas { context, _ in
                draw(blocks: output.blocks, size: size, in: &context)
            }
            .frame(width: size.width, height: size.height)
            .allowsHitTesting(false)
        }
        .contentShape(Rectangle())
        // 手势挂在缩放之前，坐标即「适配尺寸」坐标系，命中测试不用再反变换
        .gesture(
            SpatialTapGesture(count: 2).onEnded { value in
                if let block = hitTest(value.location, size: size) {
                    onSelectBlock(block)
                } else {
                    withAnimation(.snappy) {
                        scale = 1
                        baseScale = 1
                        offset = .zero
                        baseOffset = .zero
                    }
                }
            }
        )
    }

    private func draw(blocks: [RecognizedBlock], size: CGSize, in context: inout GraphicsContext) {
        let image = output.image.size
        guard image.width > 0, image.height > 0 else { return }
        let sx = size.width / image.width
        let sy = size.height / image.height

        for block in blocks {
            let rect = CGRect(x: block.bbox.minX * sx, y: block.bbox.minY * sy,
                              width: block.bbox.width * sx, height: block.bbox.height * sy)
            guard rect.width > 1, rect.height > 1 else { continue }

            if showRegions && !block.isImageLike {
                context.stroke(Path(rect), with: .color(.accentColor.opacity(0.9)), lineWidth: 1.5)
            }
            if showLines && block.isTextLike {
                context.stroke(Path(rect.insetBy(dx: 0.5, dy: 0.5)),
                               with: .color(.orange.opacity(0.95)),
                               style: StrokeStyle(lineWidth: 1, dash: [4, 3]))
            }
            if showOrder && block.order > 0 {
                let badge = Text("\(block.order)")
                    .font(.system(size: 9, weight: .bold))
                    .foregroundStyle(.white)
                context.fill(Path(roundedRect: CGRect(x: rect.minX, y: rect.minY - 13,
                                                      width: max(16, CGFloat("\(block.order)".count) * 7 + 8),
                                                      height: 13),
                                   cornerRadius: 3),
                             with: .color(.accentColor.opacity(0.9)))
                context.draw(badge, at: CGPoint(x: rect.minX + 8, y: rect.minY - 7))
            }
        }
    }

    private func hitTest(_ point: CGPoint, size: CGSize) -> RecognizedBlock? {
        let image = output.image.size
        guard image.width > 0, image.height > 0 else { return nil }
        let sx = size.width / image.width
        let sy = size.height / image.height
        // 小框（公式、页码）很难点中，命中区域外扩 6pt
        return output.blocks
            .map { block in
                (block, CGRect(x: block.bbox.minX * sx, y: block.bbox.minY * sy,
                               width: block.bbox.width * sx, height: block.bbox.height * sy).insetBy(dx: -6, dy: -6))
            }
            .filter { $0.1.contains(point) }
            .min { $0.1.width * $0.1.height < $1.1.width * $1.1.height }?
            .0
    }

    // MARK: - 手势

    private func panGesture(image size: CGSize) -> some Gesture {
        DragGesture()
            .onChanged { value in
                guard scale > 1.01 else { return }
                offset = CGSize(width: baseOffset.width + value.translation.width,
                                height: baseOffset.height + value.translation.height)
            }
            .onEnded { _ in
                offset = clamp(offset, scale: scale, image: size)
                baseOffset = offset
            }
    }

    private func magnifyGesture(image size: CGSize) -> some Gesture {
        MagnifyGesture()
            .onChanged { value in
                scale = min(max(baseScale * value.magnification, minScale), maxScale)
            }
            .onEnded { _ in
                baseScale = scale
                offset = clamp(offset, scale: scale, image: size)
                baseOffset = offset
            }
    }

    private func clamp(_ value: CGSize, scale: CGFloat, image size: CGSize) -> CGSize {
        let maxX = max(0, (size.width * scale - size.width) / 2)
        let maxY = max(0, (size.height * scale - size.height) / 2)
        return CGSize(width: min(max(value.width, -maxX), maxX),
                      height: min(max(value.height, -maxY), maxY))
    }

    // MARK: - 工具栏

    private var toolbar: some View {
        HStack(spacing: 8) {
            if toolbarOpen {
                chip("文本区域", isOn: showRegions) { showRegions.toggle() }
                chip("行", isOn: showLines) { showLines.toggle() }
                chip("阅读顺序", isOn: showOrder) { showOrder.toggle() }
            }
            Button {
                withAnimation(.snappy) { toolbarOpen.toggle() }
            } label: {
                Image(systemName: toolbarOpen ? "xmark" : "slider.horizontal.3")
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(.white)
                    .frame(width: 44, height: 44)
                    .background(Color.accentColor, in: Circle())
                    .shadow(radius: 8, y: 3)
            }
            .accessibilityLabel(toolbarOpen ? "收起叠加层工具栏" : "展开叠加层工具栏")
        }
    }

    private func chip(_ title: String, isOn: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 4) {
                Image(systemName: isOn ? "checkmark.circle.fill" : "circle")
                    .font(.system(size: 13))
                Text(title)
                    .font(.system(size: 13, weight: .medium))
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
            .background(.regularMaterial, in: Capsule())
            .overlay(Capsule().stroke(isOn ? Color.accentColor : Color.clear, lineWidth: 1))
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(isOn ? [.isSelected] : [])
    }
}
