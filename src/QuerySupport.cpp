#include "QuerySupport.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <locale>
#include <sstream>
#include <string_view>

namespace aiapi {
namespace query {

bool JsonValue::Number(double& value) const {
    if (kind != Kind::Number) return false;
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    stream >> value;
    return stream && stream.peek() == std::char_traits<char>::eof() && std::isfinite(value);
}

bool JsonValue::String(std::string& value) const {
    if (kind != Kind::String) return false;
    value = text;
    return true;
}

class InternetHandle {
public:
    explicit InternetHandle(HINTERNET handle = nullptr) : handle_(handle) {}
    ~InternetHandle() { if (handle_) WinHttpCloseHandle(handle_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const { return handle_; }

private:
    HINTERNET handle_;
};
class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool Parse(JsonValue& value) {
        SkipWhitespace();
        if (!ParseValue(value, 0)) return false;
        SkipWhitespace();
        return position_ == input_.size();
    }

private:
    static constexpr unsigned kMaxDepth = 48;

    void SkipWhitespace() {
        while (position_ < input_.size()) {
            const char c = input_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++position_;
        }
    }

    bool Consume(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    bool ParseValue(JsonValue& value, unsigned depth) {
        if (depth > kMaxDepth || position_ >= input_.size()) return false;
        SkipWhitespace();
        if (position_ >= input_.size()) return false;
        switch (input_[position_]) {
        case '{': return ParseObject(value, depth + 1);
        case '[': return ParseArray(value, depth + 1);
        case '"':
            value.kind = JsonValue::Kind::String;
            return ParseString(value.text);
        case 't':
            if (!ConsumeLiteral("true")) return false;
            value.kind = JsonValue::Kind::Boolean;
            value.boolean = true;
            return true;
        case 'f':
            if (!ConsumeLiteral("false")) return false;
            value.kind = JsonValue::Kind::Boolean;
            value.boolean = false;
            return true;
        case 'n':
            if (!ConsumeLiteral("null")) return false;
            value.kind = JsonValue::Kind::Null;
            return true;
        default:
            return ParseNumber(value);
        }
    }

    bool ConsumeLiteral(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) return false;
        position_ += literal.size();
        return true;
    }

    static void AppendUtf8(std::string& output, unsigned int codepoint) {
        if (codepoint <= 0x7f) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }

    bool ReadHexQuad(unsigned int& value) {
        if (input_.size() - position_ < 4) return false;
        value = 0;
        for (unsigned int i = 0; i < 4; ++i) {
            const char c = input_[position_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned int>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool ParseString(std::string& output) {
        if (!Consume('"')) return false;
        output.clear();
        while (position_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[position_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                output.push_back(static_cast<char>(c));
                continue;
            }
            if (position_ >= input_.size()) return false;
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                unsigned int codepoint = 0;
                if (!ReadHexQuad(codepoint)) return false;
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (input_.size() - position_ < 6 || input_[position_] != '\\' || input_[position_ + 1] != 'u') return false;
                    position_ += 2;
                    unsigned int low = 0;
                    if (!ReadHexQuad(low) || low < 0xdc00 || low > 0xdfff) return false;
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    return false;
                }
                AppendUtf8(output, codepoint);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool ParseObject(JsonValue& value, unsigned depth) {
        if (!Consume('{')) return false;
        value.kind = JsonValue::Kind::Object;
        value.object.clear();
        SkipWhitespace();
        if (Consume('}')) return true;
        while (position_ < input_.size()) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWhitespace();
            if (!Consume(':')) return false;
            JsonValue member;
            if (!ParseValue(member, depth)) return false;
            value.object[std::move(key)] = std::move(member);
            SkipWhitespace();
            if (Consume('}')) return true;
            if (!Consume(',')) return false;
        }
        return false;
    }

    bool ParseArray(JsonValue& value, unsigned depth) {
        if (!Consume('[')) return false;
        value.kind = JsonValue::Kind::Array;
        value.array.clear();
        SkipWhitespace();
        if (Consume(']')) return true;
        while (position_ < input_.size()) {
            JsonValue item;
            if (!ParseValue(item, depth)) return false;
            value.array.push_back(std::move(item));
            SkipWhitespace();
            if (Consume(']')) return true;
            if (!Consume(',')) return false;
        }
        return false;
    }

    bool ParseNumber(JsonValue& value) {
        const size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) return false;
        if (input_[position_] == '0') {
            ++position_;
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') return false;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const size_t fractionStart = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            if (fractionStart == position_) return false;
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            const size_t exponentStart = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            if (exponentStart == position_) return false;
        }
        value.kind = JsonValue::Kind::Number;
        value.text.assign(input_.substr(start, position_ - start));
        double parsed = 0.0;
        std::istringstream stream(value.text);
        stream.imbue(std::locale::classic());
        stream >> parsed;
        return stream && stream.peek() == std::char_traits<char>::eof() && std::isfinite(parsed);
    }

    std::string_view input_;
    size_t position_ = 0;
};


const JsonValue* Member(const JsonValue* value, const char* name) {
    return value ? value->Find(name) : nullptr;
}

bool ReadNumber(const JsonValue* value, double& number) {
    if (value && value->Number(number)) return true;
    if (!value || value->kind != JsonValue::Kind::String) return false;
    std::istringstream stream(value->text);
    stream.imbue(std::locale::classic());
    stream >> number;
    return stream && stream.peek() == std::char_traits<char>::eof() && std::isfinite(number);
}

bool ReadString(const JsonValue* value, std::string& text) {
    return value && value->String(text);
}

bool ParseDocument(const std::string& body, JsonValue& root) {
    std::string_view json(body);
    if (json.size() >= 3 && static_cast<unsigned char>(json[0]) == 0xef &&
        static_cast<unsigned char>(json[1]) == 0xbb && static_cast<unsigned char>(json[2]) == 0xbf) {
        json.remove_prefix(3);
    }
    JsonParser parser(json);
    return parser.Parse(root) && root.kind == JsonValue::Kind::Object;
}

std::wstring WidenUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), length) != length) return {};
    return result;
}

std::string NarrowUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), length, nullptr, nullptr) != length) return {};
    return result;
}

