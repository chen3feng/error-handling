#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// 尝试山寨一下 rust 里的 std::Result 错误处理机制
// 一个 Result 对象要么含有有个有效的 Value，要么只包含一个 Error

template <typename Type>
struct ErrorCodeTraits {
    static const char* Name();
    static const char* ToString(Type vale);
};

class ErrorMeta {
public:
    virtual const char* Name() const;
    virtual const char* ToString(int vale) const;
};

struct SourceLocation {
    const char* file;
    int line;
    const char* function;
};


class ErrorImpl {
public:
    ErrorImpl(int code, const char* file, int line, const char* function)
        : code_(code), file_(file), line_(line), function_(function) {
    }
    virtual ~ErrorImpl() = default;
    const char* File() const { return file_; }
    int Line() const { return line_; }
    const char* Function() const { return function_; }
    int Code() const { return code_; }
    virtual const ErrorImpl* Cause() const { return nullptr; }
private:
    int code_;
    const char* file_;
    int line_;
    const char* function_;
};

// 放一些共用的成员函数
class BaseError {
protected:
    BaseError() {}
    BaseError(int code, const char* file, int line, const char* function)
        : error_{std::make_shared<ErrorImpl>(code, file, line, function)} {
    }

    int RawCode() const {
        if (!error_) return 0;
        return error_->Code();
    }

public:
    explicit operator bool() const {
        return error_ && int(error_->Code()) != 0;
    }
    bool operator!() const {
        return !static_cast<bool>(*this);
    }

    std::string Message() const {
        // TODO: uses ErrorCodeTraits
        return "";
    }

    const char* File() const { return error_->File(); }
    int Line() const { return error_->Line(); }
    const char* Function() const { return error_->Function(); }

    std::vector<ErrorImpl*> Stack() const;

private:
    std::shared_ptr<ErrorImpl> error_;
};

// 对特定枚举错误码类型的包装，支持作为 bool 来检测以及转字符串，发生位置等便利操作。
// 此处用了 GCC 扩展的 __builtin_FILE等，c++2a 的 source_lication 可能更合适。
template <typename ErrorCode>
class TypedError : public BaseError {
public:
    TypedError() {}
    TypedError(ErrorCode code,
               const char* file = __builtin_FILE(), int line = __builtin_LINE(),
               const char* function = __builtin_FUNCTION())
        : BaseError((int)code, file, line, function) {
    }

    template <typename CauseError>
    TypedError(ErrorCode code, CauseError cause,
               const char* file = __builtin_FILE(), int line = __builtin_LINE(),
               const char* function = __builtin_FUNCTION());

    ErrorCode Code() const {
        return static_cast<ErrorCode>(RawCode());
    }
};

// 能兼容一切错误的错误
class GenericError : public BaseError {
public:
    GenericError() {}

    GenericError(int code,
                 const char* file = __builtin_FILE(), int line = __builtin_LINE(),
                 const char* function = __builtin_FUNCTION())
        : BaseError((int)code, file, line, function) {
    }

    template <typename CauseError>
    GenericError(int code, CauseError cause, const char* file = __builtin_FILE(), int line = __builtin_LINE());

    template <typename ErrorType>
    GenericError(ErrorType error) : BaseError(error) {
    }

    int Code() const {
        return RawCode();
    }
};

// Result 类，要么含有一个有效值，要么含有一个错误的特殊对象。
// 用于做可能出错的函数返回值，代替把正常值域里的某些特殊返回值作为错误
// （比如常见的查找下标返回-1表示不存在等）或者抛出异常的错误处理办法。
// 用法参见下面示例。
// TODO: 支持 move
template <typename T, typename ErrorType = GenericError>
class [[nodiscard]] Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(ErrorType error) : error_(std::move(error)) {}
    Result(const Result& src) : error_(std::move(src.error_)) {
        if (!error_)
            new(&value_) T(src.value_);
    }

    template <typename ErrorType2>
    Result(ErrorType2 error, std::enable_if<std::is_same<ErrorType, GenericError>::value, void>* = nullptr)
        : error_(error) {
    }

    ~Result() {
        if (OK()) {
            value_.~T();
        }
    }

    T* operator->() const {
        return &value_;
    }

    const T& Value() const {
        return value_;
    }
    T& Value() {
        return value_;
    }

    // 如果当前结果是错误，返回默认值
    T ValueOr(T default_value) const {
        if (OK()) return Value();
        return default_value;
    }

    // 返回是否是成功
    bool OK() const {
        return !error_;
    }
    const ErrorType& Error() const {
        return error_;
    }
private:
    // 用 union 避免自动构造和析构，确保有错误时对象不构造
    union {
        T value_;
    };
    ErrorType error_;
};

// Void 返回值的偏特化，和普通的比缺少部分成员函数。
template <typename ErrorType>
class [[nodiscard]] Result<void, ErrorType> {
public:
    Result() {}
    Result(ErrorType error) :error_(std::move(error)) {}

