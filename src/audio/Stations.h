// ============================================================================
//  Stations.h  --  电台表 = 内置节目（只读） + 自定义电台（LittleFS 文件持久化）
//  内置节目表编译在固件里；自定义电台由网页控制端添加/编辑/删除/批量导入，
//  存为文件 /stations.json（896KB 文件分区，可存数百个），掉电不丢。
//  首次启动自动把旧版 NVS 数据迁移到文件。索引存 NVS，掉电记住上次收听的台
// ============================================================================
#pragma once
#include <Arduino.h>

struct Station {
    const char* name;   // 电台名称
    const char* url;    // 播放地址，支持 http/https 的 mp3/aac 流，以及 m3u8/m3u/pls
    const char* note;   // 备注：编码 / 类型，纯调试信息
};

namespace Stations {

uint8_t      count();            // 内置 + 自定义 总数
uint8_t      builtinCount();     // 内置电台数（索引小于它的为内置，只读）
const Station& get(uint8_t idx);
void         printAll(int current);

// 索引持久化（NVS）
void    loadIndex(uint8_t& out);
void    saveIndex(uint8_t idx);

// 自定义电台（文件持久化，网页端管理）
// idx 均为"全局索引"（含内置电台的偏移）；失败时 err 返回给网页端显示的原因
void    loadCustom();                 // 挂载文件系统 + 读取（含 NVS 旧数据自动迁移）
bool    addCustom(const String& name, const String& url, String& err);
bool    editCustom(uint8_t idx, const String& name, const String& url, String& err);
bool    delCustom(uint8_t idx, String& err);
uint8_t maxCustom();

// 批量导入：文本每行一条，支持两种格式（可混用）：
//   1) "名称,URL"（英文/中文逗号均可）
//   2) M3U：#EXTINF:-1,名称 行后面跟 URL 行；纯 URL 行也可单独出现
// 返回成功加入条数；dup=重复地址跳过数；bad=格式/校验失败数；trunc=名称超长被截断数
int     importText(const String& text, int& dup, int& bad, int& trunc);

// 导出全部自定义电台为 M3U 文本（可存档，之后可直接再导入）
String  exportM3U();

}  // namespace Stations