// 从错误响应体里取一条可直接显示的简短原因（API 的 message 或 error.message）。
bool ExtractJsonMessage(const std::string& body, std::wstring& message) {
    JsonValue root;
    if (!ParseDocument(body, root)) return false;
    std::string text;
    if (!ReadString(Member(&root, "message"), text) || text.empty()) {
        const JsonValue* error = Member(&root, "error");
        if (!ReadString(Member(error, "message"), text) || text.empty()) return false;
    }
    if (text.size() > 200) return false;
    message = WidenUtf8(text);
    return !message.empty();
}


bool ReadBool(const JsonValue* value, bool& flag) {
    if (!value) return false;
    if (value->kind == JsonValue::Kind::Boolean) {
        flag = value->boolean;
        return true;
    }
    if (value->kind == JsonValue::Kind::Number) {
        double number = 0.0;
        if (!value->Number(number)) return false;
        flag = number != 0.0;
        return true;
    }
    if (value->kind == JsonValue::Kind::String) {
        std::string text;
        if (!value->String(text)) return false;
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (text == "true" || text == "1" || text == "yes") { flag = true; return true; }
        if (text == "false" || text == "0" || text == "no") { flag = false; return true; }
    }
    return false;
}

bool ReadInt(const JsonValue* value, long long& number) {
    double parsed = 0.0;
    if (!ReadNumber(value, parsed) || !std::isfinite(parsed)) return false;
    number = static_cast<long long>(parsed);
    return true;
}

