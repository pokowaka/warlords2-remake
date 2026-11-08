// A lightweight JSON value type for saves and settings. Objects keep their
// keys in insertion order so serialized output is deterministic.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace w2 {

/**
 * @brief Order-preserving JSON value used for game saves and user preferences.
 */
class Json {
  public:
    /** @brief Discriminator for the active JSON value kind. */
    enum Type { kNull, kBool, kNumber, kString, kArray, kObject };

    Json() = default;
    // Implicit on purpose: a value converts to Json where one is expected, as
    // in obj.Set("turn", 3) or arr.Push("name").
    // NOLINTBEGIN(google-explicit-constructor)
    Json(std::nullptr_t) {}
    Json(bool val) : type_(kBool), bool_val_(val) {}
    Json(int n) : type_(kNumber), num_val_(n) {}
    Json(long n) : type_(kNumber), num_val_(static_cast<double>(n)) {}
    Json(double n) : type_(kNumber), num_val_(n) {}
    Json(const char* str) : type_(kString), str_val_(str) {}
    Json(std::string str) : type_(kString), str_val_(std::move(str)) {}
    // NOLINTEND(google-explicit-constructor)

    /** @brief Creates an empty JSON array. */
    static Json Array() {
        Json val;
        val.type_ = kArray;
        return val;
    }
    /** @brief Creates an empty JSON object. */
    static Json Object() {
        Json val;
        val.type_ = kObject;
        return val;
    }

    /** @brief Returns the value's JSON type. */
    Type GetType() const { return type_; }
    /** @brief True when this value is `null`. */
    bool IsNull() const { return type_ == kNull; }
    /** @brief True when this value is a number. */
    bool IsNumber() const { return type_ == kNumber; }
    /** @brief True when this value is a string. */
    bool IsString() const { return type_ == kString; }
    /** @brief True when this value is an array. */
    bool IsArray() const { return type_ == kArray; }
    /** @brief True when this value is an object. */
    bool IsObject() const { return type_ == kObject; }

    /** @brief Returns the boolean value (or non-zero number), falling back to `dflt`. */
    bool Boolean(bool dflt = false) const {
        return type_ == kBool ? bool_val_ : (type_ == kNumber ? num_val_ != 0 : dflt);
    }
    /** @brief Returns the floating-point value, falling back to `dflt`. */
    double Number(double dflt = 0) const { return type_ == kNumber ? num_val_ : dflt; }
    /** @brief Returns the integer value, falling back to `dflt`. */
    long Integer(long dflt = 0) const {
        return type_ == kNumber ? static_cast<long>(num_val_) : dflt;
    }
    /** @brief Returns the string value (empty if not a string). */
    const std::string& Str() const { return str_val_; }
    /** @brief Returns the string value, falling back to `dflt`. */
    std::string Str(const std::string& dflt) const { return type_ == kString ? str_val_ : dflt; }

    // Arrays
    /** @brief Returns the number of elements in an array or entries in an object. */
    size_t size() const {
        return type_ == kArray ? arr_val_.size() : type_ == kObject ? obj_val_.size() : 0;
    }
    /** @brief Returns array element `index`, or a null `Json` if out of bounds. */
    const Json& operator[](size_t index) const {
        return index < arr_val_.size() ? arr_val_[index] : None();
    }
    /** @brief Returns array element `index`, or a null `Json` if out of bounds. */
    const Json& operator[](int index) const {
        return index >= 0 ? (*this)[static_cast<size_t>(index)] : None();
    }
    /** @brief Appends `val` to this array and returns a reference to the stored element. */
    Json& Push(Json val) {
        type_ = kArray;
        arr_val_.push_back(std::move(val));
        return arr_val_.back();
    }
    /** @brief Returns the underlying array elements. */
    const std::vector<Json>& Items() const { return arr_val_; }

    // Objects
    /** @brief True when this object contains `key`. */
    bool Has(const std::string& key) const;
    /** @brief Looks up `key` in this object, returning a null `Json` when absent. */
    const Json& operator[](const std::string& key) const;
    /** @brief Looks up `key` in this object, returning a null `Json` when absent. */
    const Json& operator[](const char* key) const { return (*this)[std::string(key)]; }
    /** @brief Sets `key` to `val` (dropping null values, like `JSON.stringify` drops `undefined`).
     */
    Json& Set(const std::string& key, Json val);
    /** @brief Returns the ordered key/value entries of this object. */
    const std::vector<std::pair<std::string, Json>>& Entries() const { return obj_val_; }

    /** @brief Serializes this JSON value to a compact string. */
    std::string Dump() const;
    /** @brief Parses `text` as JSON, throwing `std::runtime_error` on syntax errors. */
    static Json Parse(const std::string& text);

  private:
    static const Json& None();
    void Write(std::string& out) const;

    Type type_ = kNull;                                  ///< Active JSON value type.
    bool bool_val_ = false;                              ///< Value when `type_ == kBool`.
    double num_val_ = 0;                                 ///< Value when `type_ == kNumber`.
    std::string str_val_;                                ///< Value when `type_ == kString`.
    std::vector<Json> arr_val_;                          ///< Elements when `type_ == kArray`.
    std::vector<std::pair<std::string, Json>> obj_val_;  ///< Entries when `type_ == kObject`.
};

}  // namespace w2
