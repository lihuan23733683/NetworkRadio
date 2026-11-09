// ============================================================================
//  Stations.cpp
//  自定义电台存储：LittleFS 文件 /stations.json（原子写入），上限 MAX_CUSTOM_STATIONS
//  首次启动检测到旧版 NVS 数据时自动迁移
// ============================================================================
#include "Stations.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

#include "../core/AppConfig.h"

// ---------------------------------------------------------------------------
//  内置节目表（存 flash，只读）
// ---------------------------------------------------------------------------
static const Station kStations[] = {
    {"中国之声 CNR-1",  "http://ngcdn001.cnr.cn/live/zgzs/index.m3u8",  "HLS/AAC"},
    {"经济之声 CNR-2",  "http://ngcdn002.cnr.cn/live/jjzs/index.m3u8",  "HLS/AAC"},
    {"音乐之声 CNR-3",  "http://ngcdn001.cnr.cn/live/yyzs/index.m3u8",  "HLS/AAC"},
    {"SomaFM Groove",  "http://ice1.somafm.com/groovesalad-128-mp3",   "MP3 128k"},
    {"Radio Paradise",  "http://stream.radioparadise.com/mp3-128",      "MP3 128k"},
    {"Icecast 测试流",  "http://hestia2.cdnstream.com/1458_128",        "MP3 128k"},
};

static const uint8_t kCount = sizeof(kStations) / sizeof(kStations[0]);

// ---------------------------------------------------------------------------
//  自定义电台（PSRAM 优先动态分配，最多 MAX_CUSTOM_STATIONS 个）
// ---------------------------------------------------------------------------
static char    (*cName)[STA_NAME_MAX] = nullptr;
static char    (*cUrl)[STA_URL_MAX]   = nullptr;
static uint8_t cCount = 0;
static bool    fsOk   = false;

static const char* STATION_FILE = STA_FILE_PATH;   // 路径统一在 AppConfig.h 定义
static const char* STATION_TMP  = "/stations.tmp";

// ---------------------------------------------------------------------------
//  小工具：JSON 转义 / 解析（避免引入 ArduinoJson 依赖）
// ---------------------------------------------------------------------------
static String jsonEscape(const char* s) {
    String r;
    for (; *s; ++s) {
        char c = *s;
        if (c == '"' || c == '\\') { r += '\\'; r += c; }
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else r += c;
    }
    return r;
}

// 从 s[i] 开始读取一个 JSON 字符串（带 \" 转义还原），i 停在结束引号之后
static void jsonGetStr(const String& s, int& i, char* out, size_t cap) {
    out[0] = 0;
    int len = s.length();
    if (i >= len || s[i] != '"') return;
    ++i;
    size_t o = 0;
    while (i < len && s[i] != '"') {
        char c = s[i++];
        if (c == '\\' && i < len) {
            char e = s[i++];
            switch (e) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                default:   c = e;    break;
            }
        }
        if (o + 1 < cap) out[o++] = c;
    }
    out[o] = 0;
    if (i < len) ++i;   // 跳过结束引号
}

// ---------------------------------------------------------------------------
//  存储分配（PSRAM 优先）与文件读写
// ---------------------------------------------------------------------------
static bool ensureArrays() {
    if (cName && cUrl) return true;
    size_t bytes = (size_t)MAX_CUSTOM_STATIONS * (STA_NAME_MAX + STA_URL_MAX);
    void*  blk   = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!blk) blk = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (!blk) {
        Serial.printf("[存储] 电台缓冲分配失败（%u KB）\n", (unsigned)(bytes / 1024));
        return false;
    }
    cName = (char(*)[STA_NAME_MAX])blk;
    cUrl  = (char(*)[STA_URL_MAX])((char*)blk + (size_t)MAX_CUSTOM_STATIONS * STA_NAME_MAX);
    return true;
}