HttpResponse SendHttp(const std::wstring& url, const HttpRequest& request) {
    HttpResponse result;
    if (url.empty()) {
        result.error = L"请求地址为空。";
        return result;
    }

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUserNameLength = static_cast<DWORD>(-1);
    components.dwPasswordLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components) ||
        (components.nScheme != INTERNET_SCHEME_HTTPS && components.nScheme != INTERNET_SCHEME_HTTP) ||
        components.dwHostNameLength == 0 || components.dwUserNameLength != 0 ||
        components.dwPasswordLength != 0) {
        result.error = L"请求地址无效；请使用不含用户凭据的 HTTP(S) URL。";
        return result;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTPS && !request.allowInsecureHttp) {
        result.error = L"请求必须使用 HTTPS。";
        return result;
    }

    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (path.empty()) path = L"/";
    if (components.dwExtraInfoLength > 0) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    const DWORD requestFlags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;

    InternetHandle session(WinHttpOpen(L"TrafficMonitor-AIAPIQuota/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.get()) {
        result.error = L"无法初始化 HTTP 客户端。";
        return result;
    }
    const int timeout = (std::max)(1000, request.timeoutMs);
    WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout);

    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
    if (!connection.get()) {
        result.error = L"无法连接站点。";
        return result;
    }
    const std::wstring method = request.method.empty() ? L"GET" : request.method;
    InternetHandle httpRequest(WinHttpOpenRequest(connection.get(), method.c_str(), path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, requestFlags));
    if (!httpRequest.get()) {
        result.error = L"无法创建 HTTP 请求。";
        return result;
    }

    const DWORD disabledFeatures = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    if (!WinHttpSetOption(httpRequest.get(), WINHTTP_OPTION_DISABLE_FEATURE,
        const_cast<DWORD*>(&disabledFeatures), sizeof(disabledFeatures))) {
        result.error = L"无法禁用重定向和 Cookie，已取消请求。";
        return result;
    }

    std::wstring headers = L"Accept: application/json\r\nCache-Control: no-store\r\nUser-Agent: cc-switch/1.0\r\n";
    for (const auto& [name, value] : request.headers) {
        if (name.empty() || name.find_first_of(L":\r\n") != std::wstring::npos ||
            value.find_first_of(L"\r\n") != std::wstring::npos) {
            result.error = L"请求头格式无效。";
            return result;
        }
        headers += name + L": " + value + L"\r\n";
    }
    const BOOL headersAdded = WinHttpAddRequestHeaders(httpRequest.get(), headers.c_str(),
        static_cast<DWORD>(headers.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));

    std::string bodyUtf8 = NarrowUtf8(request.body);
    const DWORD bodyLength = static_cast<DWORD>(bodyUtf8.size());
    const LPVOID bodyData = bodyLength == 0 ? WINHTTP_NO_REQUEST_DATA : bodyUtf8.data();
    if (!headersAdded ||
        !WinHttpSendRequest(httpRequest.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, bodyData, bodyLength,
            bodyLength, 0) ||
        !WinHttpReceiveResponse(httpRequest.get(), nullptr)) {
        result.error = L"网络请求失败。";
        SecureZeroMemory(bodyUtf8.data(), bodyUtf8.size());
        return result;
    }
    if (!bodyUtf8.empty()) SecureZeroMemory(bodyUtf8.data(), bodyUtf8.size());

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(httpRequest.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        result.error = L"无法读取 HTTP 状态。";
        return result;
    }
    result.status = statusCode;

    constexpr size_t kMaxResponseBytes = 512 * 1024;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(httpRequest.get(), &available)) {
            result.error = L"读取响应失败。";
            return result;
        }
        if (available == 0) break;
        if (result.body.size() + available > kMaxResponseBytes) {
            result.error = L"响应超过大小限制。";
            return result;
        }
        std::vector<char> buffer(std::min<DWORD>(available, 8192));
        DWORD bytesRead = 0;
        if (!WinHttpReadData(httpRequest.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead)) {
            result.error = L"读取响应失败。";
            return result;
        }
        if (bytesRead == 0) break;
        result.body.append(buffer.data(), bytesRead);
    }

    std::wstring responseMessage;
    const bool hasMessage = ExtractJsonMessage(result.body, responseMessage);
    if (statusCode == 401 || statusCode == 403) {
        result.error = L"认证失败 (HTTP " + std::to_wstring(statusCode) + L")";
        result.error += hasMessage ? L"：" + responseMessage : L"。";
        return result;
    }
    if (statusCode < 200 || statusCode >= 300) {
        result.error = L"站点返回 HTTP " + std::to_wstring(statusCode);
        result.error += hasMessage ? L"：" + responseMessage : L"。";
        return result;
    }
    result.success = true;
    return result;
}

bool IsChineseSystemLanguage() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
}

} // namespace query
} // namespace aiapi