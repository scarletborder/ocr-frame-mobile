//
//  VLOCRApp.swift
//  最小 VL 线部署示例：底部三个 tab —— 识别 / 设置 / 日志，不做保存 / 项目 / 校对。
//
//  启动时就装好日志 sink，保证引擎创建、模型加载这些早期的输出也不会丢。
//

import SwiftUI

@main
struct VLOCRApp: App {

    @StateObject private var store = ModelStore()
    @StateObject private var host = EngineHost()

    init() {
        VLOCRLogging.install()
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(store)
                .environmentObject(host)
        }
    }
}
