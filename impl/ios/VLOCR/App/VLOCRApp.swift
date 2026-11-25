//
//  VLOCRApp.swift
//  最小 VL 线部署示例：只有「识别」「设置」两个 tab，不做保存 / 项目 / 校对。
//

import SwiftUI

@main
struct VLOCRApp: App {

    @StateObject private var store = ModelStore()
    @StateObject private var host = EngineHost()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(store)
                .environmentObject(host)
        }
    }
}
