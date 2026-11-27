//
//  RootView.swift
//  底部导航：识别 / 设置 / 日志。
//

import SwiftUI

struct RootView: View {
    var body: some View {
        TabView {
            RecognizeView()
                .tabItem {
                    Label("识别", systemImage: "camera.viewfinder")
                }
            SettingsView()
                .tabItem {
                    Label("设置", systemImage: "gearshape")
                }
            LogsView()
                .tabItem {
                    Label("日志", systemImage: "doc.text.magnifyingglass")
                }
        }
    }
}

#Preview {
    RootView()
        .environmentObject(ModelStore())
        .environmentObject(EngineHost())
}
