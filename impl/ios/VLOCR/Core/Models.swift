//
//  Models.swift
//  iOS 侧的展示用值对象。VL 线规则（标签表、提示词、阅读顺序、Markdown）
//  全部在 impl/shard 的共享核心里，这里不允许再出现第二份。
//

import CoreGraphics
import Foundation
import UIKit

struct VLError: LocalizedError {
    let message: String
    var errorDescription: String? { message }

    init(_ message: String) {
        self.message = message
    }
}

/// 预设信息由共享核心提供（vlcore_preset_*），避免两端各写一份。
enum VLPreset {
    static let name = VLOCREngine.presetName()
    static let version = VLOCREngine.presetVersion()
    static let pipelineVersion = VLOCREngine.pipelineVersion()
    static let maxNewTokens = VLOCREngine.maxNewTokens()
}

/// 单块识别结果。label 是官方 25 类标签之一（text / table / display_formula …）。
struct RecognizedBlock: Identifiable, @unchecked Sendable {
    let id = UUID()
    let label: String
    let bbox: CGRect
    let order: Int
    let score: Float
    let text: String
    let recognized: Bool
}

struct PageOutput: Identifiable, @unchecked Sendable {
    let id = UUID()
    let image: UIImage
    let markdown: String
    let blocks: [RecognizedBlock]
    let seconds: TimeInterval
}
