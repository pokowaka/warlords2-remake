// A small JSON value, for saves and settings. Objects keep their keys in the
// order they were set, so what is written reads the same each time.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace w2 {

class Json {
  public:
    enum Type { kNull, kBool, kNumber, kString, kArray, kObject };

    Json() = default;
    // Implicit on purpose: a value converts to Json where one is expected, as
    // in obj.Set("turn", 3) or arr.Push("name").
    // NOLINTBEGIN(google-explicit-constructor)
    Json(std::nullptr_t) {}
    Json(bool b) : type_(kBool), b_(b) {}
    Json(int n) : type_(kNumber), n_(n) {}
    Json(long n) : type_(kNumber), n_(static_cast<double>(n)) {}
    Json(double n) : type_(kNumber), n_(n) {}
    Json(const char* s) : type_(kString), s_(s) {}
    Json(std::string s) : type_(kString), s_(std::move(s)) {}
    // NOLINTEND(google-explicit-constructor)

    static Json Array() {
        Json j;
        j.type_ = kArray;
        return j;
    }
    static Json Object() {
        Json j;
        j.type_ = kObject;
        return j;
    }

    Type GetType() const { return type_; }
    bool IsNull() const { return type_ == kNull; }
    bool IsNumber() const { return type_ == kNumber; }
    bool IsString() const { return type_ == kString; }
    bool IsArray() const { return type_ == kArray; }
    bool IsObject() const { return type_ == kObject; }

    bool Boolean(bool dflt = false) const {
        return type_ == kBool ? b_ : (type_ == kNumber ? n_ != 0 : dflt);
    }
    double Number(double dflt = 0) const { return type_ == kNumber ? n_ : dflt; }
    long Integer(long dflt = 0) const { return type_ == kNumber ? static_cast<long>(n_) : dflt; }
    const std::string& Str() const { return s_; }
    std::string Str(const std::string& dflt) const { return type_ == kString ? s_ : dflt; }

    // arrays
    size_t size() const { return type_ == kArray ? a_.size() : type_ == kObject ? o_.size() : 0; }
    const Json& operator[](size_t i) const { return i < a_.size() ? a_[i] : None(); }
    const Json& operator[](int i) const {
        return i >= 0 ? (*this)[static_cast<size_t>(i)] : None();
    }
    Json& Push(Json v) {
        type_ = kArray;
        a_.push_back(std::move(v));
        return a_.back();
    }
    const std::vector<Json>& Items() const { return a_; }

    // objects
    bool Has(const std::string& k) const;
    const Json& operator[](const std::string& k) const;
    const Json& operator[](const char* k) const { return (*this)[std::string(k)]; }
    /** Set a key (the value is dropped if null, as JSON.stringify drops undefined). */
    Json& Set(const std::string& k, Json v);
    const std::vector<std::pair<std::string, Json>>& Entries() const { return o_; }

    std::string Dump() const;
    /** Parse, or throw std::runtime_error. */
    static Json Parse(const std::string& text);

  private:
    static const Json& None();
    void Write(std::string& out) const;

    Type type_ = kNull;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Json> a_;
    std::vector<std::pair<std::string, Json>> o_;
};

}  // namespace w2
