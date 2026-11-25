//
//  RootView.swift
//  底部导航：识别 / 设置。
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
        }
    }
}

#Preview {
    RootView()
        .environmentObject(ModelStore())
        .environmentObject(EngineHost())
}
