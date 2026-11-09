#include "front/prefs.hpp"

#include "absl/base/no_destructor.h"

#include "config/preferences_store.hpp"

namespace prefs {

namespace {
w2::PreferencesStore& DefaultStore() {
    static absl::NoDestructor<w2::PreferencesStore> store(w2::PreferencesStore::ForUserApp());
    return *store;
}
}  // namespace

std::filesystem::path UserFile(const std::string& name) {
    return w2::PreferencesStore::UserFilePath(name);
}

std::string Get(const std::string& key) {
    return DefaultStore().Get(key);
}

void Set(const std::string& key, const std::string& value) {
    auto& store = DefaultStore();
    store.Set(key, value);
    store.Save().IgnoreError();
}

int GetInt(const std::string& key, int fallback) {
    return DefaultStore().GetInt(key, fallback);
}

}  // namespace prefs
