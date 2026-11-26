//
//  VLPipeline.swift
//  两阶段编排全部在 impl/shard（C++），Swift 只负责：
//    1. UIImage → CGImage（方向归一化）；
//    2. 在串行队列上调用引擎（阻塞式）；
//    3. 把引擎返回的 JSON 解码成展示用模型。
//

import CoreGraphics
import Foundation
import UIKit

/// 共享核心返回的 JSON 结构（见 impl/shard/src/vlcore_c.cpp）。
private struct VLPageDTO: Decodable {

    struct Block: Decodable {
        let label: String
        let order: Int
        let score: Float
        let bbox: [Double]
        let text: String
        let recognized: Bool
        let error: String?
    }

    let markdown: String
    let blocks: [Block]
    let seconds: Double
}

final class VLPipeline: @unchecked Sendable {

    private let engine: VLOCREngine
    private let queue = DispatchQueue(label: "ai.ocrframe.vl.pipeline", qos: .userInitiated)

    init(engine: VLOCREngine) {
        self.engine = engine
    }

    func recognize(image: UIImage,
                   progress: ((Int, Int) -> Void)? = nil) async throws -> PageOutput {
        guard let cgImage = ImageTools.normalizedCGImage(from: image) else {
            throw VLError("图片无法解码，请重新拍摄或选择。")
        }

        let json = try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<String, Error>) in
            queue.async {
                do {
                    let text = try self.engine.runJSON(cgImage) { done, total in
                        progress?(done, total)
                    }
                    continuation.resume(returning: text)
                } catch {
                    continuation.resume(throwing: error)
                }
            }
        }

        guard let data = json.data(using: .utf8) else {
            throw VLError("识别结果编码异常。")
        }
        let page: VLPageDTO
        do {
            page = try JSONDecoder().decode(VLPageDTO.self, from: data)
        } catch {
            throw VLError("识别结果解析失败：\(error.localizedDescription)")
        }

        let blocks = page.blocks.map { block -> RecognizedBlock in
            let x1 = block.bbox.count > 0 ? block.bbox[0] : 0
            let y1 = block.bbox.count > 1 ? block.bbox[1] : 0
            let x2 = block.bbox.count > 2 ? block.bbox[2] : 0
            let y2 = block.bbox.count > 3 ? block.bbox[3] : 0
            return RecognizedBlock(label: block.label,
                                   bbox: CGRect(x: x1, y: y1, width: max(0, x2 - x1), height: max(0, y2 - y1)),
                                   order: block.order,
                                   score: block.score,
                                   text: block.text,
                                   recognized: block.recognized,
                                   error: block.error ?? "",
                                   regionType: VLOCREngine.regionType(forLabel: block.label))
        }
        return PageOutput(image: image, markdown: page.markdown, blocks: blocks, seconds: page.seconds)
    }
}