static bool parseJsonList(const String& j) {
    cCount = 0;
    int i = 0, len = j.length();
    while (i < len && cCount < MAX_CUSTOM_STATIONS) {
        int obj = j.indexOf('{', i);
        if (obj < 0) break;
        int nKey = j.indexOf("\"n\"", obj);
        int uKey = j.indexOf("\"u\"", obj);
        if (nKey < 0 || uKey < 0) break;
        int a = j.indexOf(':', nKey);
        int b = j.indexOf(':', uKey);
        if (a < 0 || b < 0) break;
        jsonGetStr(j, ++a, cName[cCount], STA_NAME_MAX);
        jsonGetStr(j, ++b, cUrl[cCount],  STA_URL_MAX);
        if (cName[cCount][0] && cUrl[cCount][0]) cCount++;
        i = obj + 1;
    }
    return true;
}

static bool saveCustom() {
    if (!fsOk) return false;
    String j = "[";
    for (uint8_t i = 0; i < cCount; ++i) {
        if (i) j += ",";
        j += "{\"n\":\"" + jsonEscape(cName[i]) + "\",\"u\":\"" + jsonEscape(cUrl[i]) + "\"}";
    }
    j += "]";

    // 原子写入：先写临时文件，成功后替换正式文件
    File f = LittleFS.open(STATION_TMP, "w");
    if (!f) { Serial.println("[存储] 打开临时文件失败"); return false; }
    size_t written = f.print(j);
    f.close();
    if (written != j.length()) {
        LittleFS.remove(STATION_TMP);
        Serial.println("[存储] 写入不完整，已放弃");
        return false;
    }
    LittleFS.remove(STATION_FILE);
    if (!LittleFS.rename(STATION_TMP, STATION_FILE)) {
        Serial.println("[存储] 文件替换失败");
        return false;
    }
    return true;
}

// 按字符边界截断 UTF-8 字符串到 maxBytes 字节以内（避免切断半个汉字）
static String fitUtf8(const String& s, size_t maxBytes) {
    if (s.length() <= maxBytes) return s;
    int cut = (int)maxBytes;
    while (cut > 0 && ((uint8_t)s[cut] & 0xC0) == 0x80) cut--;  // 回退到字符起始字节
    return s.substring(0, cut);
}

// ---------------------------------------------------------------------------
//  校验：名称 + 流媒体地址（协议白名单：HTTP / HTTPS）
//  注意：名称超长由 fitUtf8 截断（在调用方处理），这里不再拒收
// ---------------------------------------------------------------------------
static bool validate(const String& name, const String& url, String& err) {
    String n = name; n.trim();
    if (!n.length()) { err = "请填写电台名称"; return false; }

    String u = url; u.trim();
    if (!u.length()) { err = "请填写流媒体地址"; return false; }
    if (u.length() > STA_URL_MAX - 4) { err = "流媒体地址过长（最多 180 字符）"; return false; }

    String lu = u; lu.toLowerCase();
    if (!lu.startsWith("http://") && !lu.startsWith("https://")) {
        err = "不支持该流媒体协议：仅支持 http:// 或 https:// 开头的直播流"
              "（mp3 / aac / m3u8 / pls），rtmp / rtsp / udp 等无法播放";
        return false;
    }
    return true;
}

