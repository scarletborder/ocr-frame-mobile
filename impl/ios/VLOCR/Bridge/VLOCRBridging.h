//
//  VLOCRBridging.h
//  Swift ↔ ObjC++ 桥接头。
//
//  除平台门面 VLOCREngine 外，这里只额外暴露共享核心的 C 门面 vlcore_c.h：
//  Swift 侧的「日志」页需要直接安装日志 sink（vlcore_set_log），
//  其余核心逻辑仍然只走 VLOCREngine，不把 C++ 头文件泄给 Swift。
//

#import "VLOCREngine.h"
#import "vlcore/vlcore_c.h"
