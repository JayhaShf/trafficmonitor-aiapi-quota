#pragma once

// 各站点适配器共用的查询基础设施：JSON 解析、HTTP 请求、UTF-8 转换。
//
// 设计目标：让每个适配器模块（OfficialBalance / OfficialPlan / OfficialSubscription）
// 只关心「请求什么地址、怎么解析响应」，不用重复实现 JSON/HTTP 细节。
// 与 UsageApi.cpp 里旧适配器使用同一套解析语义（严格 JSON、UTF-8、错误消息提取）。

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace aiapi {
namespace query {

struct JsonValue {
    enum class Kind { Null, Boolean, Number, String, Object, Array } kind = Kind::Null;
    bool boolean = false;
    std::string text;
    std::map<std::string, JsonValue> object;
    std::vector<JsonValue> array;

    const JsonValue* Find(const char* name) const {
        if (kind != Kind::Object) return nullptr;
        const auto it = object.find(name);
        return it == object.end() ? nullptr : &it->second;
    }

    bool Number(double& value) const;
    bool String(std::string& value) const;
};

const JsonValue* Member(const JsonValue* value, const char* name);

// 数值也接受字符串形式的数字（不少接口把金额返回成字符串）。
bool ReadNumber(const JsonValue* value, double& number);
bool ReadString(const JsonValue* value, std::string& text);
bool ReadBool(const JsonValue* value, bool& flag);
bool ReadInt(const JsonValue* value, long long& number);

// 解析整个响应体为 JSON 对象（容忍 UTF-8 BOM）。
bool ParseDocument(const std::string& body, JsonValue& root);

std::wstring WidenUtf8(const std::string& text);
std::string NarrowUtf8(const std::wstring& text);

// 从错误响应体里提取可展示的简短原因（message / error.message）。
bool ExtractJsonMessage(const std::string& body, std::wstring& message);

// 一次 HTTP 请求。headers 会追加到默认的 Accept / User-Agent 之后；
// Authorization 等敏感头由调用方自己的 headers 提供。
struct HttpRequest {
    std::wstring method = L"GET";
    std::vector<std::pair<std::wstring, std::wstring>> headers;
    std::wstring body;                 // 请求体（仅 POST 使用）
    bool allowInsecureHttp = false;    // 仅用于本地 mock 测试
    int timeoutMs = 15000;
};

struct HttpResponse {
    bool success = false;
    unsigned int status = 0;
    std::string body;
    std::wstring error;
};

// 发送请求并读取响应体。2xx 返回 success=true；非 2xx/认证失败时 error 里
// 会带上 HTTP 状态和响应体里的 message（如果可解析）。
HttpResponse SendHttp(const std::wstring& url, const HttpRequest& request);

// Authorization: Bearer <token> 的便捷构造。
inline std::pair<std::wstring, std::wstring> BearerHeader(const std::wstring& token) {
    return { L"Authorization", L"Bearer " + token };
}

// 判断当前系统是否是中文区域（适配器可据此选择中文/英文错误文案）。
bool IsChineseSystemLanguage();

} // namespace query
} // namespace aiapi