namespace Stations {

uint8_t count()        { return kCount + cCount; }
uint8_t builtinCount() { return kCount; }
uint8_t maxCustom()    { return MAX_CUSTOM_STATIONS; }

const Station& get(uint8_t idx) {
    static Station tmp;
    if (idx < kCount) return kStations[idx];
    if (idx < kCount + cCount) {
        tmp.name = cName[idx - kCount];
        tmp.url  = cUrl[idx - kCount];
        tmp.note = "自定义";
        return tmp;
    }
    if (kCount) return kStations[0];
    tmp.name = "空"; tmp.url = ""; tmp.note = "";
    return tmp;
}

void printAll(int current) {
    Serial.printf("\n电台列表（内置 %u + 自定义 %u = 共 %u 个）\n",
                  kCount, cCount, count());
    Serial.println("--------------------------------------------------------------");
    for (uint8_t i = 0; i < count(); i++) {
        const Station& s = get(i);
        Serial.printf(" %c %2u. %-16s %-9s\n", (i == current) ? '>' : ' ', i + 1,
                      s.name, s.note);
        Serial.printf("        %s\n", s.url);
    }
    Serial.println("--------------------------------------------------------------");
    Serial.println("切换：play <序号>   上一个/下一个：prev / next");
    Serial.println("自定义电台请在网页控制端添加/编辑/删除/批量导入");
}

void loadIndex(uint8_t& out) {
    Preferences p;
    p.begin(NVS_NS_APP, true);
    out = p.getUChar(NVS_KEY_STATION, 0);
    p.end();
    if (out >= count()) out = 0;
}

void saveIndex(uint8_t idx) {
    Preferences p;
    p.begin(NVS_NS_APP, false);
    p.putUChar(NVS_KEY_STATION, idx);
    p.end();
}

// ---------------------------------------------------------------------------
//  自定义电台：加载（含 NVS 旧数据迁移）
// ---------------------------------------------------------------------------
void loadCustom() {
    if (!ensureArrays()) return;

    fsOk = LittleFS.begin(true);   // 首次挂载失败会自动格式化（分区本来就是空的）
    if (!fsOk) {
        Serial.println("[存储] LittleFS 挂载失败，自定义电台不可用（播放不受影响）");
        return;
    }
    Serial.printf("[存储] LittleFS 已挂载，可用 %u KB\n",
                  (unsigned)(LittleFS.totalBytes() / 1024));

    // 已有文件：直接读
    if (LittleFS.exists(STATION_FILE)) {
        File f = LittleFS.open(STATION_FILE, "r");
        if (f) {
            String j = f.readString();
            f.close();
            parseJsonList(j);
            Serial.printf("[存储] 已读取自定义电台 %u/%u 个\n", cCount, MAX_CUSTOM_STATIONS);
            return;
        }
    }

    // 没有文件：尝试从旧版 NVS 迁移（v0.4.x 存在 radio-app/custom 键里）
    Preferences p;
    p.begin(NVS_NS_APP, true);
    String j = p.getString(NVS_KEY_CUSTOM, "");
    p.end();

    if (j.length() > 2) {
        parseJsonList(j);
        if (saveCustom()) {
            Serial.printf("[存储] 已从 NVS 迁移 %u 个自定义电台到文件（NVS 保留为备份）\n",
                          cCount);
            return;
        }
        Serial.println("[存储] 迁移写入失败（内存保留本次数据，重启重试）");
        return;
    }

    Serial.println("[存储] 无自定义电台数据，全新开始");
}

bool addCustom(const String& name, const String& url, String& err) {
    if (cCount >= MAX_CUSTOM_STATIONS) {
        err = "自定义电台已达上限（" + String(MAX_CUSTOM_STATIONS) + " 个），请先删除一些";
        return false;
    }
    if (!validate(name, url, err)) return false;

    String n = fitUtf8(name, STA_NAME_MAX - 2); n.trim();
    String u = url;  u.trim();
    for (uint8_t i = 0; i < cCount; ++i) {
        if (u == cUrl[i]) { err = "该流媒体地址已存在"; return false; }
    }

    strlcpy(cName[cCount], n.c_str(), STA_NAME_MAX);
    strlcpy(cUrl[cCount],  u.c_str(), STA_URL_MAX);
    cCount++;
    if (!saveCustom()) {
        cCount--;
        err = "写入设备存储失败，请重试";
        return false;
    }
    return true;
}

bool editCustom(uint8_t idx, const String& name, const String& url, String& err) {
    if (idx < kCount)              { err = "内置电台不可编辑"; return false; }
    if (idx >= kCount + cCount)    { err = "索引超范围";       return false; }
    if (!validate(name, url, err)) return false;

    uint8_t c = idx - kCount;
    String n = fitUtf8(name, STA_NAME_MAX - 2); n.trim();
    String u = url;  u.trim();
    for (uint8_t i = 0; i < cCount; ++i) {
        if (i != c && u == cUrl[i]) { err = "该流媒体地址已存在"; return false; }
    }

    strlcpy(cName[c], n.c_str(), STA_NAME_MAX);
    strlcpy(cUrl[c],  u.c_str(), STA_URL_MAX);
    if (!saveCustom()) { err = "写入设备存储失败，请重试"; return false; }
    return true;
}

bool delCustom(uint8_t idx, String& err) {
    if (idx < kCount)           { err = "内置电台不可删除"; return false; }
    if (idx >= kCount + cCount) { err = "索引超范围";       return false; }

    uint8_t c = idx - kCount;
    for (uint8_t i = c; i + 1 < cCount; ++i) {
        strlcpy(cName[i], cName[i + 1], STA_NAME_MAX);
        strlcpy(cUrl[i],  cUrl[i + 1],  STA_URL_MAX);
    }
    cCount--;
    if (!saveCustom()) { err = "写入设备存储失败，请重试"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
//  批量导入：支持 "名称,URL" 与 M3U 两种行格式，可混用
// ---------------------------------------------------------------------------
int importText(const String& text, int& dup, int& bad, int& trunc) {
    dup = 0; bad = 0; trunc = 0;
    int added = 0;
    String pendingName = "";
    uint8_t savedCount = cCount;   // 用于写盘失败时回滚

    int start = 0, len = text.length();
    while (start <= len && cCount < MAX_CUSTOM_STATIONS) {
        int eol = text.indexOf('\n', start);
        String line = (eol < 0) ? text.substring(start) : text.substring(start, eol);
        start = (eol < 0) ? len + 1 : eol + 1;

        line.trim();
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        line.trim();
        if (!line.length()) continue;

        if (line.startsWith("#EXTINF")) {
            // #EXTINF:-1,电台名称
            int c = line.indexOf(',');
            pendingName = (c >= 0) ? line.substring(c + 1) : String("");
            pendingName.trim();
            continue;
        }
        if (line.startsWith("#")) continue;   // #EXTM3U 等其他注释

        String name, url;
        if (pendingName.length()) {
            // M3U：上一行 #EXTINF 给出名称，本行是纯 URL
            name = pendingName;
            url  = line;
            pendingName = "";
        } else {
            // "名称,URL"（英文/中文逗号均可）；无逗号则整行是 URL
            int cA = line.indexOf(',');        // 英文逗号（1 字节）
            int cC = line.indexOf("，");       // 中文逗号（3 字节）
            int c  = -1;                        // 取更靠前的分隔符
            if      (cA >= 0 && cC >= 0) c = (cA < cC) ? cA : cC;
            else if (cA >= 0)            c = cA;
            else if (cC >= 0)            c = cC;
            if (c > 0) {
                name = line.substring(0, c);
                url  = (c == cC) ? line.substring(c + 3) : line.substring(c + 1);
            } else {
                url = line;
            }
        }
        name.trim();
        url.trim();
        if (!url.length()) continue;
        if (!name.length()) name = "电台 " + String(cCount + 1);
        String fitted = fitUtf8(name, STA_NAME_MAX - 2);
        if (fitted.length() < name.length()) trunc++;
        name = fitted;

        // 校验 + 去重，全部通过后在循环外统一写盘一次（批量导入不逐条写闪存）
        String err;
        if (!validate(name, url, err)) { bad++; continue; }

        bool exist = false;
        for (uint8_t i = 0; i < cCount; ++i) {
            if (url == cUrl[i]) { exist = true; break; }
        }
        if (exist) { dup++; continue; }

        strlcpy(cName[cCount], name.c_str(), STA_NAME_MAX);
        strlcpy(cUrl[cCount],  url.c_str(),  STA_URL_MAX);
        cCount++;
        added++;
    }

    if (added > 0) {
        if (!saveCustom()) {
            cCount = savedCount;   // 回滚
            bad += added;
            added = 0;
        }
    }
    return added;
}

String exportM3U() {
    String s = "#EXTM3U\n";
    for (uint8_t i = 0; i < cCount; ++i) {
        s += "#EXTINF:-1," + String(cName[i]) + "\n" + String(cUrl[i]) + "\n";
    }
    return s;
}

}  // namespace Stations