    template <typename ErrorType2>
    Result(ErrorType2 error, std::enable_if<std::is_same<ErrorType, GenericError>::value, void>* = nullptr)
    : error_(error) {
      }

    void IgnoreError() const {}

    bool OK() const {
        return !error_;
    }
private:
    ErrorType error_;
};

// 用于产生成功 Result<void> 类型的辅助函数
Result<void> OK() {
    return {};
}

// 支持嵌套错误，尚未实现
template <typename ErrorCode, typename ErrorType>
Result<void, ErrorCode> WrapError(ErrorCode code, ErrorCode cause) {
    return Result<void, ErrorCode>(code, cause);
}

// 也是模仿 Rust 的 TRY 宏，遇到表达式的值为错误时，自动从当前函数退出，返回错误
// 无错误时，则返回表达式的值。具体参见下面的例子。
//
// 这里的实现还有几个问题：
//   TRY 这个名字太短非常容易冲突，显然不适合正式代码，这里仅用于演示
//   实现依赖了 GCC 的非标准扩展“语句表达式”，不可移植
//   Result<void> 无返回值的情况需要处理
#define TRY(stmt) ({ \
    auto&& result = stmt; \
    if (!result.OK()) return result.Error(); \
    std::move(result).Value(); \
})

//////////////////////////////////////////////////////////
// 以下为演示兼测试代码
//
#include <stdio.h>
#include <limits.h>
#include <iostream>
#include <map>

enum class ErrnoType {
};

using ErrnoError = TypedError<ErrnoType>;

static const std::map<std::string, std::string> file_content = {
    {"number", "100"},
    {"bad", "bad"},
    {"empty", ""},
};

// 测试用的假“文件”桩类
class File {
public:
    File(std::string name) : name_(std::move(name)) {}
    Result<std::string, ErrnoError> Read() const {
        return file_content.at(name_);
    }
private:
    std::string name_;
};

// 用于打开假文件的假函数
Result<File, ErrnoError> OpenFile(const std::string& name) {
    if (file_content.count(name) != 0)
        return File{name};
    return ErrnoError(ErrnoType(EEXIST));
}

Result<int, ErrnoError> ParseInt(const std::string& s) {
    errno = 0;
    char* end = const_cast<char*>(s.c_str());
    long n = strtol(s.c_str(), &end, 0);
    if (errno == 0) {
        if (*end != '\0') {
            errno = EINVAL;
        } else if (n > INT_MAX || n < INT_MIN) {
            errno = ERANGE;
        } else {
            return static_cast<int>(n);
        }
    }
    return ErrnoError(ErrnoType(errno));
}

// 从文件读取一个整数，用于演示 TRY 的用法
Result<int> GetIntFromFile(const std::string& filename) {
    // 下面每一步的都依赖上一步操作的结果。
    // TRY 遇到错误就会自动从当前函数返回，否则提取出真正的返回值。
    // 和错误码比，错误不能被无意中忽略；
    // 和异常比，代码更显式看出可能会出错，也一样能自动传播错误，但是代价较低，可控。
    auto&& f = TRY(OpenFile(filename));
    auto&& s = TRY(f.Read());
    auto&& n = TRY(ParseInt(s));
    return n;
}

int ParseInt(const std::string& str, int default_value) {
    // 返回失败时，ValueOr 函数允许指定一个替代值
    return ParseInt(str).ValueOr(default_value);
}


enum class DnsErrorCode {};
using DnsError = TypedError<DnsErrorCode>;

// 一个无返回值的测试函数
Result<void> FlushAll() {
    return OK();
}

int main() {
    auto r = GetIntFromFile("bad");
    if (r.Error()) {
        std::cout
            << "In " << r.Error().File() << ":" << r.Error().Line() << ":" << r.Error().Function()
            << " Code: " << r.Error().Code() << '\n';
    }
    std::cout << r.ValueOr(-1) << '\n';
    std::cout << GetIntFromFile("number").Value() << '\n';

    // 可以显式地忽略错误，如果不加这个，Result 定义上的 [[nodiscard]] 属性会导致编译器警告，提醒开发者。
    FlushAll().IgnoreError();
}

/*
   enum class HttpErrorCode {
   OK = 200,
   FORBIDDEN = 403,
   NOT_FOUND = 404,
   };

   using HttpError = TypedError<HttpErrorCode>;

   class HttpRequest {};
   class HttpResponse {};

   Result<HttpResponse, HttpError> Download(const std::string& url) {
   if (true)
   return HttpResponse{};
   return HttpError(HttpErrorCode::NOT_FOUND);
   }

//////////////////////////////////////////////////////////

enum class RpcErrorCode {
SUCCESS = 0,
FAILED = 1,
};

using RpcError = TypedError<RpcErrorCode>;

template <typename Request, typename Response>
Result<Response, RpcError> Call(const Request&);

DownloadError e;
if (e.Is<DnsError>()) {
}

if (DnsError de; e.As(&de)) {
de;
}
*/
