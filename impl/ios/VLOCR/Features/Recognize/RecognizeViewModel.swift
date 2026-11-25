//
//  RecognizeViewModel.swift
//  一次选图 → 逐页识别 → 展示结果。不做保存、不做项目，符合本 demo 的范围。
//

import Foundation
import UIKit

@MainActor
final class RecognizeViewModel: ObservableObject {

    enum Status {
        case pending
        case running(done: Int, total: Int)
        case done(PageOutput)
        case failed(String)
    }

    struct Item: Identifiable {
        let id = UUID()
        let image: UIImage
        let thumbnail: UIImage
        var status: Status = .pending
    }

    @Published private(set) var items: [Item] = []
    @Published private(set) var isRunning = false
    @Published var errorMessage: String?

    func recognize(_ images: [UIImage], store: ModelStore, host: EngineHost) {
        guard !images.isEmpty else { return }
        items = images.map { Item(image: $0, thumbnail: ImageTools.thumbnail($0, maxDimension: 320)) }
        errorMessage = nil
        isRunning = true
        Task { await run(store: store, host: host) }
    }

    func clear() {
        guard !isRunning else { return }
        items = []
        errorMessage = nil
    }

    private func run(store: ModelStore, host: EngineHost) async {
        defer { isRunning = false }
        do {
            let pipeline = try await host.makePipeline(for: store)
            for index in items.indices {
                items[index].status = .running(done: 0, total: 0)
                let image = items[index].image
                do {
                    // 版面分析是同步阻塞调用，整体挪到后台线程，主线程只更新进度。
                    let output = try await Task.detached(priority: .userInitiated) {
                        try await pipeline.recognize(image: image) { done, total in
                            Task { @MainActor in
                                guard index < self.items.count else { return }
                                self.items[index].status = .running(done: done, total: total)
                            }
                        }
                    }.value
                    items[index].status = .done(output)
                } catch {
                    items[index].status = .failed(error.localizedDescription)
                }
            }
        } catch {
            errorMessage = error.localizedDescription
            for index in items.indices {
                if case .pending = items[index].status {
                    items[index].status = .failed("未执行")
                }
            }
        }
    }
}
